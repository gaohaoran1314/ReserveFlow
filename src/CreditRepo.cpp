// =============================================================================
// src/CreditRepo.cpp  —  credit_log / user 信用列的实现（ARCH-P3 §6）
// =============================================================================
#include "CreditRepo.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf::repo {

namespace {

/** credit_log 列清单（14 列），列序必须与 model::CreditLog::fromRow 一致 */
constexpr const char* kCreditSelect =
    "SELECT id, user_id, IFNULL(reservation_id, 0) AS reservation_id, change_score, "
    "score_before, score_after, status_before, status_after, reason_type, "
    "IFNULL(rule_code, '') AS rule_code, source_ref, IFNULL(operator_id, 0) AS operator_id, "
    "remark, DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%s') AS created_at "
    "FROM `credit_log` ";

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

}  // namespace

Result<std::pair<int, std::string>> CreditRepo::lockUser(DbConn& db, int64_t userId) {
    if (userId <= 0) {
        return Result<std::pair<int, std::string>>::fail(kInvalidParam,
                                                         "参数校验失败：用户 id 非法");
    }
    try {
        Stmt st(db, "SELECT credit_score, status FROM `user` WHERE id = ? FOR UPDATE");
        st.bindI64(userId);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::pair<int, std::string>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::pair<int, std::string>>::fail(kInvalidCredential, "账号不存在");
        }
        const int         score  = static_cast<int>(cellI64(rs.data.front(), 0));
        const std::string status = cellStr(rs.data.front(), 1);
        return Result<std::pair<int, std::string>>::make(std::make_pair(score, status));
    } catch (const std::exception& e) {
        return Result<std::pair<int, std::string>>::fail(
            kDbError, std::string("读取信用分失败：") + e.what());
    }
}

VoidResult CreditRepo::updateScoreAndStatus(DbConn& db, int64_t userId, int score,
                                            const std::string& status) {
    if (userId <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：用户 id 非法");
    }
    try {
        // 两列的值都由 C++ 算好传进来，SQL 里没有 credit_score - ? 这类算式。
        // 这是 D-P3-9 的落地点：避免 MySQL「SET 从左到右求值」把 delta 应用两次。
        Stmt st(db, "UPDATE `user` SET credit_score = ?, status = ? WHERE id = ?");
        st.bindI64(static_cast<int64_t>(score)).bindStr(status).bindI64(userId);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("更新信用分失败：") + e.what());
    }
}

Result<int64_t> CreditRepo::insertLog(DbConn& db, const model::CreditLog& log) {
    if (log.userId <= 0 || log.sourceRef.empty() || log.reasonType.empty()) {
        return Result<int64_t>::fail(kInvalidParam,
                                     "参数校验失败：user_id / source_ref / reason_type 不能为空");
    }
    try {
        Stmt st(db,
                "INSERT INTO `credit_log` "
                "(user_id, reservation_id, change_score, score_before, score_after, "
                " status_before, status_after, reason_type, rule_code, source_ref, "
                " operator_id, remark) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        st.bindI64(log.userId);
        if (log.reservationId > 0) {
            st.bindI64(log.reservationId);
        } else {
            st.bindNull();   // 手工/自动调整没有关联单据
        }
        st.bindI64(static_cast<int64_t>(log.changeScore))
            .bindI64(static_cast<int64_t>(log.scoreBefore))
            .bindI64(static_cast<int64_t>(log.scoreAfter))
            .bindStr(log.statusBefore)
            .bindStr(log.statusAfter)
            .bindStr(log.reasonType);
        if (log.ruleCode.empty()) {
            st.bindNull();   // 非规则触发
        } else {
            st.bindStr(log.ruleCode);
        }
        st.bindStr(log.sourceRef);
        if (log.operatorId > 0) {
            st.bindI64(log.operatorId);
        } else {
            st.bindNull();   // 系统触发
        }
        st.bindStr(log.remark);

        auto rr = st.exec();
        if (!rr.ok()) {
            if (st.isDupEntry()) {
                // uk_credit_source 命中 → 这次变动已经应用过（幂等）
                return Result<int64_t>::fail(kSlotUnavailable, "该信用变动已应用过");
            }
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(static_cast<int64_t>(rr.data.insertId));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("写信用明细失败：") + e.what());
    }
}

Result<std::vector<model::CreditLog>> CreditRepo::listLogs(DbConn& db, int64_t userId, int page,
                                                           int size,
                                                           const std::string& reasonType) {
    if (userId <= 0) {
        return Result<std::vector<model::CreditLog>>::make(std::vector<model::CreditLog>{});
    }
    const int p    = (page < 1) ? 1 : page;
    const int sz   = (size < 1) ? 10 : (size > 100 ? 100 : size);
    const int64_t offset = static_cast<int64_t>(p - 1) * static_cast<int64_t>(sz);
    try {
        std::string sql = std::string(kCreditSelect) + "WHERE user_id = ?";
        if (!reasonType.empty()) {
            sql += " AND reason_type = ?";
        }
        sql += " ORDER BY id DESC LIMIT ? OFFSET ?";

        Stmt st(db, sql);
        st.bindI64(userId);
        if (!reasonType.empty()) {
            st.bindStr(reasonType);
        }
        st.bindI64(static_cast<int64_t>(sz)).bindI64(offset);

        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<model::CreditLog>>::fail(rs.code, rs.msg);
        }
        std::vector<model::CreditLog> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(model::CreditLog::fromRow(row));
        }
        return Result<std::vector<model::CreditLog>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<model::CreditLog>>::fail(
            kDbError, std::string("查询信用明细失败：") + e.what());
    }
}

Result<int64_t> CreditRepo::countLogs(DbConn& db, int64_t userId, const std::string& reasonType) {
    if (userId <= 0) {
        return Result<int64_t>::make(0);
    }
    try {
        std::string sql = "SELECT COUNT(*) AS c FROM `credit_log` WHERE user_id = ?";
        if (!reasonType.empty()) {
            sql += " AND reason_type = ?";
        }
        Stmt st(db, sql);
        st.bindI64(userId);
        if (!reasonType.empty()) {
            st.bindStr(reasonType);
        }
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<int64_t>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<int64_t>::make(0);
        }
        return Result<int64_t>::make(cellI64(rs.data.front(), 0));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("统计信用明细失败：") + e.what());
    }
}

Result<std::vector<std::pair<int64_t, std::pair<int, std::string>>>> CreditRepo::findInconsistent(
    DbConn& db, int enterScore, int exitScore, int limit) {
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    try {
        // 走 idx_user_status：只扫「状态与分数不符」的两类行（§6.2）
        Stmt st(db,
                "SELECT id, credit_score, status FROM `user` "
                "WHERE (status = 'NORMAL'    AND credit_score <  ?) "
                "   OR (status = 'BLACKLIST' AND credit_score >= ?) "
                "LIMIT ?");
        st.bindI64(static_cast<int64_t>(enterScore))
            .bindI64(static_cast<int64_t>(exitScore))
            .bindI64(static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<std::pair<int64_t, std::pair<int, std::string>>>>::fail(
                rs.code, rs.msg);
        }
        std::vector<std::pair<int64_t, std::pair<int, std::string>>> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.emplace_back(cellI64(row, 0),
                             std::make_pair(static_cast<int>(cellI64(row, 1)), cellStr(row, 2)));
        }
        return Result<std::vector<std::pair<int64_t, std::pair<int, std::string>>>>::make(
            std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<std::pair<int64_t, std::pair<int, std::string>>>>::fail(
            kDbError, std::string("扫描信用状态不一致失败：") + e.what());
    }
}

} // namespace rf::repo
