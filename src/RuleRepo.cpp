// =============================================================================
// src/RuleRepo.cpp  —  rule_config 读取实现（ARCH-P3 §2.1 / §1.3）
//
// 两个实现要点：
//   1) condition_json / action_json / params_json 是 MySQL JSON 列，取出来时
//      用 CAST(... AS CHAR) 保证拿到文本（否则部分驱动给二进制）；
//   2) 单行 JSON 畸形只跳过该行并记录原因 —— 一条坏规则不该让整个引擎停摆，
//      但规则列表接口（T08）要把这些行如实报出来。
// =============================================================================
#include "RuleRepo.h"

#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf::repo {

namespace {

/** 列链表（列序与 toRule 严格一致） */
constexpr const char* kRuleSelect =
    "SELECT id, rule_code, rule_name, rule_type, eval_phase, "
    "CAST(condition_json AS CHAR) AS condition_json, "
    "CAST(action_json AS CHAR) AS action_json, "
    "CAST(IFNULL(params_json, JSON_OBJECT()) AS CHAR) AS params_json, "
    "priority, enabled, remark, "
    "DATE_FORMAT(updated_at, '%Y-%m-%dT%H:%i:%s') AS updated_at "
    "FROM `rule_config` ";

std::string cellStr(const Row& row, size_t i) {
    return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
}

int64_t cellI64(const Row& row, size_t i) {
    const std::string v = cellStr(row, i);
    if (v.empty()) {
        return 0;
    }
    return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
}

/** 安全的 JSON 文本 → object；失败返回 false 并写原因 */
bool parseObject(const std::string& text, const char* field, nlohmann::json* out,
                 std::string* err) {
    if (text.empty()) {
        *out = nlohmann::json::object();
        return true;
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (!j.is_object()) {
            *err = std::string(field) + " 不是 JSON object";
            return false;
        }
        *out = j;
        return true;
    } catch (const std::exception& e) {
        *err = std::string(field) + " 不是合法 JSON：" + e.what();
        return false;
    }
}

/**
 * 行 → Rule。本函数不校验语义（语义校验属 T08 的 3024 清单），
 * 只保证「类型正确、能被 RuleEngine 安全使用」；语义非法的规则由引擎在
 * 求值时按 D-P3-5 的 fail 策略处理（这也让"有人直接改 DB 写坏规则"能自愈）。
 */
bool toRule(const Row& row, Rule* out, std::string* err) {
    out->id       = cellI64(row, 0);
    out->ruleCode = cellStr(row, 1);
    out->ruleName = cellStr(row, 2);
    out->ruleType = cellStr(row, 3);

    const std::string phase = cellStr(row, 4);
    const auto        ph    = evalPhaseFrom(phase);
    if (!ph.has_value()) {
        *err = "eval_phase 非法：" + phase;
        return false;
    }
    out->evalPhase = *ph;

    if (!parseObject(cellStr(row, 5), "condition_json", &out->condition, err)) {
        return false;
    }
    if (!parseObject(cellStr(row, 6), "action_json", &out->action, err)) {
        return false;
    }
    if (!parseObject(cellStr(row, 7), "params_json", &out->params, err)) {
        return false;
    }

    out->priority  = static_cast<int>(cellI64(row, 8));
    out->enabled   = cellI64(row, 9) != 0;
    out->remark    = cellStr(row, 10);
    out->updatedAt = cellStr(row, 11);

    if (out->ruleCode.empty()) {
        *err = "rule_code 为空";
        return false;
    }
    return true;
}

}  // namespace

Result<std::vector<Rule>> RuleRepo::loadAll(DbConn& db, std::vector<std::string>* skipped) {
    try {
        Stmt st(db, std::string(kRuleSelect) +
                        "ORDER BY priority ASC, rule_code ASC");
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<Rule>>::fail(rs.code, rs.msg);
        }
        std::vector<Rule> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            Rule        r;
            std::string err;
            if (!toRule(row, &r, &err)) {
                const std::string code = cellStr(row, 1);
                if (skipped != nullptr) {
                    skipped->push_back((code.empty() ? std::string("<no-code>") : code) + ": " + err);
                }
                continue;   // 坏行跳过，不影响整表
            }
            out.push_back(std::move(r));
        }
        return Result<std::vector<Rule>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<Rule>>::fail(kDbError, std::string("载入规则失败：") + e.what());
    }
}

Result<RuleFingerprint> RuleRepo::fingerprint(DbConn& db) {
    try {
        Stmt st(db,
                "SELECT COUNT(*) AS n, "
                "COALESCE(DATE_FORMAT(MAX(updated_at), '%Y-%m-%dT%H:%i:%s'), '1970-01-01T00:00:00') AS ts, "
                "COALESCE(SUM(enabled), 0) AS en "
                "FROM `rule_config`");
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<RuleFingerprint>::fail(rs.code, rs.msg);
        }
        RuleFingerprint fp;
        if (!rs.data.empty()) {
            fp.count        = cellI64(rs.data.front(), 0);
            fp.maxUpdatedAt = cellStr(rs.data.front(), 1);
            fp.enabledSum   = cellI64(rs.data.front(), 2);
        }
        return Result<RuleFingerprint>::make(fp);
    } catch (const std::exception& e) {
        return Result<RuleFingerprint>::fail(kDbError,
                                             std::string("读取规则指纹失败：") + e.what());
    }
}

Result<std::optional<Rule>> RuleRepo::findById(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<std::optional<Rule>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kRuleSelect) + "WHERE id = ?");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<Rule>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<Rule>>::make(std::nullopt);
        }
        Rule        r;
        std::string err;
        if (!toRule(rs.data.front(), &r, &err)) {
            return Result<std::optional<Rule>>::fail(kRuleInvalid, "规则定义非法：" + err);
        }
        return Result<std::optional<Rule>>::make(std::optional<Rule>(std::move(r)));
    } catch (const std::exception& e) {
        return Result<std::optional<Rule>>::fail(kDbError,
                                                 std::string("查询规则失败：") + e.what());
    }
}

Result<std::optional<Rule>> RuleRepo::findByCode(DbConn& db, const std::string& ruleCode) {
    if (ruleCode.empty()) {
        return Result<std::optional<Rule>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kRuleSelect) + "WHERE rule_code = ?");
        st.bindStr(ruleCode);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<Rule>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<Rule>>::make(std::nullopt);
        }
        Rule        r;
        std::string err;
        if (!toRule(rs.data.front(), &r, &err)) {
            return Result<std::optional<Rule>>::fail(kRuleInvalid, "规则定义非法：" + err);
        }
        return Result<std::optional<Rule>>::make(std::optional<Rule>(std::move(r)));
    } catch (const std::exception& e) {
        return Result<std::optional<Rule>>::fail(kDbError,
                                                 std::string("按编码查询规则失败：") + e.what());
    }
}

// ============================================================================
// P3 T08：CRUD
// ============================================================================
namespace {

/** JSON 列绑定：nlohmann → 文本（MySQL JSON 列接受字符串字面量，非法 JSON 由 3024 前置拦住） */
std::string dumpJson(const nlohmann::json& j) { return j.dump(); }

}  // namespace

Result<int64_t> RuleRepo::insertRule(DbConn& db, const Rule& r) {
    if (r.ruleCode.empty()) {
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：rule_code 不能为空");
    }
    try {
        Stmt st(db,
                "INSERT INTO `rule_config` "
                "(rule_code, rule_name, rule_type, eval_phase, condition_json, action_json, "
                " params_json, priority, enabled, remark) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        st.bindStr(r.ruleCode)
            .bindStr(r.ruleName)
            .bindStr(r.ruleType)
            .bindStr(toStr(r.evalPhase))
            .bindStr(dumpJson(r.condition))
            .bindStr(dumpJson(r.action))
            .bindStr(dumpJson(r.params))
            .bindI64(static_cast<int64_t>(r.priority))
            .bindI64(r.enabled ? 1 : 0)
            .bindStr(r.remark);
        auto rr = st.exec();
        if (!rr.ok()) {
            if (st.isDupEntry()) {
                return Result<int64_t>::fail(kRuleCodeDup, "规则编码已存在");
            }
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(static_cast<int64_t>(rr.data.insertId));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("新增规则失败：") + e.what());
    }
}

VoidResult RuleRepo::updateRule(DbConn& db, int64_t id, const Rule& r) {
    if (id <= 0) {
        return VoidResult::fail(kRuleNotFound, "规则不存在");
    }
    try {
        auto exists = findById(db, id);
        if (!exists.ok()) {
            return VoidResult::fail(exists.code, exists.msg);
        }
        if (!exists.data.has_value()) {
            return VoidResult::fail(kRuleNotFound, "规则不存在");
        }
        Stmt st(db,
                "UPDATE `rule_config` SET rule_name = ?, rule_type = ?, eval_phase = ?, "
                "condition_json = ?, action_json = ?, params_json = ?, priority = ?, "
                "enabled = ?, remark = ? WHERE id = ?");
        st.bindStr(r.ruleName)
            .bindStr(r.ruleType)
            .bindStr(toStr(r.evalPhase))
            .bindStr(dumpJson(r.condition))
            .bindStr(dumpJson(r.action))
            .bindStr(dumpJson(r.params))
            .bindI64(static_cast<int64_t>(r.priority))
            .bindI64(r.enabled ? 1 : 0)
            .bindStr(r.remark)
            .bindI64(id);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("更新规则失败：") + e.what());
    }
}

VoidResult RuleRepo::setEnabled(DbConn& db, int64_t id, bool enabled) {
    try {
        Stmt st(db, "UPDATE `rule_config` SET enabled = ? WHERE id = ?");
        st.bindI64(enabled ? 1 : 0).bindI64(id);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        if (rr.data.affectedRows == 0) {
            // enabled 本来就等于目标值时 affectedRows 也是 0，需再确认存在性
            auto exists = findById(db, id);
            if (!exists.ok()) {
                return VoidResult::fail(exists.code, exists.msg);
            }
            if (!exists.data.has_value()) {
                return VoidResult::fail(kRuleNotFound, "规则不存在");
            }
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("启停规则失败：") + e.what());
    }
}

VoidResult RuleRepo::setPriority(DbConn& db, int64_t id, int priority) {
    try {
        Stmt st(db, "UPDATE `rule_config` SET priority = ? WHERE id = ?");
        st.bindI64(static_cast<int64_t>(priority)).bindI64(id);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        if (rr.data.affectedRows == 0) {
            auto exists = findById(db, id);
            if (!exists.ok()) {
                return VoidResult::fail(exists.code, exists.msg);
            }
            if (!exists.data.has_value()) {
                return VoidResult::fail(kRuleNotFound, "规则不存在");
            }
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("调整规则优先级失败：") + e.what());
    }
}

VoidResult RuleRepo::removeRule(DbConn& db, int64_t id) {
    if (id <= 0) {
        return VoidResult::fail(kRuleNotFound, "规则不存在");
    }
    try {
        auto exists = findById(db, id);
        if (!exists.ok()) {
            return VoidResult::fail(exists.code, exists.msg);
        }
        if (!exists.data.has_value()) {
            return VoidResult::fail(kRuleNotFound, "规则不存在");
        }
        Stmt st(db, "DELETE FROM `rule_config` WHERE id = ?");
        st.bindI64(id);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("删除规则失败：") + e.what());
    }
}

} // namespace rf::repo
