// =============================================================================
// src/SettlementRepo.h  —  押金流水 / 违约记录 / 信用分 的 SQL（无状态）
// 对应 ARCH-P2 §3.3 / §6 / D-P2-6 / D-P2-11
//
// 两条幂等铁律落在本层：
//   1) uk_deposit_txn 冲突（1062）→ **视为已记账**，返回 kOk（不是错误）；
//   2) uk_violation_dedup 冲突（1062）→ 返回 kSlotUnavailable(3005) 作为「本周期已扣过」
//      的内部信号，调用方据此跳过扣分（这是 P2 扣分幂等的最终防线，第 2 层）。
// 本层不做业务判断、不开事务（沿用 P0/P1 约定）。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"

namespace rf::repo {

/** deposit_record + violation_record + user.credit_score 的读写 */
class SettlementRepo {
public:
    SettlementRepo()  = default;
    ~SettlementRepo() = default;
    SettlementRepo(const SettlementRepo&)            = delete;
    SettlementRepo& operator=(const SettlementRepo&) = delete;

    /**
     * 插入押金流水。
     * transaction_no 冲突（uk_deposit_txn 1062）→ **视为已记账，返回 kOk**（幂等，D-P2-6）。
     * 其它 SQL 错 → kDbError(1009)。
     */
    VoidResult insertDeposit(DbConn& db, const model::DepositRecord& d);

    /** 按单查流水（升序，给 /reservations/{id}/deposits） */
    Result<std::vector<model::DepositRecord>> listDeposits(DbConn& db, int64_t reservationId);

    /** 按单 + 类型汇总金额（用于算「已冻结 / 已扣减 / 已退还」） */
    Result<double> sumDepositByType(DbConn& db, int64_t reservationId,
                                    const std::string& recordType);

    /**
     * 插入违约记录。
     * (reservation_id, violation_type, deduct_seq) 冲突 1062 → 返回 **kSlotUnavailable(3005)**
     * 作为「本周期已扣过」的内部信号；调用方据此跳过扣分（D-P2-6 第 2 层）。
     */
    Result<int64_t> insertViolation(DbConn& db, const model::ViolationRecord& v);

    /** 原子扣信用分（下限 0）：UPDATE user SET credit_score = GREATEST(0, credit_score - ?) */
    VoidResult deductCredit(DbConn& db, int64_t userId, int score);

    /** 按用户查违约记录（P3 信用看板预留，P2 仅调试用） */
    Result<std::vector<model::ViolationRecord>> listViolations(DbConn& db, int64_t userId,
                                                               int limit = 50);
};

} // namespace rf::repo
