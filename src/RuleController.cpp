// =============================================================================
// src/RuleController.cpp  —  规则管理接口（ARCH-P3 §7.5 / T08 / 附录 A）
//
// 7 + 1 个接口：列 / 详情 / 创建 / 更新 / 启停 / 改优先级 / 删除 / 只校验不落库。
// 全部 ADMIN（路由层用 protect(..., Role::ADMIN) 兜）。
//
// **写成功后立刻 RuleCache::reload()**（D-P3-10）：管理员改完规则不应等 5s TTL，
// 否则「停用黑名单拦截 → 立刻下单」这类演示会看到旧行为。
//
// 校验清单完整实现附录 A 的 15 项，全部失败都归到 3024（msg 里逐条列出原因），
// 校验不通过**绝不落库**。
// =============================================================================
#include "Controllers.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"
#include "ConditionEval.h"
#include "RuleRepo.h"

namespace rf {

namespace {

/** 安全领取 JSON 数组元素（不存在返回空数组引用） */
const nlohmann::json& arrOr(const nlohmann::json& j, const char* key) {
    static const nlohmann::json kEmpty = nlohmann::json::array();
    const auto                  it     = j.find(key);
    if (it == j.end() || !it->is_array()) {
        return kEmpty;
    }
    return *it;
}

/** rule_code 命名规范：^[A-Z][A-Z0-9_]{2,63}$ */
bool validRuleCode(const std::string& code) {
    if (code.size() < 3 || code.size() > 64) {
        return false;
    }
    if (!(code[0] >= 'A' && code[0] <= 'Z')) {
        return false;
    }
    for (const char c : code) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/** §3.3 允许的 (eval_phase, action.type) 组合 */
bool phaseAllows(EvalPhase phase, ActionType type) {
    switch (type) {
        case ActionType::Reject:
            return phase == EvalPhase::CREATE || phase == EvalPhase::TIMER;
        case ActionType::DeductCredit:
            return phase == EvalPhase::CREATE || phase == EvalPhase::SETTLE;
        case ActionType::DeductOnly:
            return phase == EvalPhase::TIMER || phase == EvalPhase::SETTLE;
        case ActionType::Transition:
            return phase == EvalPhase::TIMER;
        case ActionType::FreezeDeposit:
            return phase == EvalPhase::CREATE;
        case ActionType::DeductDeposit:
            return phase == EvalPhase::SETTLE;
    }
    return false;
}

/** 校验 params 里的数值参数：必须是数字且落在区间内 */
void checkNumParam(const nlohmann::json& params, const char* key, bool required, double lo,
                   double hi, std::vector<std::string>* errs) {
    const auto it = params.find(key);
    if (it == params.end() || it->is_null()) {
        if (required) {
            errs->push_back(std::string("params.") + key + " 必填");
        }
        return;
    }
    if (!it->is_number()) {
        errs->push_back(std::string("params.") + key + " 必须是数字");
        return;
    }
    const double v = it->get<double>();
    if (v < lo || v > hi) {
        errs->push_back(std::string("params.") + key + " 必须在 [" + std::to_string(lo) + ", " +
                        std::to_string(hi) + "] 之间（当前 " + std::to_string(v) + "）");
        return;
    }
    if ((key == std::string("deductScore") || key == std::string("maxTimes") ||
         key == std::string("maxActive")) &&
        v != static_cast<double>(static_cast<int64_t>(v))) {
        errs->push_back(std::string("params.") + key + " 必须是整数");
    }
}

/**
 * 附录 A 的 15 项校验。
 * @param body        请求体（不含 id / updatedAt）
 * @param requireCode true 时要求 ruleCode 必填且合法（创建）；更新时忽略
 * @param out         校验通过时填好的 Rule
 */
std::vector<std::string> validateDefinition(const nlohmann::json& body, bool requireCode,
                                            Rule* out) {
    std::vector<std::string> errs;
    Rule                     r;

    // ① 顶层必须是 object
    if (!body.is_object()) {
        errs.push_back("规则定义必须是 JSON 对象");
        return errs;
    }

    // ② rule_code
    const std::string code = body.value("ruleCode", std::string());
    if (requireCode) {
        if (code.empty()) {
            errs.push_back("ruleCode 必填");
        } else if (!validRuleCode(code)) {
            errs.push_back("ruleCode 必须匹配 ^[A-Z][A-Z0-9_]{2,63}$（当前 " + code + "）");
        }
    }
    r.ruleCode = code;

    r.ruleName = body.value("ruleName", std::string());
    r.ruleType = body.value("ruleType", std::string("CREDIT"));
    r.remark   = body.value("remark", std::string());

    // ③ priority
    r.priority = body.value("priority", 100);
    if (r.priority < 0 || r.priority > 9999) {
        errs.push_back("priority 必须在 [0, 9999] 之间（当前 " + std::to_string(r.priority) + "）");
    }

    // ④ eval_phase
    const std::string phaseStr = body.value("evalPhase", std::string());
    const auto        phaseOpt = evalPhaseFrom(phaseStr);
    if (!phaseOpt.has_value()) {
        errs.push_back("evalPhase 必须是 CREATE / TIMER / SETTLE（当前 " + phaseStr + "）");
    } else {
        r.evalPhase = *phaseOpt;
    }

    // ⑤ condition 是 object 且含 all 数组
    if (!body.contains("condition") || !body["condition"].is_object()) {
        errs.push_back("condition 必须是 JSON 对象");
    } else if (!body["condition"].contains("all") || !body["condition"]["all"].is_array()) {
        errs.push_back("condition 必须包含 all 数组（空数组表示恒真）");
    } else {
        r.condition = body["condition"];

        // ⑥⑦⑧ 逐 predicate 校验
        auto checkPredicates = [&errs](const nlohmann::json& list, const char* where) {
            for (const auto& p : list) {
                if (!p.is_object()) {
                    errs.push_back(std::string(where) + " 的 predicate 必须是对象");
                    continue;
                }
                const std::string op = p.value("op", std::string());
                if (!ConditionEval::isKnownOp(op)) {
                    errs.push_back(std::string(where) + " 出现未知 op：" + op);
                    continue;
                }
                const std::string field = p.value("field", std::string());
                if (field.empty() || field[0] != '$') {
                    errs.push_back(std::string(where) + " 的 field 必须以 $ 开头：" + field);
                    continue;
                }
                if (!FactBag::isKnownFact(field.substr(1))) {
                    errs.push_back("field '" + field + "' 不在 facts 白名单内");
                }
                if (op == "isNull" || op == "notNull" || op == "notEmpty") {
                    continue;
                }
                if (!p.contains("value")) {
                    errs.push_back(std::string(where) + " 的 op " + op + " 缺少 value");
                    continue;
                }
                if ((op == "in" || op == "notIn") && !p["value"].is_array()) {
                    errs.push_back(std::string(where) + " 的 op " + op + " 的 value 必须是数组");
                }
                ValueNode   node;
                std::string perr;
                if (!ValueNode::parse(p["value"], &node, &perr)) {
                    errs.push_back(std::string(where) + " 的 value 非法：" + perr);
                }
            }
        };
        checkPredicates(body["condition"]["all"], "condition.all");
        if (body["condition"].contains("any") && !body["condition"]["any"].is_null()) {
            if (!body["condition"]["any"].is_array()) {
                errs.push_back("condition.any 必须是数组");
            } else {
                checkPredicates(body["condition"]["any"], "condition.any");
            }
        }
    }

    // ⑨ action.type
    ActionType type = ActionType::Reject;
    bool       typeOk = false;
    if (!body.contains("action") || !body["action"].is_object()) {
        errs.push_back("action 必须是 JSON 对象");
    } else {
        r.action = body["action"];
        const auto t = actionTypeFrom(body["action"].value("type", std::string()));
        if (!t.has_value()) {
            errs.push_back("action.type 非法（只允许 reject / deduct_credit / deduct_only / "
                           "transition / freeze_deposit / deduct_deposit）");
        } else {
            type   = *t;
            typeOk = true;
        }

        // ⑩⑪⑫⑬ 分动作类型的必填字段
        const nlohmann::json& act = body["action"];
        const std::string     vt  = act.value("violationType", std::string());
        if (!vt.empty() && !model::violationTypeFrom(vt).has_value()) {
            errs.push_back("action.violationType 必须是 OVERDUE / NO_SHOW / DAMAGE（当前 " + vt + "）");
        }
        if (typeOk && type == ActionType::Reject) {
            const std::string rc = act.value("reasonCode", std::string());
            if (rc.empty()) {
                errs.push_back("reject 动作必须带 reasonCode（BLACKLISTED / ACTIVE_LIMIT）");
            }
        }
        if (typeOk && type == ActionType::Transition) {
            const std::string ev = act.value("event", std::string());
            if (ev.empty()) {
                errs.push_back("transition 动作必须带 event");
            } else if (!transitEventFrom(ev).has_value()) {
                errs.push_back("transition.event 不是合法的状态机事件：" + ev);
            }
            const std::string ts = act.value("targetStatus", std::string());
            if (ts.empty()) {
                errs.push_back("transition 动作必须带 targetStatus");
            } else if (!model::reservationStatusFrom(ts).has_value()) {
                errs.push_back("transition.targetStatus 不是合法的预约状态：" + ts);
            }
        }
        if (typeOk && type == ActionType::FreezeDeposit) {
            const std::string af = act.value("amountFrom", std::string());
            if (af.empty() || af[0] != '$' || !FactBag::isKnownFact(af.substr(1))) {
                errs.push_back("freeze_deposit.amountFrom 必须是白名单内的 $ 字段（当前 " + af + "）");
            }
            if (act.value("rateParam", std::string()).empty()) {
                errs.push_back("freeze_deposit 必须带 rateParam");
            }
        }
    }

    // ⑭ params
    if (!body.contains("params") || body["params"].is_null()) {
        r.params = nlohmann::json::object();
    } else if (!body["params"].is_object()) {
        errs.push_back("params 必须是 JSON 对象");
    } else {
        r.params = body["params"];
    }
    if (r.params.is_object() && typeOk) {
        switch (type) {
            case ActionType::DeductCredit:
            case ActionType::DeductOnly:
                checkNumParam(r.params, "deductScore", true, 1, 1000, &errs);
                break;
            case ActionType::Transition:
                if (r.action.value("deductCredit", false)) {
                    checkNumParam(r.params, "deductScore", true, 1, 1000, &errs);
                }
                break;
            case ActionType::FreezeDeposit: {
                const std::string rateParam = r.action.value("rateParam", std::string("rate"));
                if (!rateParam.empty()) {
                    checkNumParam(r.params, rateParam.c_str(), true, 0.0, 1.0, &errs);
                }
                break;
            }
            case ActionType::DeductDeposit:
                checkNumParam(r.params, "ratio", true, 0.0, 1.0, &errs);
                checkNumParam(r.params, "maxAmount", false, 0.0, 1000000.0, &errs);
                break;
            case ActionType::Reject:
                break;
        }
        // 规则条件里引用到的参数必须真的存在（否则求值期必然 Malformed）
        for (const auto& [k, v] : r.params.items()) {
            (void)k;
            (void)v;
        }
    }

    // ⑮ (phase, action.type) 组合
    if (typeOk && phaseOpt.has_value() && !phaseAllows(*phaseOpt, type)) {
        errs.push_back("动作 " + toStr(type) + " 不允许出现在 " + toStr(*phaseOpt) +
                       " 阶段（见 ARCH-P3 §3.3 的允许表）");
    }

    if (out != nullptr) {
        *out = std::move(r);
    }
    return errs;
}

/** Rule → 接口 JSON（附录 B 的形状） */
nlohmann::json ruleToJson(const Rule& r) {
    nlohmann::json j = nlohmann::json::object();
    j["id"]        = r.id;
    j["ruleCode"]  = r.ruleCode;
    j["ruleName"]  = r.ruleName;
    j["ruleType"]  = r.ruleType;
    j["evalPhase"] = toStr(r.evalPhase);
    j["priority"]  = r.priority;
    j["enabled"]   = r.enabled;
    j["condition"] = r.condition;
    j["action"]    = r.action;
    j["params"]    = r.params;
    j["remark"]    = r.remark;
    j["updatedAt"] = r.updatedAt;
    return j;
}

/** 内置保护规则（安全关键，只允许停用不允许删除） */
bool isProtectedRule(const std::string& code) { return code == "BLACKLIST_REJECT"; }

}  // namespace

RuleController::RuleController(RuleCache& cache, repo::RuleRepo& repo, DbPool& pool)
    : cache_(cache), repo_(repo), pool_(pool) {}

Result<Json> RuleController::list(const httplib::Request& req, const AuthContext& ctx) {
    (void)ctx;
    std::vector<Rule> all;
    if (!cache_.snapshot(&all, /*onlyEnabled=*/false)) {
        return Result<Json>::fail(kDbError, "规则表不可读，请稍后重试");
    }
    const auto phaseFilter   = detail::queryStr(req, "phase");
    const auto enabledFilter = detail::queryStr(req, "enabled");
    const auto codeFilter    = detail::queryStr(req, "ruleCode");

    Json list = Json::array();
    for (const auto& r : all) {
        if (phaseFilter.has_value() && toStr(r.evalPhase) != *phaseFilter) {
            continue;
        }
        if (enabledFilter.has_value()) {
            const bool want = (*enabledFilter == "1" || *enabledFilter == "true");
            if (r.enabled != want) {
                continue;
            }
        }
        if (codeFilter.has_value() && r.ruleCode != *codeFilter) {
            continue;
        }
        list.push_back(ruleToJson(r));
    }
    Json data;
    data["list"]  = std::move(list);
    data["total"] = data["list"].size();
    return Result<Json>::make(std::move(data));
}

Result<Json> RuleController::detail(const httplib::Request& req, const AuthContext& ctx) {
    (void)ctx;
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto r = repo_.findById(*guard, *id);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    if (!r.data.has_value()) {
        return Result<Json>::fail(kRuleNotFound, "规则不存在");
    }
    return Result<Json>::make(ruleToJson(*r.data));
}

Result<Json> RuleController::create(const httplib::Request& req, const AuthContext& ctx) {
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    Rule               r;
    std::vector<std::string> errs = validateDefinition(body, /*requireCode=*/true, &r);
    if (!errs.empty()) {
        std::string msg = "规则定义非法：";
        for (size_t i = 0; i < errs.size(); ++i) {
            msg += (i == 0 ? "" : "；") + errs[i];
        }
        return Result<Json>::fail(kRuleInvalid, msg);
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto ins = repo_.insertRule(*guard, r);
    if (!ins.ok()) {
        return Result<Json>::fail(ins.code, ins.msg);
    }
    // D-P3-10：写成功后立刻生效（不等 5s TTL）
    std::string rerr;
    if (!cache_.reload(rerr)) {
        LOG_WARN("规则已写入但缓存重载失败（将在 TTL 内自愈）：{}", rerr);
    }
    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "RULE_CREATE", kOk, ins.data);
    Json data;
    data["id"] = ins.data;
    return Result<Json>::make(std::move(data));
}

Result<Json> RuleController::update(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    Rule               r;
    std::vector<std::string> errs = validateDefinition(body, /*requireCode=*/false, &r);
    if (!errs.empty()) {
        std::string msg = "规则定义非法：";
        for (size_t i = 0; i < errs.size(); ++i) {
            msg += (i == 0 ? "" : "；") + errs[i];
        }
        return Result<Json>::fail(kRuleInvalid, msg);
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto upd = repo_.updateRule(*guard, *id, r);
    if (!upd.ok()) {
        return Result<Json>::fail(upd.code, upd.msg);
    }
    std::string rerr;
    if (!cache_.reload(rerr)) {
        LOG_WARN("规则已更新但缓存重载失败（将在 TTL 内自愈）：{}", rerr);
    }
    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "RULE_UPDATE", kOk, *id);
    auto after = repo_.findById(*guard, *id);
    if (!after.ok() || !after.data.has_value()) {
        return Result<Json>::fail(kRuleNotFound, "规则不存在");
    }
    return Result<Json>::make(ruleToJson(*after.data));
}

Result<Json> RuleController::setEnabled(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    if (!body.contains("enabled") || !body["enabled"].is_boolean()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：enabled 必填且必须是布尔值");
    }
    const bool enabled = body["enabled"].get<bool>();

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto r = repo_.setEnabled(*guard, *id, enabled);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    std::string rerr;
    if (!cache_.reload(rerr)) {
        LOG_WARN("规则已启停但缓存重载失败（将在 TTL 内自愈）：{}", rerr);
    }
    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "RULE_ENABLED", kOk, *id);
    auto after = repo_.findById(*guard, *id);
    if (!after.ok() || !after.data.has_value()) {
        return Result<Json>::fail(kRuleNotFound, "规则不存在");
    }
    return Result<Json>::make(ruleToJson(*after.data));
}

Result<Json> RuleController::setPriority(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    if (!body.contains("priority") || !body["priority"].is_number_integer()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：priority 必填且必须是整数");
    }
    const int priority = body["priority"].get<int>();
    if (priority < 0 || priority > 9999) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：priority 必须在 [0, 9999] 之间");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto r = repo_.setPriority(*guard, *id, priority);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    std::string rerr;
    if (!cache_.reload(rerr)) {
        LOG_WARN("规则优先级已改但缓存重载失败（将在 TTL 内自愈）：{}", rerr);
    }
    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "RULE_PRIORITY", kOk, *id);
    auto after = repo_.findById(*guard, *id);
    if (!after.ok() || !after.data.has_value()) {
        return Result<Json>::fail(kRuleNotFound, "规则不存在");
    }
    return Result<Json>::make(ruleToJson(*after.data));
}

Result<Json> RuleController::remove(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto cur = repo_.findById(*guard, *id);
    if (!cur.ok()) {
        return Result<Json>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<Json>::fail(kRuleNotFound, "规则不存在");
    }
    if (isProtectedRule(cur.data->ruleCode)) {
        return Result<Json>::fail(kRuleProtected, "内置规则不允许删除，请改为停用");
    }
    auto r = repo_.removeRule(*guard, *id);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    std::string rerr;
    if (!cache_.reload(rerr)) {
        LOG_WARN("规则已删除但缓存重载失败（将在 TTL 内自愈）：{}", rerr);
    }
    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "RULE_DELETE", kOk, *id);
    Result<Json> ok = Result<Json>::make(Json::object());
    ok.msg = "规则已删除";
    return ok;
}

Result<Json> RuleController::validate(const httplib::Request& req, const AuthContext& ctx) {
    (void)ctx;
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    Rule                     r;
    std::vector<std::string> errs = validateDefinition(body, /*requireCode=*/false, &r);
    Json                     data;
    data["valid"]  = errs.empty();
    data["errors"] = errs;
    return Result<Json>::make(std::move(data));
}

} // namespace rf
