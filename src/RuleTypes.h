// =============================================================================
// src/RuleTypes.h  —  规则引擎的纯类型层（header-only，零 DB / 零 Redis）
// 对应 ARCH-P3 §3 / §7.1 / D-P3-2 / D-P3-3
//
// 本文件只放「数据结构 + 取值换算」，不放解析与求值逻辑：
//   Fact / FactBag        —— 事实容器（内联实现，无依赖）
//   ValueNode             —— 值节点（**声明**；parse/eval 在 ConditionEval.cpp）
//   Rule                  —— 一条已解析的规则
//   EvalPhase / ActionType —— 求值时机与动作类型 + 双向转换
//
// 关键约定（D-P3-3）：时间是 "yyyy-MM-ddTHH:mm:ss" 定长串，字典序即时间序，
// 所以 gt/lt 直接按字符串比较，不需要解析成时间戳。
// =============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace rf {

// ------------------------------------------------------------------ 求值时机
/** 求值时机（对应 rule_config.eval_phase，D-P3-2：显式列，不由 rule_type 推断） */
enum class EvalPhase { CREATE, TIMER, SETTLE };

inline std::string toStr(EvalPhase p) {
    switch (p) {
        case EvalPhase::TIMER:  return "TIMER";
        case EvalPhase::SETTLE: return "SETTLE";
        case EvalPhase::CREATE:
        default:                return "CREATE";
    }
}

inline std::optional<EvalPhase> evalPhaseFrom(const std::string& s) {
    if (s == "CREATE") return EvalPhase::CREATE;
    if (s == "TIMER")  return EvalPhase::TIMER;
    if (s == "SETTLE") return EvalPhase::SETTLE;
    return std::nullopt;
}

// ------------------------------------------------------------------ 动作类型
/** 动作类型（对应 action_json.type，§3.3） */
enum class ActionType {
    Reject, DeductCredit, DeductOnly, Transition, FreezeDeposit, DeductDeposit
};

inline std::string toStr(ActionType t) {
    switch (t) {
        case ActionType::DeductCredit:  return "deduct_credit";
        case ActionType::DeductOnly:    return "deduct_only";
        case ActionType::Transition:    return "transition";
        case ActionType::FreezeDeposit: return "freeze_deposit";
        case ActionType::DeductDeposit: return "deduct_deposit";
        case ActionType::Reject:
        default:                        return "reject";
    }
}

inline std::optional<ActionType> actionTypeFrom(const std::string& s) {
    if (s == "reject")         return ActionType::Reject;
    if (s == "deduct_credit")  return ActionType::DeductCredit;
    if (s == "deduct_only")    return ActionType::DeductOnly;
    if (s == "transition")     return ActionType::Transition;
    if (s == "freeze_deposit") return ActionType::FreezeDeposit;
    if (s == "deduct_deposit") return ActionType::DeductDeposit;
    return std::nullopt;
}

/**
 * 是否「拒绝」类动作 —— 决定求值失败时的 fail 策略（D-P3-5）：
 * reject 类 fail-closed（返回 3029），其余 fail-safe（跳过 + WARN）。
 */
inline bool isRejectAction(ActionType t) { return t == ActionType::Reject; }

// ------------------------------------------------------------------ 一条规则
/** 一条已解析的规则（RuleCache 的缓存单元） */
struct Rule {
    int64_t        id = 0;
    std::string    ruleCode;
    std::string    ruleName;
    std::string    ruleType;      ///< CREDIT / RESERVATION / DEPOSIT（仅分类展示）
    EvalPhase      evalPhase = EvalPhase::CREATE;
    nlohmann::json condition;     ///< 已确认是 object
    nlohmann::json action;        ///< 已确认是 object 且 type 合法
    nlohmann::json params;        ///< 已确认是 object
    int            priority = 100;
    bool           enabled  = true;
    std::string    remark;
    std::string    updatedAt;
};

// ------------------------------------------------------------------ 事实值
/**
 * 一个事实值：四种标量 + 数组 + null。
 * 数组只为 `in` / `notIn` 的 ValueNode::Array 存在（§3.2 最后一行）；
 * 其余比较一律拒绝数组，避免「数组 == 标量」这种含义不明的判定。
 */
struct Fact {
    enum class Kind { Null, Str, Num, Bool, Arr } kind = Kind::Null;
    std::string       s;
    double            n = 0.0;
    bool              b = false;
    std::vector<Fact> arr;   ///< Kind::Arr

    bool isNull() const { return kind == Kind::Null; }
    bool isArray() const { return kind == Kind::Arr; }

    /** 非布尔按「非空 / 非零」换算（规则作者写 eq true 时也能得到确定结果） */
    bool asBool() const {
        switch (kind) {
            case Kind::Bool: return b;
            case Kind::Num:  return n != 0.0;
            case Kind::Str:  return !s.empty();
            case Kind::Null:
            default:         return false;
        }
    }

    /** 非数值 → NaN（比较时 NaN 一律不成立，避免静默误判） */
    double asNum() const {
        switch (kind) {
            case Kind::Num:  return n;
            case Kind::Bool: return b ? 1.0 : 0.0;
            case Kind::Str: {
                try {
                    size_t     used = 0;
                    const double v  = std::stod(s, &used);
                    return (used == s.size()) ? v : std::nan("");
                } catch (const std::exception&) {
                    return std::nan("");
                }
            }
            case Kind::Null:
            default:         return std::nan("");
        }
    }

    /** 数值按 "%.10g" 输出，保证与字符串型事实比较时形态稳定 */
    std::string asStr() const {
        switch (kind) {
            case Kind::Str: return s;
            case Kind::Bool: return b ? "true" : "false";
            case Kind::Num: {
                char buf[40] = {0};
                std::snprintf(buf, sizeof(buf), "%.10g", n);
                return std::string(buf);
            }
            case Kind::Null:
            default: return std::string();
        }
    }

    static Fact of(const std::string& v) {
        Fact f;
        f.kind = Kind::Str;
        f.s    = v;
        return f;
    }
    static Fact of(double v) {
        Fact f;
        f.kind = Kind::Num;
        f.n    = v;
        return f;
    }
    static Fact of(bool v) {
        Fact f;
        f.kind = Kind::Bool;
        f.b    = v;
        return f;
    }
    static Fact ofNull() { return Fact(); }
    static Fact ofArray(std::vector<Fact> v) {
        Fact f;
        f.kind = Kind::Arr;
        f.arr  = std::move(v);
        return f;
    }
};

// ------------------------------------------------------------------ 事实容器
/** 事实容器：只在一次 evaluate() 内有效；name 形如 "user.status"（不带 $） */
class FactBag {
public:
    void set(const std::string& name, const Fact& f) { facts_[name] = f; }

    /** 时间统一按 "yyyy-MM-ddTHH:mm:ss" 存字符串（D-P3-3：字典序即时间序） */
    void setTime(const std::string& name, const std::string& iso) {
        facts_[name] = Fact::of(iso);
    }

    bool get(const std::string& name, Fact* out) const {
        const auto it = facts_.find(name);
        if (it == facts_.end()) {
            return false;
        }
        if (out != nullptr) {
            *out = it->second;
        }
        return true;
    }

    bool has(const std::string& name) const { return facts_.find(name) != facts_.end(); }

    const std::map<std::string, Fact>& all() const { return facts_; }

    /**
     * facts 白名单（ARCH-P3 §3.4）——**保存规则时就据此拦截**，
     * 不留到运行时才发现「引用了不存在的事实」。
     */
    static bool isKnownFact(const std::string& name) {
        static const char* kKnown[] = {
            "now",
            "user.id", "user.status", "user.role", "user.creditScore", "user.activeReservations",
            "resource.id", "resource.type", "resource.status", "resource.depositAmount",
            "reservation.id", "reservation.status", "reservation.startTime",
            "reservation.endTime", "reservation.createdAt", "reservation.deductSeq",
            "reservation.overdueMinutes",
            "deposit.frozen", "deposit.deducted", "deposit.refunded",
            "damage.requestedAmount"};
        for (const char* k : kKnown) {
            if (name == k) {
                return true;
            }
        }
        return false;
    }

private:
    std::map<std::string, Fact> facts_;
};

// ------------------------------------------------------------------ 值节点
/**
 * 值节点（§3.2，五种形态）：
 *   字面量      "PENDING_REVIEW" / 5 / 1.0 / true
 *   字段引用    以 $ 开头的字符串（**只允许出现在 condition 的 field/value 里**）
 *   参数引用    {"param":"maxActive"}
 *   派生时间    {"expr":"tsAddMin","field":"$x","param":"y"}
 *   数组        ["A","B"]
 */
struct ValueNode {
    enum class Kind { Literal, Field, Param, TsAddMin, Array } kind = Kind::Literal;
    Fact                   literal;
    std::string            field;   ///< "$xxx" 去掉 $ 后的名字
    std::string            param;   ///< params_json 的 key
    std::vector<ValueNode> items;   ///< Array 形态

    /** 从 JSON 解析；失败返回 false 并写 err（→ 3024） */
    static bool parse(const nlohmann::json& j, ValueNode* out, std::string* err);

    /** 求值；Field 缺失 / 类型不符 / param 缺失 → false（调用方转 Malformed） */
    bool eval(const FactBag& facts, const nlohmann::json& params, Fact* out,
              std::string* err) const;
};

} // namespace rf
