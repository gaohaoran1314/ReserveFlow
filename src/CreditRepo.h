// =============================================================================
// src/CreditRepo.h  —  credit_log + user.credit_score 的读写（无状态）
// 对应 ARCH-P3 §2.1 #6 / §6 / D-P3-8 / D-P3-9
//
// 本层刻意**不做**「一条 UPDATE 同时改分数和状态」——
// D-P3-9 的实测陷阱（MySQL SET 从左到右求值，CASE 读到已更新的分数，delta 被应用两次）
// 要求把「读旧值 → 算新值 → 写两列」拆成两个方法，由 CreditService 在事务内串起来。
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"

namespace rf::repo {

/** 明细分页（与 ReservationPage 同构；total 由同条件 COUNT(*) 单独取） */
struct CreditPage {
    std::vector<model::CreditLog> list;
    int64_t                       total = 0;
    int                           page  = 1;
    int                           size  = 10;
};

class CreditRepo {
public:
    CreditRepo()  = default;
    ~CreditRepo() = default;
    CreditRepo(const CreditRepo&)            = delete;
    CreditRepo& operator=(const CreditRepo&) = delete;

    /**
     * 读账号当前分与状态（`SELECT ... FOR UPDATE`，调用方**必须已在事务内**）。
     * @return first=credit_score, second=status
     */
    Result<std::pair<int, std::string>> lockUser(DbConn& db, int64_t userId);

    /** 写两列（值由 C++ 算好，SQL 不做任何算术 —— D-P3-9） */
    VoidResult updateScoreAndStatus(DbConn& db, int64_t userId, int score,
                                    const std::string& status);

    /**
     * 插明细（幂等闸门）。
     * source_ref 冲突（uk_credit_source 1062）→ 返回 kSlotUnavailable 作为「已应用」信号。
     */
    Result<int64_t> insertLog(DbConn& db, const model::CreditLog& log);

    Result<std::vector<model::CreditLog>> listLogs(DbConn& db, int64_t userId, int page, int size,
                                                   const std::string& reasonType);
    Result<int64_t>                       countLogs(DbConn& db, int64_t userId,
                                                     const std::string& reasonType);

    /**
     * 定时纠偏用：分与状态不一致的账号。
     * @return 每项 {userId, {score, status}}
     */
    Result<std::vector<std::pair<int64_t, std::pair<int, std::string>>>> findInconsistent(
        DbConn& db, int enterScore, int exitScore, int limit);
};

} // namespace rf::repo
