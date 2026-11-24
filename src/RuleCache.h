// =============================================================================
// src/RuleCache.h  —  规则表快照缓存（ARCH-P3 §7.2 / D-P3-10）
//
// 线程安全模型：`shared_mutex` —— 求值侧（HTTP 工作线程 + 维护线程）持读锁，
// 重载持写锁。读多写极少，所以用 shared_mutex 而不是普通 mutex。
//
// 一致性策略（§1.3）：
//   · 管理员改规则 → 主动 reload()（延迟 0）
//   · 有人直接改 DB → 每 cacheTtlSec 做一次轻量指纹查询（延迟 ≤ TTL）
//   · reload 失败 → **保留旧快照**（宁可跑旧规则，也不要因为一次查询抖动把引擎打成 degraded）
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "RuleTypes.h"
#include "RuleRepo.h"   // 需要 repo::RuleFingerprint 的完整定义

namespace rf {

class RuleCache {
public:
    RuleCache(DbPool& pool, repo::RuleRepo& repo, int cacheTtlSec = 5);

    /** 启动时调用；失败返回 false（服务仍可启动，引擎进入 degraded 模式） */
    bool init(std::string& err);

    /**
     * 取当前**启用**的规则快照（已按 priority 升序拷贝）。
     * 内部先做 TTL 指纹校验（每 cacheTtlSec 一次）。
     * @return false 表示规则表不可读（调用方按 degraded 处理 → 走 P2 兜底）
     */
    bool snapshot(std::vector<Rule>* out, bool onlyEnabled = true);

    /** 强制重载（规则管理接口写成功后调用）；失败时保留旧快照 */
    bool reload(std::string& err);

    /**
     * 取指定规则的参数（D-P3-11：时间窗口 / 分值的唯一真相）。
     * 规则不存在 / 参数缺失 → 返回 fallback 并记 WARN（**不抛异常**）。
     */
    int         paramInt(const std::string& ruleCode, const char* key, int fallback) const;
    double      paramDouble(const std::string& ruleCode, const char* key, double fallback) const;
    std::string paramStr(const std::string& ruleCode, const char* key,
                         const std::string& fallback) const;

    using Fingerprint = repo::RuleFingerprint;

    Fingerprint fingerprint() const;
    size_t      size() const;
    std::string lastLoadedAt() const;
    bool        degraded() const;

    /** 最近一次载入时被跳过的坏规则摘要（日志/排查用） */
    std::vector<std::string> lastSkipped() const;

private:
    bool loadLocked(std::string& err);
    /** TTL 到点则做一次指纹校验；返回 false 表示**表不可读**（与"没变化"区分开） */
    bool ttlCheck();

    DbPool&         pool_;
    repo::RuleRepo& repo_;
    int             cacheTtlSec_ = 5;

    mutable std::shared_mutex              mu_;
    std::vector<Rule>                      rules_;   ///< 全量（含停用），已按 priority 排序
    Fingerprint                            fp_;
    std::chrono::steady_clock::time_point  lastCheck_{};
    std::string                            lastLoadedAt_;
    std::vector<std::string>               lastSkipped_;
    bool                                   degraded_ = false;
    bool                                   loaded_   = false;
};

} // namespace rf
