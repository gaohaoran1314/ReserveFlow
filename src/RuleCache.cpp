// =============================================================================
// src/RuleCache.cpp  —  规则快照缓存实现（ARCH-P3 §7.2 / §1.3 / D-P3-10）
// =============================================================================
#include "RuleCache.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf {

namespace {

/** 按 (priority, rule_code) 升序：保证求值顺序确定（D-P3-4 的"平局取 priority 小者"依赖它） */
void sortRules(std::vector<Rule>* rules) {
    std::stable_sort(rules->begin(), rules->end(), [](const Rule& a, const Rule& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.ruleCode < b.ruleCode;
    });
}

}  // namespace

RuleCache::RuleCache(DbPool& pool, repo::RuleRepo& repo, int cacheTtlSec)
    : pool_(pool), repo_(repo), cacheTtlSec_(cacheTtlSec > 0 ? cacheTtlSec : 5) {}

bool RuleCache::init(std::string& err) {
    return reload(err);
}

bool RuleCache::reload(std::string& err) {
    err.clear();
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        err = "获取数据库连接超时，无法载入规则";
        std::unique_lock<std::shared_mutex> wl(mu_);
        degraded_ = true;
        return false;
    }

    std::vector<std::string> skipped;
    auto rulesRes = repo_.loadAll(*guard, &skipped);
    if (!rulesRes.ok()) {
        err = rulesRes.msg;
        std::unique_lock<std::shared_mutex> wl(mu_);
        degraded_ = true;   // 保留旧快照（若有），只标脏
        return false;
    }
    auto fpRes = repo_.fingerprint(*guard);
    if (!fpRes.ok()) {
        err = fpRes.msg;
        std::unique_lock<std::shared_mutex> wl(mu_);
        degraded_ = true;
        return false;
    }

    std::vector<Rule> rules = std::move(rulesRes.data);
    sortRules(&rules);

    {
        std::unique_lock<std::shared_mutex> wl(mu_);
        rules_        = std::move(rules);
        fp_           = fpRes.data;
        lastCheck_    = std::chrono::steady_clock::now();
        lastLoadedAt_ = TimeUtil::nowString();
        lastSkipped_  = skipped;
        degraded_     = false;
        loaded_       = true;
    }

    if (!skipped.empty()) {
        for (const auto& s : skipped) {
            LOG_WARN("载入规则时跳过一条畸形规则：{}", s);
        }
    }
    LOG_INFO("规则缓存已载入：{} 条（启用 {} 条），指纹 count={} enabledSum={}", size(),
             static_cast<int>(std::count_if(rules_.begin(), rules_.end(),
                                            [](const Rule& r) { return r.enabled; })),
             fp_.count, fp_.enabledSum);
    return true;
}

bool RuleCache::ttlCheck() {
    // 快路径：读锁下判断是否还在 TTL 内
    {
        std::shared_lock<std::shared_mutex> rl(mu_);
        if (loaded_ && std::chrono::steady_clock::now() - lastCheck_ <
                           std::chrono::seconds(cacheTtlSec_)) {
            return true;
        }
    }

    // 慢路径：写锁 + 双检 + 指纹查询
    std::unique_lock<std::shared_mutex> wl(mu_);
    if (loaded_ && std::chrono::steady_clock::now() - lastCheck_ <
                       std::chrono::seconds(cacheTtlSec_)) {
        return true;
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        // 拿不到连接不算「表变了」，也不把旧快照打成 degraded（可用性优先）
        LOG_WARN("规则指纹校验：获取数据库连接超时，本轮跳过（继续用现有快照）");
        return loaded_;
    }
    auto fpRes = repo_.fingerprint(*guard);
    if (!fpRes.ok()) {
        LOG_WARN("规则指纹校验失败：{}（继续用现有快照）", fpRes.msg);
        return loaded_;
    }

    lastCheck_ = std::chrono::steady_clock::now();
    if (fpRes.data == fp_) {
        return true;   // 无变化
    }

    // 有变化：重载（写锁已持有，直接内联载入避免重复加锁）
    std::vector<std::string> skipped;
    auto rulesRes = repo_.loadAll(*guard, &skipped);
    if (!rulesRes.ok()) {
        LOG_WARN("规则重载失败：{}（保留旧快照）", rulesRes.msg);
        degraded_ = true;
        return true;
    }
    std::vector<Rule> rules = std::move(rulesRes.data);
    sortRules(&rules);
    rules_        = std::move(rules);
    fp_           = fpRes.data;
    lastLoadedAt_ = TimeUtil::nowString();
    lastSkipped_  = skipped;
    degraded_     = false;
    loaded_       = true;
    LOG_INFO("规则缓存已按指纹变化重载：{} 条", rules_.size());
    return true;
}

bool RuleCache::snapshot(std::vector<Rule>* out, bool onlyEnabled) {
    if (out == nullptr) {
        return false;
    }
    out->clear();

    if (!loaded_) {
        // 冷启动没载入成功 → 尝试一次；再失败就是真 degraded
        std::string err;
        if (!reload(err)) {
            return false;
        }
    } else {
        (void)ttlCheck();   // 失败也继续用旧快照（见 ttlCheck 内的注释）
    }

    std::shared_lock<std::shared_mutex> rl(mu_);
    out->reserve(rules_.size());
    for (const auto& r : rules_) {
        if (onlyEnabled && !r.enabled) {
            continue;
        }
        out->push_back(r);
    }
    return true;
}

int RuleCache::paramInt(const std::string& ruleCode, const char* key, int fallback) const {
    const Fact v = [&]() -> Fact {
        std::shared_lock<std::shared_mutex> rl(mu_);
        for (const auto& r : rules_) {
            if (r.ruleCode != ruleCode) {
                continue;
            }
            const auto it = r.params.is_object() ? r.params.find(key) : r.params.end();
            if (it != r.params.end() && it->is_number()) {
                return Fact::of(it->get<double>());
            }
            return Fact::ofNull();
        }
        return Fact::ofNull();
    }();
    if (v.isNull()) {
        LOG_WARN("规则 {} 缺少参数 {}，回落到默认值 {}", ruleCode, key, fallback);
        return fallback;
    }
    return static_cast<int>(v.asNum());
}

double RuleCache::paramDouble(const std::string& ruleCode, const char* key,
                             double fallback) const {
    const Fact v = [&]() -> Fact {
        std::shared_lock<std::shared_mutex> rl(mu_);
        for (const auto& r : rules_) {
            if (r.ruleCode != ruleCode) {
                continue;
            }
            const auto it = r.params.is_object() ? r.params.find(key) : r.params.end();
            if (it != r.params.end() && it->is_number()) {
                return Fact::of(it->get<double>());
            }
            return Fact::ofNull();
        }
        return Fact::ofNull();
    }();
    if (v.isNull()) {
        LOG_WARN("规则 {} 缺少参数 {}，回落到默认值 {}", ruleCode, key, fallback);
        return fallback;
    }
    return v.asNum();
}

std::string RuleCache::paramStr(const std::string& ruleCode, const char* key,
                                const std::string& fallback) const {
    std::shared_lock<std::shared_mutex> rl(mu_);
    for (const auto& r : rules_) {
        if (r.ruleCode != ruleCode) {
            continue;
        }
        const auto it = r.params.is_object() ? r.params.find(key) : r.params.end();
        if (it != r.params.end() && it->is_string()) {
            return it->get<std::string>();
        }
        break;
    }
    LOG_WARN("规则 {} 缺少参数 {}，回落到默认值 {}", ruleCode, key, fallback);
    return fallback;
}

RuleCache::Fingerprint RuleCache::fingerprint() const {
    std::shared_lock<std::shared_mutex> rl(mu_);
    return fp_;
}

size_t RuleCache::size() const {
    std::shared_lock<std::shared_mutex> rl(mu_);
    return rules_.size();
}

std::string RuleCache::lastLoadedAt() const {
    std::shared_lock<std::shared_mutex> rl(mu_);
    return lastLoadedAt_;
}

bool RuleCache::degraded() const {
    std::shared_lock<std::shared_mutex> rl(mu_);
    return degraded_;
}

std::vector<std::string> RuleCache::lastSkipped() const {
    std::shared_lock<std::shared_mutex> rl(mu_);
    return lastSkipped_;
}

} // namespace rf
