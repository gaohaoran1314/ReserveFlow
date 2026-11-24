// =============================================================================
// src/RuleEngine.cpp  —  三阶段求值 / 多命中裁决 / fail 策略（ARCH-P3 §5）
//
// 求值顺序与确定性完全由「已排序规则 + 首个命中短路 + 分组取最强」三件事决定，
// 没有任何随机性，也不依赖容器遍历顺序（用 std::map 做分组，键有序）。
// =============================================================================
#include "RuleEngine.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"
#include "ConditionEval.h"

namespace rf {

// ============================================================================
// ResolvedAction / EvalResult
// ============================================================================
nlohmann::json ResolvedAction::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["ruleCode"] = ruleCode;
    j["type"]     = toStr(type);
    if (!violationType.empty()) {
        j["violationType"] = violationType;
    }
    if (!transitEvent.empty()) {
        j["event"] = transitEvent;
    }
    if (!targetStatus.empty()) {
        j["targetStatus"] = targetStatus;
    }
    if (deductScore != 0) {
        j["deductScore"] = deductScore;
    }
    if (deductCredit) {
        j["deductCredit"] = true;
    }
    if (releaseSlots) {
        j["releaseSlots"] = true;
    }
    if (ratio != 0.0) {
        j["ratio"] = ratio;
    }
    if (maxAmount != 0.0) {
        j["maxAmount"] = maxAmount;
    }
    if (rate != 0.0) {
        j["rate"] = rate;
    }
    if (!amountFrom.empty()) {
        j["amountFrom"] = amountFrom;
        j["amount"]     = amount;
    }
    return j;
}

bool EvalResult::hasAction(ActionType t) const {
    for (const auto& a : actions) {
        if (a.type == t) {
            return true;
        }
    }
    return false;
}

const ResolvedAction* EvalResult::findAction(ActionType t) const {
    for (const auto& a : actions) {
        if (a.type == t) {
            return &a;
        }
    }
    return nullptr;
}

// ============================================================================
// RuleEngine
// ============================================================================
RuleEngine::RuleEngine(RuleCache& cache, bool enabled) : cache_(cache), enabled_(enabled) {}

int RuleEngine::reasonCodeOf(const std::string& reasonCode) {
    if (reasonCode == "BLACKLISTED")  return kUserBlacklisted;      // 3010
    if (reasonCode == "ACTIVE_LIMIT") return kActiveLimitExceeded;  // 3027
    return kReservationStateInvalid;                                // 3007
}

std::string RuleEngine::reasonMsgOf(const std::string& reasonCode) {
    if (reasonCode == "BLACKLISTED")  return "账号处于黑名单，无法预约";
    if (reasonCode == "ACTIVE_LIMIT") return "同时进行的预约已达上限";
    return "当前状态不允许该操作";
}

std::string RuleEngine::targetOf(const ResolvedAction& a) {
    switch (a.type) {
        case ActionType::DeductCredit:
        case ActionType::DeductOnly:
            return a.violationType;
        case ActionType::Transition:
            return a.targetStatus;
        default:
            return std::string();
    }
}

bool RuleEngine::strongerThan(const ResolvedAction& a, const ResolvedAction& b) {
    switch (a.type) {
        case ActionType::DeductCredit:
        case ActionType::DeductOnly:
            return a.deductScore > b.deductScore;    // 取最高扣分
        case ActionType::DeductDeposit:
            return a.ratio > b.ratio;                // 取最高比例
        case ActionType::FreezeDeposit:
            return a.rate > b.rate;                  // 取最高冻结比例
        default:
            return false;                            // 不可比 → 保留先到的（更小 priority）
    }
}

ResolvedAction RuleEngine::resolve(const Rule& r, const FactBag& facts) {
    ResolvedAction out;
    out.ruleCode = r.ruleCode;
    out.params   = r.params;

    const auto typeOpt =
        actionTypeFrom(r.action.value("type", std::string()));
    out.type = typeOpt.value_or(ActionType::Reject);

    auto paramNum = [&r](const char* key, double def) {
        if (r.params.is_object()) {
            const auto it = r.params.find(key);
            if (it != r.params.end() && it->is_number()) {
                return it->get<double>();
            }
        }
        return def;
    };

    switch (out.type) {
        case ActionType::Reject:
            // reasonCode 由调用方从 rule.action 直接读（见 runOn），这里无需处理
            break;

        case ActionType::DeductCredit:
        case ActionType::DeductOnly:
            out.violationType = r.action.value("violationType", std::string());
            out.deductCredit  = r.action.value("deductCredit", true);
            out.deductScore   = static_cast<int>(paramNum("deductScore", 0.0));
            break;

        case ActionType::Transition:
            out.transitEvent   = r.action.value("event", std::string());
            out.targetStatus   = r.action.value("targetStatus", std::string());
            out.releaseSlots   = r.action.value("releaseSlots", false);
            out.deductCredit   = r.action.value("deductCredit", false);
            if (r.action.contains("violationType")) {
                out.violationType = r.action.value("violationType", std::string());
            }
            // 迁移顺带扣分（#6/#7）：分值同样取 params.deductScore
            out.deductScore = static_cast<int>(paramNum("deductScore", 0.0));
            break;

        case ActionType::FreezeDeposit: {
            out.amountFrom = r.action.value("amountFrom", std::string("$resource.depositAmount"));
            const std::string rateParam = r.action.value("rateParam", std::string("rate"));
            out.rate = paramNum(rateParam.c_str(), 1.0);
            // 顺手把 amountFrom 解析成数值：执行器不必再认识 `$field` 语法
            if (!out.amountFrom.empty() && out.amountFrom[0] == '$') {
                Fact f;
                if (facts.get(out.amountFrom.substr(1), &f)) {
                    const double v = f.asNum();
                    out.amount     = std::isnan(v) ? 0.0 : v;
                }
            }
            break;
        }

        case ActionType::DeductDeposit:
            out.ratio     = paramNum("ratio", 0.0);
            out.maxAmount = paramNum("maxAmount", 0.0);
            break;
    }
    return out;
}

EvalResult RuleEngine::runOn(std::vector<Rule> rules, EvalPhase phase,
                             const FactBag& facts) const {
    EvalResult out;

    // ---- ① 排序（RuleCache 已排好，这里再稳定一次：evaluateWith 传进来的表可能没排）----
    std::stable_sort(rules.begin(), rules.end(), [](const Rule& a, const Rule& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.ruleCode < b.ruleCode;
    });

    // 分组键用 std::map（有序），保证 actions 的输出顺序也是确定的
    struct Bucket {
        ResolvedAction best;
        bool           has = false;
    };
    std::map<std::pair<int, std::string>, Bucket> adjustBucket;

    for (const Rule& r : rules) {
        if (r.evalPhase != phase) {
            continue;
        }
        // 动作类型先解析出来：决定 fail 策略（D-P3-5）
        const auto typeOpt = actionTypeFrom(r.action.is_object()
                                                ? r.action.value("type", std::string())
                                                : std::string());
        if (!typeOpt.has_value()) {
            const std::string msg = "action.type 非法或缺失";
            out.errors.push_back(r.ruleCode + ": " + msg);
            LOG_WARN("规则 {} 的动作定义非法（{}），按 fail 策略处理", r.ruleCode, msg);
            // 动作都解析不出来时无法判断是否 reject 类 → 保守按 adjust 处理（跳过）
            continue;
        }
        const ActionType type = *typeOpt;

        const MatchOutcome m = ConditionEval::match(r.condition, facts, r.params);
        if (m.malformed()) {
            out.errors.push_back(r.ruleCode + ": " + m.error);
            LOG_WARN("规则 {} 条件求值失败（{}），按动作类型降级", r.ruleCode, m.error);
            if (isRejectAction(type)) {
                // fail-closed：黑名单/准入类规则坏了宁可拒绝，也不放违约用户进闸门
                out.rejected   = true;
                out.rejectCode = kRuleEngineFailClosed;   // 3029
                out.rejectMsg  = "规则校验暂不可用，请稍后重试";
                return out;
            }
            continue;   // adjust 类 fail-safe：跳过这一条
        }
        if (!m.matched()) {
            continue;
        }

        out.matchedRuleCodes.push_back(r.ruleCode);

        // ---- reject 类：首个命中即短路（priority 最小者胜）----
        if (isRejectAction(type)) {
            const std::string reasonCode =
                r.action.is_object() ? r.action.value("reasonCode", std::string()) : std::string();
            out.rejected   = true;
            out.rejectCode = reasonCodeOf(reasonCode);
            out.rejectMsg  = reasonMsgOf(reasonCode);
            return out;
        }

        // ---- adjust 类：分组取最强，不叠加 ----
        ResolvedAction cand = resolve(r, facts);
        auto key = std::make_pair(static_cast<int>(cand.type), targetOf(cand));
        Bucket& b = adjustBucket[key];
        if (!b.has || strongerThan(cand, b.best)) {
            b.best = std::move(cand);
            b.has  = true;
        }
    }

    for (auto& [k, b] : adjustBucket) {
        if (b.has) {
            out.actions.push_back(b.best);
        }
    }
    return out;
}

EvalResult RuleEngine::evaluate(EvalPhase phase, const FactBag& facts) {
    if (!enabled_) {
        return EvalResult{};   // 总开关关闭：空结果，调用方走 P2 硬编码路径
    }
    std::vector<Rule> rules;
    if (!cache_.snapshot(&rules, /*onlyEnabled=*/true)) {
        EvalResult out;
        out.degraded = true;   // 整表不可读 → 调用方走 P2 兜底
        LOG_WARN("规则引擎降级：规则表不可读，本轮走 P2 硬编码兜底");
        return out;
    }
    return runOn(std::move(rules), phase, facts);
}

EvalResult RuleEngine::evaluateWith(const std::vector<Rule>& rules, EvalPhase phase,
                                    const FactBag& facts) const {
    if (!enabled_) {
        return EvalResult{};
    }
    return runOn(rules, phase, facts);
}

int RuleEngine::paramInt(const std::string& ruleCode, const char* key, int fallback) const {
    return cache_.paramInt(ruleCode, key, fallback);
}

double RuleEngine::paramDouble(const std::string& ruleCode, const char* key,
                               double fallback) const {
    return cache_.paramDouble(ruleCode, key, fallback);
}

} // namespace rf
