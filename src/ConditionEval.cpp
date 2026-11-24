// =============================================================================
// src/ConditionEval.cpp  —  ValueNode 解析/求值 + condition 匹配实现
// 对应 ARCH-P3 §3.1 / §3.2 / §5.2
//
// 纯函数，无 DB / Redis / 日志副作用。所有错误都通过返回值表达，不抛异常。
//
// 时间比较（D-P3-3）：ISO 定长串 "yyyy-MM-ddTHH:mm:ss" 的字典序 == 时间序，
// 所以 gt/lt 直接比字符串，既不解析也不依赖时区。
// =============================================================================
#include "ConditionEval.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>

namespace rf {

namespace {

/** ISO 串 → epoch 秒；失败返回 false（与 TimeUtil::parseIso 同口径，此处自带以免依赖日志层） */
bool parseIsoLocal(const std::string& s, std::time_t* out) {
    if (s.size() < 19 || out == nullptr) {
        return false;
    }
    std::string buf = s.substr(0, 19);
    if (buf[10] == 'T' || buf[10] == 't') {
        buf[10] = ' ';
    }
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(buf.c_str(), "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) {
        return false;
    }
    std::tm t{};
    t.tm_year  = y - 1900;
    t.tm_mon   = mo - 1;
    t.tm_mday  = d;
    t.tm_hour  = h;
    t.tm_min   = mi;
    t.tm_sec   = se;
    t.tm_isdst = -1;
    const std::time_t v = std::mktime(&t);
    if (v == static_cast<std::time_t>(-1)) {
        return false;
    }
    *out = v;
    return true;
}

/** epoch 秒 → "yyyy-MM-ddTHH:mm:ss" */
std::string formatIsoLocal(std::time_t v) {
    std::tm t{};
#ifdef _WIN32
    if (localtime_s(&t, &v) != 0) {
        return std::string();
    }
#else
    if (localtime_r(&v, &t) == nullptr) {
        return std::string();
    }
#endif
    char buf[32] = {0};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t) == 0) {
        return std::string();
    }
    return std::string(buf);
}

/** 取 params 里的一个键；不存在或类型不符返回 false */
bool paramOf(const nlohmann::json& params, const std::string& key, Fact* out) {
    if (out == nullptr || !params.is_object()) {
        return false;
    }
    const auto it = params.find(key);
    if (it == params.end() || it->is_null()) {
        return false;
    }
    if (it->is_number()) {
        *out = Fact::of(it->get<double>());
        return true;
    }
    if (it->is_boolean()) {
        *out = Fact::of(it->get<bool>());
        return true;
    }
    if (it->is_string()) {
        *out = Fact::of(it->get<std::string>());
        return true;
    }
    return false;
}

/** 两个标量是否「数值可比」：都是 Num，或 Str 能完整解析成数值 */
bool bothNumeric(const Fact& a, const Fact& b, double* x, double* y) {
    if (a.isNull() || b.isNull() || a.isArray() || b.isArray()) {
        return false;
    }
    const double va = a.asNum();
    const double vb = b.asNum();
    if (std::isnan(va) || std::isnan(vb)) {
        return false;
    }
    if (x != nullptr) {
        *x = va;
    }
    if (y != nullptr) {
        *y = vb;
    }
    return true;
}

/** 标量相等（类型不同但都能转数值时按数值比，否则按字符串比） */
bool scalarEquals(const Fact& a, const Fact& b) {
    if (a.isNull() || b.isNull()) {
        return a.isNull() && b.isNull();
    }
    if (a.isArray() != b.isArray()) {
        return false;
    }
    if (a.isArray()) {
        if (a.arr.size() != b.arr.size()) {
            return false;
        }
        for (size_t i = 0; i < a.arr.size(); ++i) {
            if (!scalarEquals(a.arr[i], b.arr[i])) {
                return false;
            }
        }
        return true;
    }
    double x = 0.0;
    double y = 0.0;
    if (bothNumeric(a, b, &x, &y)) {
        return x == y;
    }
    return a.asStr() == b.asStr();
}

}  // namespace

// ============================================================================
// ValueNode
// ============================================================================
bool ValueNode::parse(const nlohmann::json& j, ValueNode* out, std::string* err) {
    auto fail = [err](const std::string& m) {
        if (err != nullptr) {
            *err = m;
        }
        return false;
    };
    if (out == nullptr) {
        return fail("ValueNode 输出指针为空");
    }

    // ---- 数组：逐元素递归 ----
    if (j.is_array()) {
        out->kind = Kind::Array;
        out->items.clear();
        out->items.reserve(j.size());
        for (const auto& e : j) {
            ValueNode sub;
            if (!parse(e, &sub, err)) {
                return false;
            }
            out->items.push_back(std::move(sub));
        }
        return true;
    }

    // ---- 对象：param 引用 / expr 派生 ----
    if (j.is_object()) {
        if (j.contains("expr")) {
            const auto it = j.find("expr");
            if (!it->is_string() || it->get<std::string>() != "tsAddMin") {
                return fail("value.expr 只支持 \"tsAddMin\"（新增表达式需同步更新 3024 校验清单）");
            }
            const auto f = j.find("field");
            if (f == j.end() || !f->is_string() || f->get<std::string>().empty() ||
                f->get<std::string>()[0] != '$') {
                return fail("value.expr 必须带 field 且以 $ 开头");
            }
            const auto p = j.find("param");
            if (p == j.end() || !p->is_string() || p->get<std::string>().empty()) {
                return fail("value.expr 必须带 param（分钟数参数名）");
            }
            out->kind  = Kind::TsAddMin;
            out->field = f->get<std::string>().substr(1);
            out->param = p->get<std::string>();
            return true;
        }
        if (j.contains("param")) {
            const auto p = j.find("param");
            if (!p->is_string() || p->get<std::string>().empty()) {
                return fail("value.param 必须是非空字符串");
            }
            out->kind  = Kind::Param;
            out->param = p->get<std::string>();
            return true;
        }
        return fail("value 对象只支持 {\"param\":...} 或 {\"expr\":\"tsAddMin\",...}");
    }

    // ---- 标量：数字 / 布尔 / 字符串（$ 开头 = 字段引用）----
    if (j.is_number()) {
        out->kind    = Kind::Literal;
        out->literal = Fact::of(j.get<double>());
        return true;
    }
    if (j.is_boolean()) {
        out->kind    = Kind::Literal;
        out->literal = Fact::of(j.get<bool>());
        return true;
    }
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '$') {
            out->kind  = Kind::Field;
            out->field = s.substr(1);
            return true;
        }
        out->kind    = Kind::Literal;
        out->literal = Fact::of(s);
        return true;
    }
    if (j.is_null()) {
        out->kind    = Kind::Literal;
        out->literal = Fact::ofNull();
        return true;
    }
    return fail("value 类型不支持（只允许标量 / $字段 / {param} / {expr} / 数组）");
}

bool ValueNode::eval(const FactBag& facts, const nlohmann::json& params, Fact* out,
                     std::string* err) const {
    auto fail = [err](const std::string& m) {
        if (err != nullptr) {
            *err = m;
        }
        return false;
    };

    switch (kind) {
        case Kind::Literal:
            if (out != nullptr) {
                *out = literal;
            }
            return true;

        case Kind::Field:
            if (!facts.get(field, out)) {
                return fail("未声明的 fact：$" + field);
            }
            return true;

        case Kind::Param: {
            Fact f;
            if (!paramOf(params, param, &f)) {
                return fail("params 缺少参数：" + param);
            }
            if (out != nullptr) {
                *out = f;
            }
            return true;
        }

        case Kind::TsAddMin: {
            Fact base;
            if (!facts.get(field, &base)) {
                return fail("未声明的 fact：$" + field);
            }
            Fact minutes;
            if (!paramOf(params, param, &minutes)) {
                return fail("params 缺少参数：" + param);
            }
            if (base.isNull() || minutes.isNull()) {
                return fail("tsAddMin 的操作数不能为空");
            }
            std::time_t t = 0;
            if (!parseIsoLocal(base.asStr(), &t)) {
                return fail("tsAddMin 的 field 不是合法时间串：$" + field);
            }
            const double  off  = minutes.asNum();
            const int64_t addm = static_cast<int64_t>(off);
            const std::string shifted = formatIsoLocal(t + static_cast<std::time_t>(addm) * 60);
            if (shifted.empty()) {
                return fail("tsAddMin 结果格式化失败");
            }
            if (out != nullptr) {
                *out = Fact::of(shifted);
            }
            return true;
        }

        case Kind::Array: {
            std::vector<Fact> vals;
            vals.reserve(items.size());
            for (const auto& it : items) {
                Fact f;
                if (!it.eval(facts, params, &f, err)) {
                    return false;
                }
                vals.push_back(std::move(f));
            }
            if (out != nullptr) {
                *out = Fact::ofArray(std::move(vals));
            }
            return true;
        }
    }
    return fail("未知 ValueNode 形态");
}

// ============================================================================
// ConditionEval
// ============================================================================
bool ConditionEval::isKnownOp(const std::string& op) {
    static const char* kOps[] = {"eq",   "ne",     "gt",   "gte",     "lt",
                                 "lte",  "in",     "notIn", "isNull", "notNull",
                                 "notEmpty"};
    for (const char* k : kOps) {
        if (op == k) {
            return true;
        }
    }
    return false;
}

bool ConditionEval::compare(const std::string& op, const Fact& lhs, const Fact& rhs, bool* ok,
                            std::string* err) {
    auto malformed = [ok, err](const std::string& m) {
        if (ok != nullptr) {
            *ok = false;
        }
        if (err != nullptr) {
            *err = m;
        }
        return false;
    };
    if (ok != nullptr) {
        *ok = true;
    }

    if (op == "eq") {
        return scalarEquals(lhs, rhs);
    }
    if (op == "ne") {
        return !scalarEquals(lhs, rhs);
    }

    if (op == "in" || op == "notIn") {
        if (!rhs.isArray()) {
            return malformed("op " + op + " 的 value 必须是数组");
        }
        bool hit = false;
        for (const auto& e : rhs.arr) {
            if (scalarEquals(lhs, e)) {
                hit = true;
                break;
            }
        }
        return (op == "in") ? hit : !hit;
    }

    if (op == "gt" || op == "gte" || op == "lt" || op == "lte") {
        if (lhs.isNull() || rhs.isNull() || lhs.isArray() || rhs.isArray()) {
            return malformed("op " + op + " 的操作数必须是非空标量");
        }
        // 数值优先；都能转数值就按数值比，否则按字符串比（时间串走这条，D-P3-3）
        double x = 0.0;
        double y = 0.0;
        if (bothNumeric(lhs, rhs, &x, &y)) {
            if (op == "gt")  return x > y;
            if (op == "gte") return x >= y;
            if (op == "lt")  return x < y;
            return x <= y;
        }
        const std::string a = lhs.asStr();
        const std::string b = rhs.asStr();
        if (op == "gt")  return a > b;
        if (op == "gte") return a >= b;
        if (op == "lt")  return a < b;
        return a <= b;
    }

    return malformed("op " + op + " 不适合走 compare()");
}

MatchOutcome ConditionEval::evalPredicate(const nlohmann::json& p, const FactBag& facts,
                                          const nlohmann::json& params) {
    if (!p.is_object()) {
        return MatchOutcome::malformed("predicate 必须是 JSON 对象");
    }
    const auto opIt = p.find("op");
    if (opIt == p.end() || !opIt->is_string()) {
        return MatchOutcome::malformed("predicate.op 必填且必须是字符串");
    }
    const std::string op = opIt->get<std::string>();
    if (!isKnownOp(op)) {
        return MatchOutcome::malformed("未知 op：" + op);
    }

    const auto fIt = p.find("field");
    if (fIt == p.end() || !fIt->is_string()) {
        return MatchOutcome::malformed("predicate.field 必填且必须是字符串");
    }
    const std::string fieldRef = fIt->get<std::string>();
    if (fieldRef.empty() || fieldRef[0] != '$') {
        return MatchOutcome::malformed("field 必须以 $ 开头：" + fieldRef);
    }
    const std::string factName = fieldRef.substr(1);
    if (!FactBag::isKnownFact(factName)) {
        return MatchOutcome::malformed("field 不在 facts 白名单内：" + fieldRef);
    }

    Fact lhs;
    if (!facts.get(factName, &lhs)) {
        return MatchOutcome::malformed("本次求值未提供 fact：" + fieldRef);
    }

    // 三个「无 value」的 op
    if (op == "isNull") {
        return MatchOutcome::of(lhs.isNull());
    }
    if (op == "notNull") {
        return MatchOutcome::of(!lhs.isNull());
    }
    if (op == "notEmpty") {
        return MatchOutcome::of(!lhs.isNull() && !lhs.asStr().empty());
    }

    const auto vIt = p.find("value");
    if (vIt == p.end()) {
        return MatchOutcome::malformed("op " + op + " 缺少 value");
    }
    ValueNode node;
    std::string perr;
    if (!ValueNode::parse(*vIt, &node, &perr)) {
        return MatchOutcome::malformed("value 解析失败：" + perr);
    }
    Fact rhs;
    if (!node.eval(facts, params, &rhs, &perr)) {
        return MatchOutcome::malformed("value 求值失败：" + perr);
    }

    bool        ok = true;
    std::string cerr;
    const bool  matched = compare(op, lhs, rhs, &ok, &cerr);
    if (!ok) {
        return MatchOutcome::malformed(cerr);
    }
    return MatchOutcome::of(matched);
}

MatchOutcome ConditionEval::match(const nlohmann::json& cond, const FactBag& facts,
                                  const nlohmann::json& params) {
    if (!cond.is_object() || !cond.contains("all") || !cond["all"].is_array()) {
        return MatchOutcome::malformed("condition 必须是 {\"all\":[...]}");
    }

    // AND(all)
    for (const auto& p : cond["all"]) {
        const MatchOutcome r = evalPredicate(p, facts, params);
        if (r.malformed()) {
            return r;
        }
        if (!r.matched()) {
            return MatchOutcome::no();   // AND 短路
        }
    }

    // 可选 OR(any)
    if (cond.contains("any") && !cond["any"].is_null()) {
        if (!cond["any"].is_array()) {
            return MatchOutcome::malformed("condition.any 必须是数组");
        }
        if (!cond["any"].empty()) {
            bool anyHit = false;
            for (const auto& p : cond["any"]) {
                const MatchOutcome r = evalPredicate(p, facts, params);
                if (r.malformed()) {
                    return r;
                }
                if (r.matched()) {
                    anyHit = true;
                    break;
                }
            }
            if (!anyHit) {
                return MatchOutcome::no();
            }
        }
    }
    return MatchOutcome::yes();
}

} // namespace rf
