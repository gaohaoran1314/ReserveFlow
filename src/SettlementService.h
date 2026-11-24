// =============================================================================
// src/SettlementService.h  —  押金账目规则 + 违约金扣分规则
// 对应 ARCH-P2 §3.4 / §6 / D-P2-6 / D-P2-11 / D-P2-12
//
// 口径（主理人已拍板）：
//   · 只记「账目意图」流水（FREEZE / DEDUCT / REFUND），status 恒 SUCCESS；
//   · 不建钱包、不校验余额、不做真实支付（P4 与支付平台一起做）；
//   · deposit_amount = 0 的资源**一条流水都不写**（连 FREEZE 都不写，D-P2-12）；
//   · 损坏扣减 = min(剩余, 请求额 or frozen*ratio, damageDeductMax)，不做分级。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"
#include "SettlementRepo.h"
#include "CreditService.h"
#include "Config.h"

namespace rf {

/** 押金账目聚合视图（由流水推导，不存状态列 —— D-P2-11） */
struct DepositAccount {
    double frozen   = 0.0;   ///< SUM(FREEZE)
    double deducted = 0.0;   ///< SUM(DEDUCT)
    double refunded = 0.0;   ///< SUM(REFUND)

    bool hasFreeze() const { return frozen > 0.0; }
    /** NONE / FROZEN / PARTIAL / SETTLED（纯推导，见 §6.1） */
    std::string state() const;
    double remaining() const { return frozen - deducted - refunded; }

    nlohmann::json toJson() const;
};

/** 押金结算 + 违约金扣分 */
class SettlementService {
public:
    SettlementService(DbPool& pool, repo::SettlementRepo& srepo, CreditService& credit,
                      const DepositConfig& cfg);

    /**
     * 下单冻结。
     * amount <= 0（会议室）→ **不写任何流水**（D-P2-12），直接成功。
     * transaction_no = "DP{reservationId}FREEZE"，重复调用幂等。
     */
    VoidResult freeze(DbConn& conn, int64_t reservationId, int64_t userId, double amount);

    /**
     * 验收通过退还剩余。
     * 退还额 = frozen - deducted - refunded；<= 0 则不写流水。
     * transaction_no = "DP{reservationId}REFUND"。
     */
    VoidResult refundRemaining(DbConn& conn, int64_t reservationId, int64_t userId,
                               const std::string& remark);

    /**
     * 损坏扣减。
     * 实际扣减 = min(冻结剩余, 请求金额 or frozen * damageDeductRatio)，并受 damageDeductMax 上限约束。
     * transaction_no = "DP{reservationId}DEDUCT{seq}"（seq 从 1 起，P2 单次结案故恒为 1）。
     * @param requestedAmount <=0 表示「按比例自动计算」
     * @param outDeducted     实际扣减额（供响应展示与写 violation）
     */
    VoidResult deductForDamage(DbConn& conn, int64_t reservationId, int64_t userId,
                              double requestedAmount, const std::string& remark, double* outDeducted,
                              const std::optional<double>& ratioOverride = std::nullopt,
                              const std::optional<double>& maxOverride = std::nullopt);

    /** 账目聚合（三个 SELECT SUM） */
    Result<DepositAccount> account(DbConn& conn, int64_t reservationId);

    /** 流水列表 */
    Result<std::vector<model::DepositRecord>> listDeposits(DbConn& conn, int64_t reservationId);

    /**
     * 违约扣分（唯一索引幂等的落地点，D-P2-6）。
     * @param violationType  OVERDUE / NO_SHOW / DAMAGE
     * @param deductSeq      周期序号；一次性违约传 0
     * @param score          本次扣分（>0 才写记录）
     * 实现（P3 起）：violation_record 照旧写（验收断言锁死它的字段），
     * 扣分改由 CreditService::applyDelta 完成（多写一条 credit_log + 按迟滞区改状态）。
     * 幂等由两层保证：uk_violation_dedup（本表）+ uk_credit_source（credit_log）。
     *
     * @return kOk           表示本次真的扣了；
     *         3005          表示该 (单,类型,序号) 已扣过（静默跳过）；
     *         3017          表示 score 非法
     */
    VoidResult penalize(DbConn& conn, int64_t reservationId, int64_t userId,
                        const std::string& violationType, int deductSeq, int score,
                        int64_t operatorId, const std::string& remark,
                        const std::string& ruleCode = std::string());

    const DepositConfig& config() const { return cfg_; }

private:
    DbPool&               pool_;
    repo::SettlementRepo& srepo_;
    CreditService&        credit_;
    DepositConfig         cfg_;
};

} // namespace rf
