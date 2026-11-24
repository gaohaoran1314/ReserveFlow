// =============================================================================
// src/RuleEngine.h  —  三阶段规则求值引擎（ARCH-P3 §5 / D-P3-4 / D-P3-5）
//
// 职责边界：**只裁决，不执行**。
//   evaluate() 返回「应该做什么」（ResolvedAction 列表）或「拒绝」；
//   真正写库（迁状态/扣分/冻结/派候补）在各 Service 里实现（ActionExecutor）。
// 因此引擎不需要 DB 事务，也不会出现「引擎写了一半」的中间态。
//
// 三条确定性保证（D-P3-4）：
//   1) 按 (priority, rule_code) 升序求值；
//   2) reject 类首个命中即短路；
//   3) adjust 类按 (动作类型, 目标) 分组，每组只取**最强**的一条（不叠加）。
// =============================================================================
#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "Result.h"
#include "RuleTypes.h"
#include "RuleCache.h"

namespace rf {

/** 裁决后的可执行动作 */
struct ResolvedAction {
    std::string ruleCode;        ///< 供 status_log.rule_code / credit_log.rule_code
    ActionType  type = ActionType::Reject;
    std::string violationType;   ///< OVERDUE / NO_SHOW / DAMAGE（deduct* 时有值）
    std::string transitEvent;    ///< Transition 时：TIMEOUT / REPORT_DUE / ...
    std::string targetStatus;    ///< Transition 时
    bool        releaseSlots = false;
    int         deductScore  = 0;
    bool        deductCredit = false;   ///< Transition 的可选副动作：是否顺带扣分
    double      ratio        = 0.0;
    double      maxAmount    = 0.0;
    double      rate         = 0.0;
    // FreezeDeposit：amountFrom 是字段名（文档语义），amount 是该字段在本次 facts 下的
    // 求值结果 —— 引擎顺手解析，执行器就不用再认识 `$field` 语法了（§7.3 的小扩展）。
    std::string amountFrom;
    double      amount = 0.0;
    nlohmann::json params;       ///< 原始 params（动作执行器可用）

    nlohmann::json toJson() const;
};

/** 一次求值的完整结论 */
struct EvalResult {
    bool                        rejected   = false;
    int                         rejectCode = 0;   ///< 3010 / 3027 / 3029 / 3007
    std::string                 rejectMsg;
    std::vector<ResolvedAction> actions;
    std::vector<std::string>    matchedRuleCodes;
    bool                        degraded = false; ///< true → 调用方走 P2 硬编码兜底
    std::vector<std::string>    errors;           ///< 解析错误摘要（WARN 日志用）

    bool                  hasAction(ActionType t) const;
    const ResolvedAction* findAction(ActionType t) const;
};

class RuleEngine {
public:
    explicit RuleEngine(RuleCache& cache, bool enabled = true);

    /** 总开关（D-P3-6）；false 时 evaluate 返回空结果（不 rejected、不 degraded） */
    bool enabled() const { return enabled_; }
    void setEnabled(bool v) { enabled_ = v; }

    /**
     * 三阶段求值（§5.1）。
     * 保证：不抛异常；不访问 DB（快照来自 RuleCache）；除日志外无副作用。
     */
    EvalResult evaluate(EvalPhase phase, const FactBag& facts);

    /**
     * 直接用给定规则求值（**单测专用**：不经过 DB / RuleCache 的指纹与 TTL）。
     * T05 的五种裁决场景就是这么验的。
     */
    EvalResult evaluateWith(const std::vector<Rule>& rules, EvalPhase phase,
                            const FactBag& facts) const;

    /** 取规则参数（D-P3-11）：窗口/分值/比例的唯一真相 */
    int    paramInt(const std::string& ruleCode, const char* key, int fallback) const;
    double paramDouble(const std::string& ruleCode, const char* key, double fallback) const;

private:
    EvalResult runOn(std::vector<Rule> rules, EvalPhase phase, const FactBag& facts) const;

    /** 把 Rule 的 action + params 解析成 ResolvedAction（含 amountFrom 的求值） */
    static ResolvedAction resolve(const Rule& r, const FactBag& facts);

    /** reject reasonCode → 业务错误码 / 文案（§3.3） */
    static int         reasonCodeOf(const std::string& reasonCode);
    static std::string reasonMsgOf(const std::string& reasonCode);

    /** 同组内比较强度：deductScore / ratio / rate 取大；不可比返回 false（保留先到的） */
    static bool strongerThan(const ResolvedAction& a, const ResolvedAction& b);

    /** 分组键里的「目标」：扣分类用 violationType，迁移类用 targetStatus */
    static std::string targetOf(const ResolvedAction& a);

    RuleCache& cache_;
    bool       enabled_;
};

} // namespace rf
