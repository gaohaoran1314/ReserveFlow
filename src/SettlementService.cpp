// =============================================================================
// src/SettlementService.cpp  —  押金 / 违约金规则实现
// 对应 ARCH-P2 §3.4 / §6 / D-P2-6 / D-P2-11 / D-P2-12
//
// 三条最容易写错的规则，都集中在这里：
//   1) 零押金 → 一条流水都不写（不是写 0 元流水）：会议室遍地都是，写 0 会污染 I3 不变量；
//   2) 扣减必须三重截断：min(冻结剩余, 期望额, damageDeductMax)，且期望额 <= 0 时按比例算；
//   3) 扣分幂等靠 uk_violation_dedup（1062→3005），**不是**靠应用层 flag。
// =============================================================================
#include "SettlementService.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "Base.h"

namespace rf {

namespace {

/** 金额保留两位小数（DECIMAL(10,2) 语义）；避免 199.99999 这类浮点噪声入库 */
double round2(double v) {
    return static_cast<double>(static_cast<int64_t>(v * 100.0 + (v >= 0 ? 0.5 : -0.5))) / 100.0;
}

/** 金额比较容差（DECIMAL(10,2) 最小单位的一半） */
constexpr double kMoneyEps = 0.005;

/** 流水号：DP{reservationId}{suffix} */
std::string txnNo(int64_t reservationId, const char* suffix) {
    return "DP" + std::to_string(reservationId) + suffix;
}

}  // namespace

// ============================================================================
// DepositAccount
// ============================================================================
std::string DepositAccount::state() const {
    if (frozen <= kMoneyEps) {
        return "NONE";                     // 零押金资源，从无流水
    }
    const double settled = deducted + refunded;
    if (settled <= kMoneyEps) {
        return "FROZEN";                   // 已冻结、未结算
    }
    if (settled + kMoneyEps >= frozen) {
        return "SETTLED";                  // 已结清
    }
    return "PARTIAL";                      // 部分扣减，尚有余额
}

nlohmann::json DepositAccount::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["frozen"]    = round2(frozen);
    j["deducted"]  = round2(deducted);
    j["refunded"]  = round2(refunded);
    j["remaining"] = round2(remaining());
    j["state"]     = state();
    return j;
}

// ============================================================================
// SettlementService
// ============================================================================
SettlementService::SettlementService(DbPool& pool, repo::SettlementRepo& srepo,
                                     CreditService& credit, const DepositConfig& cfg)
    : pool_(pool), srepo_(srepo), credit_(credit), cfg_(cfg) {}

VoidResult SettlementService::freeze(DbConn& conn, int64_t reservationId, int64_t userId,
                                     double amount) {
    if (reservationId <= 0 || userId <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：冻结押金的单据/用户非法");
    }
    const double amt = round2(amount);
    if (amt <= 0.0) {
        // D-P2-12：零押金资源一条流水都不写（连 0 元 FREEZE 也不写）
        return VoidResult::make(Empty{});
    }

    model::DepositRecord d;
    d.reservationId = reservationId;
    d.userId        = userId;
    d.amount        = amt;
    d.recordType    = model::toStr(model::DepositRecordType::FREEZE);
    d.status        = "SUCCESS";
    d.transactionNo = txnNo(reservationId, "FREEZE");
    d.remark        = "下单冻结押金";
    return srepo_.insertDeposit(conn, d);
}

Result<DepositAccount> SettlementService::account(DbConn& conn, int64_t reservationId) {
    DepositAccount acc;
    if (reservationId <= 0) {
        return Result<DepositAccount>::make(acc);
    }
    const auto f = srepo_.sumDepositByType(
        conn, reservationId, model::toStr(model::DepositRecordType::FREEZE));
    if (!f.ok()) {
        return Result<DepositAccount>::fail(f.code, f.msg);
    }
    const auto d = srepo_.sumDepositByType(
        conn, reservationId, model::toStr(model::DepositRecordType::DEDUCT));
    if (!d.ok()) {
        return Result<DepositAccount>::fail(d.code, d.msg);
    }
    const auto r = srepo_.sumDepositByType(
        conn, reservationId, model::toStr(model::DepositRecordType::REFUND));
    if (!r.ok()) {
        return Result<DepositAccount>::fail(r.code, r.msg);
    }
    acc.frozen   = f.data;
    acc.deducted = d.data;
    acc.refunded = r.data;
    return Result<DepositAccount>::make(acc);
}

VoidResult SettlementService::refundRemaining(DbConn& conn, int64_t reservationId, int64_t userId,
                                              const std::string& remark) {
    if (reservationId <= 0 || userId <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：退还押金的单据/用户非法");
    }
    auto acc = account(conn, reservationId);
    if (!acc.ok()) {
        return VoidResult::fail(acc.code, acc.msg);
    }
    if (acc.data.frozen <= kMoneyEps) {
        // 非法路径「未 FREEZE 就 REFUND」：无流水可记，直接成功（§6.2）
        LOG_WARN("单据 {} 无冻结押金，跳过退还（零押金资源属正常情况）", reservationId);
        return VoidResult::make(Empty{});
    }
    const double remain = round2(acc.data.remaining());
    if (remain <= kMoneyEps) {
        return VoidResult::make(Empty{});   // 已结清，无需再退
    }

    model::DepositRecord d;
    d.reservationId = reservationId;
    d.userId        = userId;
    d.amount        = remain;
    d.recordType    = model::toStr(model::DepositRecordType::REFUND);
    d.status        = "SUCCESS";
    d.transactionNo = txnNo(reservationId, "REFUND");
    d.remark        = remark.empty() ? std::string("押金退还") : remark;
    return srepo_.insertDeposit(conn, d);
}

VoidResult SettlementService::deductForDamage(DbConn& conn, int64_t reservationId, int64_t userId,
                                              double requestedAmount, const std::string& remark,
                                              double* outDeducted,
                                              const std::optional<double>& ratioOverride,
                                              const std::optional<double>& maxOverride) {
    if (outDeducted != nullptr) {
        *outDeducted = 0.0;
    }
    if (reservationId <= 0 || userId <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：扣减押金的单据/用户非法");
    }
    if (requestedAmount < 0.0) {
        return VoidResult::fail(kDepositAmountInvalid, "押金金额不合法：扣减金额不能为负数");
    }

    auto acc = account(conn, reservationId);
    if (!acc.ok()) {
        return VoidResult::fail(acc.code, acc.msg);
    }
    if (acc.data.frozen <= kMoneyEps) {
        LOG_WARN("单据 {} 无冻结押金，跳过损坏扣减（零押金资源）", reservationId);
        return VoidResult::make(Empty{});
    }

    const double remain = round2(acc.data.remaining());
    if (remain <= kMoneyEps) {
        return VoidResult::make(Empty{});   // 已经全部扣完/退完
    }

    // 比例与上限：P3 起可由规则（DEPOSIT_DAMAGE_RATIO）覆盖；未提供时用 config（P2 行为）
    const double ratio = ratioOverride.value_or(cfg_.damageDeductRatio);
    const double maxAmt = maxOverride.value_or(cfg_.damageDeductMax);

    // 期望扣减额：显式金额优先，否则按比例
    double want = (requestedAmount > 0.0) ? requestedAmount : acc.data.frozen * ratio;
    // 上限约束（<= 0 表示不封顶）
    if (maxAmt > 0.0) {
        want = std::min(want, maxAmt);
    }
    // 三重截断的最后一步：不能超过「还没结算的余额」
    double actual = round2(std::min(remain, want));
    if (actual <= kMoneyEps) {
        return VoidResult::make(Empty{});   // 比例为 0 或额度已用尽
    }
    if (actual > remain + kMoneyEps) {
        actual = remain;
    }

    model::DepositRecord d;
    d.reservationId = reservationId;
    d.userId        = userId;
    d.amount        = actual;
    d.recordType    = model::toStr(model::DepositRecordType::DEDUCT);
    d.status        = "SUCCESS";
    d.transactionNo = txnNo(reservationId, "DEDUCT1");   // P2 每单只结案一次，seq 恒 1
    d.remark        = remark.empty() ? std::string("损坏扣减") : remark;

    auto rr = srepo_.insertDeposit(conn, d);
    if (!rr.ok()) {
        return rr;
    }
    if (outDeducted != nullptr) {
        *outDeducted = actual;
    }
    return VoidResult::make(Empty{});
}

Result<std::vector<model::DepositRecord>> SettlementService::listDeposits(DbConn& conn,
                                                                         int64_t reservationId) {
    return srepo_.listDeposits(conn, reservationId);
}

VoidResult SettlementService::penalize(DbConn& conn, int64_t reservationId, int64_t userId,
                                       const std::string& violationType, int deductSeq, int score,
                                       int64_t operatorId, const std::string& remark,
                                       const std::string& ruleCode) {
    if (score <= 0) {
        return VoidResult::fail(kDepositAmountInvalid, "押金金额不合法：扣分分值必须为正数");
    }
    if (violationType.empty() || userId <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：违约类型 / 用户非法");
    }

    model::ViolationRecord v;
    v.userId        = userId;
    v.reservationId = reservationId;
    v.violationType = violationType;
    v.deductScore   = score;
    v.deductSeq     = deductSeq;
    v.operatorId    = operatorId;
    v.remark        = remark;

    auto ins = srepo_.insertViolation(conn, v);
    if (!ins.ok()) {
        // 3005 = 本周期已扣过（唯一索引命中），静默跳过；其它错误照常上报
        return VoidResult::fail(ins.code, ins.msg);
    }
    // 违约记录写成功之后才扣信用分（幂等顺序不可颠倒）。
    // P3 起扣分走 CreditService::applyDelta：它负责 clamp、迟滞区状态迁移、写 credit_log；
    // source_ref 与 uk_violation_dedup 是同一组 (单, 类型, seq)，两层幂等天然对齐。
    const std::string sourceRef = "RES:" + std::to_string(reservationId) + ":" + violationType +
                                  ":" + std::to_string(deductSeq);
    auto credit = credit_.applyDelta(conn, userId,
                                     reservationId > 0 ? std::optional<int64_t>(reservationId)
                                                       : std::nullopt,
                                     -score, violationType, sourceRef, ruleCode,
                                     operatorId > 0 ? std::optional<int64_t>(operatorId)
                                                    : std::nullopt,
                                     remark);
    if (!credit.ok()) {
        return VoidResult::fail(credit.code, credit.msg);
    }
    LOG_INFO("违约扣分：单={} 用户={} 类型={} seq={} 扣{}分（{} → {}，{}）", reservationId, userId,
             violationType, deductSeq, score, credit.data.scoreBefore, credit.data.scoreAfter,
             credit.data.applied ? "已应用" : "幂等跳过");
    return VoidResult::make(Empty{});
}

} // namespace rf
