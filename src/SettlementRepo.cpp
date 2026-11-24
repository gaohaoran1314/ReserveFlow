// =============================================================================
// src/SettlementRepo.cpp  —  押金 / 违约 / 信用分 的 SQL 实现
// 对应 ARCH-P2 §3.3 / §6 / D-P2-6
//
// 关键点：
//   1) 1062 在两个方向语义相反：押金流水「已记账 = 成功」（幂等），
//      违约记录「已扣过 = 跳过扣分」（返回 3005 作为内部信号）；
//   2) 信用分扣减用 GREATEST(0, ...) 在数据库侧做下限保护，应用层无需再读一次；
//   3) 所有时间列走 DATE_FORMAT 输出 ISO，与既有实体的时间口径一致。
// =============================================================================
#include "SettlementRepo.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf::repo {

namespace {

/** deposit_record 列清单（9 列），列序必须与 model::DepositRecord::fromRow 一致 */
constexpr const char* kDepositSelect =
    "SELECT id, reservation_id, user_id, amount, record_type, status, transaction_no, remark, "
    "DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%s') AS created_at "
    "FROM `deposit_record` ";

/** violation_record 列清单（9 列），列序必须与 model::ViolationRecord::fromRow 一致 */
constexpr const char* kViolationSelect =
    "SELECT id, user_id, reservation_id, violation_type, deduct_score, deduct_seq, "
    "operator_id, remark, DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%s') AS created_at "
    "FROM `violation_record` ";

std::string cellStr(const Row& row, size_t i) {
    return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
}

double cellDbl(const Row& row, size_t i) {
    const std::string v = cellStr(row, i);
    if (v.empty()) {
        return 0.0;
    }
    return std::strtod(v.c_str(), nullptr);
}

int64_t cellI64(const Row& row, size_t i) {
    const std::string v = cellStr(row, i);
    if (v.empty()) {
        return 0;
    }
    return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
}

}  // namespace

VoidResult SettlementRepo::insertDeposit(DbConn& db, const model::DepositRecord& d) {
    if (d.reservationId <= 0 || d.userId <= 0 || d.transactionNo.empty()) {
        return VoidResult::fail(kInvalidParam,
                                "参数校验失败：押金流水的 reservationId / userId / 流水号 非法");
    }
    try {
        Stmt st(db,
                "INSERT INTO `deposit_record` "
                "(reservation_id, user_id, amount, record_type, status, transaction_no, remark) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)");
        st.bindI64(d.reservationId)
            .bindI64(d.userId)
            .bindDouble(d.amount)
            .bindStr(d.recordType)
            .bindStr(d.status.empty() ? std::string("SUCCESS") : d.status)
            .bindStr(d.transactionNo)
            .bindStr(d.remark);
        auto rr = st.exec();
        if (!rr.ok()) {
            if (st.isDupEntry()) {
                // uk_deposit_txn 命中 → 这笔账已经记过。幂等成功（D-P2-6 第 3 层）
                return VoidResult::make(Empty{});
            }
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("写押金流水失败：") + e.what());
    }
}

Result<std::vector<model::DepositRecord>> SettlementRepo::listDeposits(DbConn& db,
                                                                      int64_t reservationId) {
    if (reservationId <= 0) {
        return Result<std::vector<model::DepositRecord>>::make(
            std::vector<model::DepositRecord>{});
    }
    try {
        Stmt st(db, std::string(kDepositSelect) + "WHERE reservation_id = ? ORDER BY id ASC");
        st.bindI64(reservationId);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<model::DepositRecord>>::fail(rs.code, rs.msg);
        }
        std::vector<model::DepositRecord> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(model::DepositRecord::fromRow(row));
        }
        return Result<std::vector<model::DepositRecord>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<model::DepositRecord>>::fail(
            kDbError, std::string("查询押金流水失败：") + e.what());
    }
}

Result<double> SettlementRepo::sumDepositByType(DbConn& db, int64_t reservationId,
                                                const std::string& recordType) {
    if (reservationId <= 0 || recordType.empty()) {
        return Result<double>::make(0.0);
    }
    try {
        Stmt st(db,
                "SELECT IFNULL(SUM(amount), 0) AS total FROM `deposit_record` "
                "WHERE reservation_id = ? AND record_type = ?");
        st.bindI64(reservationId).bindStr(recordType);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<double>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<double>::make(0.0);
        }
        return Result<double>::make(cellDbl(rs.data.front(), 0));
    } catch (const std::exception& e) {
        return Result<double>::fail(kDbError, std::string("汇总押金失败：") + e.what());
    }
}

Result<int64_t> SettlementRepo::insertViolation(DbConn& db, const model::ViolationRecord& v) {
    if (v.userId <= 0 || v.violationType.empty()) {
        return Result<int64_t>::fail(kInvalidParam,
                                     "参数校验失败：违约记录的 userId / violationType 非法");
    }
    try {
        Stmt st(db,
                "INSERT INTO `violation_record` "
                "(user_id, reservation_id, violation_type, deduct_score, deduct_seq, "
                " operator_id, remark) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)");
        st.bindI64(v.userId);
        if (v.reservationId > 0) {
            st.bindI64(v.reservationId);
        } else {
            st.bindNull();   // 无关联单据（P3 管理员手工扣分场景）
        }
        st.bindStr(v.violationType)
            .bindI64(static_cast<int64_t>(v.deductScore))
            .bindI64(static_cast<int64_t>(v.deductSeq));
        if (v.operatorId > 0) {
            st.bindI64(v.operatorId);
        } else {
            st.bindNull();   // 系统自动认定
        }
        st.bindStr(v.remark);

        auto rr = st.exec();
        if (!rr.ok()) {
            if (st.isDupEntry()) {
                // uk_violation_dedup 命中 → 本周期已扣过。用 3005 当内部信号（D-P2-6 第 2 层）
                return Result<int64_t>::fail(kSlotUnavailable, "本周期已扣过分，跳过");
            }
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(static_cast<int64_t>(rr.data.insertId));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("写违约记录失败：") + e.what());
    }
}

VoidResult SettlementRepo::deductCredit(DbConn& db, int64_t userId, int score) {
    if (userId <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：用户 id 非法");
    }
    if (score <= 0) {
        return VoidResult::make(Empty{});   // 扣 0 分等于不扣
    }
    try {
        Stmt st(db, "UPDATE `user` SET credit_score = GREATEST(0, credit_score - ?) WHERE id = ?");
        st.bindI64(static_cast<int64_t>(score)).bindI64(userId);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("扣减信用分失败：") + e.what());
    }
}

Result<std::vector<model::ViolationRecord>> SettlementRepo::listViolations(DbConn& db,
                                                                          int64_t userId,
                                                                          int limit) {
    if (userId <= 0) {
        return Result<std::vector<model::ViolationRecord>>::make(
            std::vector<model::ViolationRecord>{});
    }
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    try {
        Stmt st(db, std::string(kViolationSelect) +
                        "WHERE user_id = ? ORDER BY id DESC LIMIT ?");
        st.bindI64(userId).bindI64(static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<model::ViolationRecord>>::fail(rs.code, rs.msg);
        }
        std::vector<model::ViolationRecord> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(model::ViolationRecord::fromRow(row));
        }
        return Result<std::vector<model::ViolationRecord>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<model::ViolationRecord>>::fail(
            kDbError, std::string("查询违约记录失败：") + e.what());
    }
}

} // namespace rf::repo
