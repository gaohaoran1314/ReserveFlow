// =============================================================================
// src/ConditionEval.h  —  条件求值（ARCH-P3 §3.1 / §5.2 / D-P3-3）
//
// **纯函数层**：只吃 condition JSON + FactBag + params，不碰 DB / Redis / 全局状态。
// 因此可以写一个临时 main 直接断言（T04 的判据就是这么设计的）。
//
// 三态返回是关键：No / Yes / Malformed。
//   · Malformed 交给 RuleEngine 按动作类型决定 fail 策略（reject fail-closed）；
//   · 不能用 bool 返回，否则「条件不成立」与「条件写错了」会混为一谈。
// =============================================================================
#pragma once

#include <string>

#include "json.hpp"
#include "RuleTypes.h"

namespace rf {

/** 一个 predicate / 整条 condition 的求值结果 */
struct MatchOutcome {
    enum class Status { No, Yes, Malformed } status = Status::No;
    std::string error;

    bool matched() const { return status == Status::Yes; }
    bool malformed() const { return status == Status::Malformed; }

    static MatchOutcome of(bool v) { return v ? yes() : no(); }
    static MatchOutcome yes() { return MatchOutcome{Status::Yes, std::string()}; }
    static MatchOutcome no() { return MatchOutcome{Status::No, std::string()}; }
    static MatchOutcome malformed(std::string err) {
        return MatchOutcome{Status::Malformed, std::move(err)};
    }
};

class ConditionEval {
public:
    /**
     * 求值一条 condition_json：
     *   {"all":[pred...], "any":[pred...]?}
     *     最终 = AND(all) && (OR(any) 若存在)
     * @return No / Yes / Malformed（Malformed 带可读原因）
     */
    static MatchOutcome match(const nlohmann::json& cond, const FactBag& facts,
                              const nlohmann::json& params);

    /** §3.1.1 的 10 个 op（`notEmpty` 是 `notNull` 的别名） */
    static bool isKnownOp(const std::string& op);

private:
    static MatchOutcome evalPredicate(const nlohmann::json& p, const FactBag& facts,
                                      const nlohmann::json& params);

    /** eq / ne / gt / gte / lt / lte / in / notIn 的比较实现 */
    static bool compare(const std::string& op, const Fact& lhs, const Fact& rhs, bool* ok,
                        std::string* err);
};

} // namespace rf
