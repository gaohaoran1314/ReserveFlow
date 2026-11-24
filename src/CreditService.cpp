// =============================================================================
// src/CreditService.cpp  —  信用分状态机实现（ARCH-P3 §6）
//
// ⚠️ 本文件最关键的一行不是算术，而是**不用 SQL 算术**：
//    分数与新状态都在 C++ 算好，UPDATE 一次写两列（D-P3-9）。
//    若写成 `SET credit_score = ..., status = CASE ... credit_score-20 ... END`，
//    MySQL 会读到已更新的分数，delta 被应用两次（实测 80-20 得到 60/BLACKLIST）。
// =============================================================================
#include "CreditService.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <utility>

#include "Base.h"

namespace rf {

CreditService::CreditService(DbPool& pool, repo::CreditRepo& repo, const CreditConfig& cfg)
    : pool_(pool), repo_(repo), cfg_(cfg) {}

std::string CreditService::nextStatus(const std::string& current, int score) const {
    // DISABLED（及其它非 NORMAL/BLACKLIST 的人工态）不归状态机管
    if (current != "NORMAL" && current != "BLACKLIST") {
        return current;
    }
    if (current == "NORMAL" && score < cfg_.blacklistEnterScore) {
        return "BLACKLIST";   // **严格小于**：60 分仍 NORMAL（D-P3-7）
    }
    if (current == "BLACKLIST" && score >= cfg_.blacklistExitScore) {
        return "NORMAL";      // 迟滞区 [60,80) 内不解除
    }
    return current;
}

Result<CreditDeltaResult> CreditService::applyDelta(DbConn& conn, int64_t userId,
                                                    std::optional<int64_t> reservationId,
                                                    int delta, const std::string& reasonType,
                                                    const std::string& sourceRef,
                                                    const std::string& ruleCode,
                                                    std::optional<int64_t> operatorId,
                                                    const std::string& remark) {
    CreditDeltaResult out;
    out.userId = userId;

    if (userId <= 0) {
        return Result<CreditDeltaResult>::fail(kInvalidParam, "参数校验失败：用户 id 非法");
    }
    if (sourceRef.empty()) {
        return Result<CreditDeltaResult>::fail(kInvalidParam, "参数校验失败：sourceRef 不能为空");
    }
    if (reasonType.empty()) {
        return Result<CreditDeltaResult>::fail(kInvalidParam, "参数校验失败：reasonType 不能为空");
    }

    // ---- ① 读旧值（行锁；同一用户的并发变动在这里串行化）----
    auto locked = repo_.lockUser(conn, userId);
    if (!locked.ok()) {
        return Result<CreditDeltaResult>::fail(locked.code, locked.msg);
    }
    const int         before       = locked.data.first;
    const std::string statusBefore = locked.data.second;

    // ---- ② C++ 算新值（SQL 不参与算术）----
    const int after       = std::clamp(before + delta, 0, 100);
    const std::string statusAfter = nextStatus(statusBefore, after);

    out.scoreBefore   = before;
    out.scoreAfter    = after;
    out.statusBefore  = statusBefore;
    out.statusAfter   = statusAfter;
    out.statusChanged = (statusBefore != statusAfter);

    // ---- ③ 插明细：幂等闸门 ----
    model::CreditLog log;
    log.userId        = userId;
    log.reservationId = reservationId.value_or(0);
    log.changeScore   = after - before;   // 用实际变化量（clamp 后可能与 delta 不同）
    log.scoreBefore   = before;
    log.scoreAfter    = after;
    log.statusBefore  = statusBefore;
    log.statusAfter   = statusAfter;
    log.reasonType    = reasonType;
    log.ruleCode      = ruleCode;
    log.sourceRef     = sourceRef;
    log.operatorId    = operatorId.value_or(0);
    log.remark        = remark;

    auto ins = repo_.insertLog(conn, log);
    if (!ins.ok()) {
        if (ins.code == kSlotUnavailable) {
            // sourceRef 已存在 → 这次变动早应用过，静默跳过（applied=false）
            out.applied = false;
            return Result<CreditDeltaResult>::make(out);
        }
        // §10.2 取舍：明细写不进去不应阻塞惩罚（漏账可事后补算），记 ERROR 后继续扣分
        LOG_ERROR("写信用明细失败（继续执行扣分，账目可能缺失）：user={} source={} err={}", userId,
                  sourceRef, ins.msg);
    } else {
        out.logId = ins.data;
    }

    // ---- ④ 写两列 ----
    auto upd = repo_.updateScoreAndStatus(conn, userId, after, statusAfter);
    if (!upd.ok()) {
        return Result<CreditDeltaResult>::fail(upd.code, upd.msg);
    }

    // ---- ⑤ 状态变化补一条 change=0 的日志 ----
    if (out.statusChanged) {
        const std::string reason =
            (statusAfter == "BLACKLIST") ? model::toStr(model::CreditReasonType::AUTO_BLACKLIST)
                                         : model::toStr(model::CreditReasonType::AUTO_RECOVER);
        (void)logStatusChange(conn, userId, log, reason);
        LOG_INFO("信用状态变化：user={} {} → {}（score={}）", userId, statusBefore, statusAfter,
                 after);
    }

    LOG_DEBUG("信用变动：user={} {}→{}（{}）source={} rule={}", userId, before, after,
              out.statusChanged ? statusBefore + "→" + statusAfter : std::string("状态不变"),
              sourceRef, ruleCode);
    return Result<CreditDeltaResult>::make(out);
}

VoidResult CreditService::logStatusChange(DbConn& conn, int64_t userId,
                                          const model::CreditLog& main,
                                          const std::string& reasonType) {
    model::CreditLog extra;
    extra.userId        = userId;
    extra.reservationId = main.reservationId;
    extra.changeScore   = 0;   // 纯状态迁移，分数不变（§6.3 最后一段）
    extra.scoreBefore   = main.scoreAfter;
    extra.scoreAfter    = main.scoreAfter;
    extra.statusBefore  = main.statusBefore;
    extra.statusAfter   = main.statusAfter;
    extra.reasonType    = reasonType;
    extra.ruleCode      = main.ruleCode;
    // sourceRef 带 epoch 后缀：同一用户同一秒内只可能发生一次状态跃迁，
    // 万一撞上（1062）也只是少一条解释性日志，不影响分数正确性
    const std::string tag = (reasonType == "AUTO_BLACKLIST") ? "BL" : "RC";
    extra.sourceRef = "AUTO:" + tag + ":" + std::to_string(userId) + ":" +
                      std::to_string(static_cast<long long>(TimeUtil::nowEpoch()));
    extra.operatorId = 0;
    extra.remark     = (reasonType == "AUTO_BLACKLIST") ? std::string("信用分跌破阈值，自动进入黑名单")
                                                        : std::string("信用分达到恢复阈值，自动解除黑名单");

    auto r = repo_.insertLog(conn, extra);
    if (!r.ok() && r.code != kSlotUnavailable) {
        LOG_WARN("写状态变化明细失败（不影响分数正确性）：user={} err={}", userId, r.msg);
    }
    return VoidResult::make(Empty{});
}

Result<CreditAccount> CreditService::account(DbConn& conn, int64_t userId) {
    if (userId <= 0) {
        return Result<CreditAccount>::fail(kInvalidParam, "参数校验失败：用户 id 非法");
    }
    try {
        Stmt st(conn, "SELECT id, username, credit_score, status FROM `user` WHERE id = ?");
        st.bindI64(userId);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<CreditAccount>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<CreditAccount>::fail(kInvalidCredential, "账号不存在");
        }
        CreditAccount acc;
        acc.userId    = static_cast<int64_t>(std::strtoll(
            rs.data.front()[0].has_value() ? rs.data.front()[0]->c_str() : "0", nullptr, 10));
        acc.username  = rs.data.front()[1].has_value() ? *rs.data.front()[1] : std::string();
        acc.score     = static_cast<int>(std::strtoll(
            rs.data.front()[2].has_value() ? rs.data.front()[2]->c_str() : "0", nullptr, 10));
        acc.status    = rs.data.front()[3].has_value() ? *rs.data.front()[3] : std::string();
        acc.enterScore = cfg_.blacklistEnterScore;
        acc.exitScore  = cfg_.blacklistExitScore;
        return Result<CreditAccount>::make(std::move(acc));
    } catch (const std::exception& e) {
        return Result<CreditAccount>::fail(kDbError, std::string("读取信誉概览失败：") + e.what());
    }
}

Result<repo::CreditPage> CreditService::listLogs(DbConn& conn, int64_t userId, int page, int size,
                                                 const std::string& reasonType) {
    repo::CreditPage out;
    out.page = (page < 1) ? 1 : page;
    out.size = (size < 1) ? 10 : (size > 100 ? 100 : size);
    if (!reasonType.empty() && !model::creditReasonTypeFrom(reasonType).has_value()) {
        return Result<repo::CreditPage>::fail(
            kInvalidParam, "参数校验失败：reasonType 取值非法（" + reasonType + "）");
    }

    auto total = repo_.countLogs(conn, userId, reasonType);
    if (!total.ok()) {
        return Result<repo::CreditPage>::fail(total.code, total.msg);
    }
    out.total = total.data;
    if (out.total == 0) {
        return Result<repo::CreditPage>::make(std::move(out));
    }
    auto list = repo_.listLogs(conn, userId, out.page, out.size, reasonType);
    if (!list.ok()) {
        return Result<repo::CreditPage>::fail(list.code, list.msg);
    }
    out.list = std::move(list.data);
    return Result<repo::CreditPage>::make(std::move(out));
}

Result<int> CreditService::repairStatus(DbConn& conn) {
    auto rows = repo_.findInconsistent(conn, cfg_.blacklistEnterScore, cfg_.blacklistExitScore, 100);
    if (!rows.ok()) {
        return Result<int>::fail(rows.code, rows.msg);
    }
    int fixed = 0;
    for (const auto& item : rows.data) {
        const int64_t     userId = item.first;
        const int         score  = item.second.first;
        const std::string status = item.second.second;

        // 复算一次：以「分数」为唯一真相，把状态拉正
        const std::string target = (score < cfg_.blacklistEnterScore)   ? "BLACKLIST"
                                   : (score >= cfg_.blacklistExitScore) ? "NORMAL"
                                                                        : status;
        if (target == status) {
            continue;
        }
        auto upd = repo_.updateScoreAndStatus(conn, userId, score, target);
        if (!upd.ok()) {
            LOG_WARN("纠正信用状态失败：user={} err={}", userId, upd.msg);
            continue;
        }
        model::CreditLog log;
        log.userId       = userId;
        log.changeScore  = 0;
        log.scoreBefore  = score;
        log.scoreAfter   = score;
        log.statusBefore = status;
        log.statusAfter  = target;
        log.reasonType   = model::toStr(model::CreditReasonType::AUTO_REPAIR);
        log.sourceRef    = "AUTO:RP:" + std::to_string(userId) + ":" +
                        std::to_string(static_cast<long long>(TimeUtil::nowEpoch()));
        log.operatorId = 0;
        log.remark     = "定时纠偏：信用状态与分数不一致，已按分数修正";
        (void)repo_.insertLog(conn, log);
        LOG_INFO("定时纠偏：user={} 状态 {} → {}（score={}）", userId, status, target, score);
        ++fixed;
    }
    return Result<int>::make(fixed);
}

} // namespace rf
