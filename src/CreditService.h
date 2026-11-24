// =============================================================================
// src/CreditService.h  —  信用分与黑名单状态机（ARCH-P3 §6 / D-P3-7 / D-P3-9）
//
// 唯一入口是 applyDelta()：分数变动 + 状态迁移 + 明细落库，一步到位。
// 迟滞区（D-P3-7）：
//   · score <  enterScore(60) → 进黑名单   ← **严格小于**
//   · score >= exitScore(80)  → 解除黑名单
//   · [enter, exit)           → 保持原状态（防临界反复横跳）
//   · DISABLED 是人工态，状态机永不改动它
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"
#include "CreditRepo.h"
#include "Config.h"

namespace rf {

/** 一次 applyDelta 的结果 */
struct CreditDeltaResult {
    int64_t     userId      = 0;
    int         scoreBefore = 0;
    int         scoreAfter  = 0;
    std::string statusBefore, statusAfter;
    bool        statusChanged = false;
    bool        applied       = true;   ///< false = sourceRef 已存在（幂等跳过）
    int64_t     logId         = 0;
};

/** 信誉概览（/credit/me 等返回；与 model::UserCreditView 同一实现，避免两套口径） */
using CreditAccount = model::UserCreditView;

class CreditService {
public:
    CreditService(DbPool& pool, repo::CreditRepo& repo, const CreditConfig& cfg);

    /**
     * 应用一次信用变动（**唯一入口**）。
     * 事务内顺序（D-P3-9：绝不写成一条 UPDATE + CASE）：
     *   ① 读 user FOR UPDATE 拿 before（同时行锁，天然串行化同一用户的并发变动）
     *   ② C++ 算 after 与新状态（clamp 到 0..100）
     *   ③ 插 credit_log —— 幂等闸门：sourceRef 冲突 → applied=false，**不改分数**
     *   ④ 写两列（值都来自 C++）
     *   ⑤ 若状态变化，再补一条 change=0 的日志（否则"没扣分怎么进黑名单了"无法解释）
     *
     * @param sourceRef 幂等键，见 D-P3-8（`RES:{id}:{TYPE}:{seq}` / `ADMIN:{uid}:{epoch}` …）
     * @param ruleCode  触发规则（非规则触发传空）
     * @return applied=false 表示该 sourceRef 已应用过（调用方静默跳过）
     */
    Result<CreditDeltaResult> applyDelta(DbConn& conn, int64_t userId,
                                         std::optional<int64_t> reservationId, int delta,
                                         const std::string& reasonType,
                                         const std::string& sourceRef,
                                         const std::string& ruleCode,
                                         std::optional<int64_t> operatorId,
                                         const std::string& remark);

    /** 当前分 / 状态 / 阈值 / 距下一档 */
    Result<CreditAccount> account(DbConn& conn, int64_t userId);

    /** 明细分页（reasonType 可空） */
    Result<repo::CreditPage> listLogs(DbConn& conn, int64_t userId, int page, int size,
                                      const std::string& reasonType);

    /**
     * 定时兜底纠偏（§6.2）：把「分与状态不一致」的账号拉到正确状态，
     * 写 credit_log(reason=AUTO_REPAIR)，返回纠正数。
     */
    Result<int> repairStatus(DbConn& conn);

    /** 纯函数：按迟滞区算新状态（T06 的边界单测直接调它，不需要 DB） */
    std::string nextStatus(const std::string& current, int score) const;

    const CreditConfig& config() const { return cfg_; }

private:
    /** 状态变化的补充日志（change=0） */
    VoidResult logStatusChange(DbConn& conn, int64_t userId, const model::CreditLog& main,
                               const std::string& reasonType);

    DbPool&           pool_;
    repo::CreditRepo& repo_;
    CreditConfig      cfg_;
};

} // namespace rf
