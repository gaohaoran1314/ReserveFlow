// =============================================================================
// src/repo/AuditRepo.cpp  —  status_log 参数化实现
// 对应 ARCH-P0 §3.9 / D-12 / §8.2（列语义：from_status 创建场景为空串，
// rule_code 手工操作为 NULL）
// =============================================================================
#include "AuditRepo.h"

#include <cstdlib>
#include <string>
#include <utility>

#include "Base.h"

namespace rf::repo {

namespace {

/** 列序必须与 model::StatusLogEntry::fromRow 的契约一致 */
constexpr const char* kStatusLogSelect =
    "SELECT id, business_type, business_id, from_status, to_status, operator_id, reason, "
    "rule_code, "
    "DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%s') AS created_at "
    "FROM `status_log` ";

}  // namespace

VoidResult AuditRepo::insertStatusLog(DbConn& db, const StatusLogEntry& e) {
    if (e.businessType.empty() || e.businessId <= 0) {
        return VoidResult::fail(kInvalidParam,
                                "参数校验失败：状态留痕的 business_type / business_id 非法");
    }
    try {
        Stmt st(db,
                "INSERT INTO `status_log` "
                "(business_type, business_id, from_status, to_status, operator_id, reason, "
                " rule_code) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)");
        st.bindStr(e.businessType)
            .bindI64(e.businessId)
            .bindStr(e.fromStatus)
            .bindStr(e.toStatus);
        if (e.operatorId > 0) {
            st.bindI64(e.operatorId);
        } else {
            st.bindNull();   // operator_id 允许 NULL（系统自动流转场景）
        }
        st.bindStr(e.reason);
        if (e.ruleCode.has_value() && !e.ruleCode->empty()) {
            st.bindStr(*e.ruleCode);
        } else {
            st.bindNull();   // 手工操作 → NULL（D-12）
        }

        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& ex) {
        return VoidResult::fail(kDbError, std::string("写状态留痕失败：") + ex.what());
    }
}

Result<std::vector<StatusLogEntry>> AuditRepo::listByBusiness(DbConn& db, const std::string& type,
                                                             int64_t businessId, int limit) {
    if (type.empty() || businessId <= 0) {
        return Result<std::vector<StatusLogEntry>>::make(std::vector<StatusLogEntry>{});
    }
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    try {
        Stmt st(db, std::string(kStatusLogSelect) +
                        "WHERE business_type = ? AND business_id = ? ORDER BY id DESC LIMIT ?");
        st.bindStr(type).bindI64(businessId).bindI64(static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<StatusLogEntry>>::fail(rs.code, rs.msg);
        }
        std::vector<StatusLogEntry> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(StatusLogEntry::fromRow(row));
        }
        return Result<std::vector<StatusLogEntry>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<StatusLogEntry>>::fail(
            kDbError, std::string("查询状态留痕失败：") + e.what());
    }
}

} // namespace rf::repo
