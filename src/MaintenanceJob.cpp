// =============================================================================
// src/MaintenanceJob.cpp  —  维护任务实现（ARCH-P2 §3.2 / §5）
//
// 四类扫描 + 一类告警：
//   ① 待审核超时  PENDING_REVIEW → REJECTED（释放槽 + 派候补）
//   ② 爽约        LOCKED → CANCELLED（扣分 + 释放槽 + 派候补）
//   ③ 逾期        IN_USE → DUE_RETURN（扣分；**不释放槽**，时段仍归该单）
//   ④ 逾期待续    DUE_RETURN 不迁移，按 deduct_seq 周期扣分（唯一索引幂等）
//   ⑤ RETURN_CHECK 长期未验收：**只 WARN + 计数，不改状态**（主理人决策 2）
//
// 每条都遵守：CAS 成功（affectedRows==1）之后才做副作用（D-P2-5）。
// =============================================================================
#include "MaintenanceJob.h"

#include <algorithm>
#include <ctime>
#include <exception>
#include <string>
#include <utility>

#include <chrono>

#include "Base.h"
#include "SlotUtil.h"
#include "Controllers.h"

namespace rf {

namespace {

/** 定时任务统一使用的锁 TTL（与 config.lock.ttlMs 默认值一致） */
constexpr int kMaintLockTtlMs = 30000;

/** 规则码（与 rule_config 的三条 P2 种子对齐） */
constexpr const char* kRuleReviewTimeout = "RESERVE_REVIEW_TIMEOUT";
constexpr const char* kRuleNoShow        = "RESERVE_NO_SHOW";
constexpr const char* kRuleOverdue       = "RESERVE_OVERDUE";

/** now + N 分钟的 ISO 字符串（N 可为负） */
std::string isoAfterMin(int minutes) {
    const std::time_t t = TimeUtil::nowEpoch() + static_cast<std::time_t>(minutes) * 60;
    return TimeUtil::format(t);
}

/** 从 start/end 重算槽区间（槽可能已被删，不能查表；SlotUtil 是纯函数） */
bool slotRangeOf(const std::string& start, const std::string& end, std::string& date, int& from,
                 int& to) {
    rf::slot::DateTime sd;
    rf::slot::DateTime ed;
    if (!rf::slot::parseIso(start, sd) || !rf::slot::parseIso(end, ed)) {
        return false;
    }
    from = rf::slot::indexOf(sd);
    to   = rf::slot::indexOf(ed);
    if (from < 0 || to <= from) {
        return false;
    }
    char buf[16] = {0};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", sd.year, sd.month, sd.day);
    date = buf;
    return true;
}

}  // namespace

// ============================================================================
// MaintenanceStats
// ============================================================================
void MaintenanceStats::add(const MaintenanceStats& o) {
    pendingRejected   += o.pendingRejected;
    noShowCancelled   += o.noShowCancelled;
    overdueMarked     += o.overdueMarked;
    overdueDeducted   += o.overdueDeducted;
    offersExpired     += o.offersExpired;
    offersDispatched  += o.offersDispatched;
    skippedBusy       += o.skippedBusy;
    errors            += o.errors;
    returnCheckWarned += o.returnCheckWarned;
}

nlohmann::json MaintenanceStats::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["pendingRejected"]   = pendingRejected;
    j["noShowCancelled"]   = noShowCancelled;
    j["overdueMarked"]     = overdueMarked;
    j["overdueDeducted"]   = overdueDeducted;
    j["offersExpired"]     = offersExpired;
    j["offersDispatched"]  = offersDispatched;
    j["skippedBusy"]       = skippedBusy;
    j["errors"]            = errors;
    j["returnCheckWarned"] = returnCheckWarned;
    return j;
}

// ============================================================================
// MaintenanceJob
// ============================================================================
MaintenanceJob::MaintenanceJob(DbPool& pool, DistributedLock& lock, StateMachine& sm,
                               repo::ReservationRepo& rrepo, repo::ResourceRepo& rr,
                               repo::AuditRepo& ar, repo::WaitlistRepo& wrepo,
                               SettlementService& settlement, WaitlistService& waitlist,
                               RuleEngine& engine, const MaintenanceConfig& cfg,
                               const PickupConfig& pickupCfg)
    : pool_(pool),
      lock_(lock),
      sm_(sm),
      rrepo_(rrepo),
      rr_(rr),
      ar_(ar),
      wrepo_(wrepo),
      settlement_(settlement),
      waitlist_(waitlist),
      engine_(engine),
      cfg_(cfg),
      pickupCfg_(pickupCfg) {}

VoidResult MaintenanceJob::writeStatusLog(DbConn& conn, const char* bizType, int64_t businessId,
                                         const std::string& from, const std::string& to,
                                         int64_t operatorId, const std::string& reason,
                                         const std::string& ruleCode) {
    repo::StatusLogEntry e;
    e.businessType = bizType;
    e.businessId   = businessId;
    e.fromStatus   = from;
    e.toStatus     = to;
    e.operatorId   = operatorId;   // 0 → 留 NULL（系统自动流转）
    e.reason       = reason;
    if (ruleCode.empty()) {
        e.ruleCode = std::nullopt;
    } else {
        e.ruleCode = ruleCode;
    }
    auto r = ar_.insertStatusLog(conn, e);
    if (!r.ok()) {
        return VoidResult::fail(r.code, r.msg);
    }
    return VoidResult::make(Empty{});
}

VoidResult MaintenanceJob::convergeResource(DbConn& conn, int64_t resourceId,
                                            int64_t excludeReservationId,
                                            const std::string& why) {
    auto actives = rrepo_.listActiveStatuses(conn, resourceId, excludeReservationId);
    if (!actives.ok()) {
        return VoidResult::fail(actives.code, actives.msg);
    }
    auto cur = rr_.findById(conn, resourceId);
    if (!cur.ok()) {
        return VoidResult::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return VoidResult::make(Empty{});   // 资源已删除：不收敛
    }
    const std::string from = model::toStr(cur.data->status);
    const std::string to   = model::toStr(sm_.convergeResource(from, actives.data));
    if (from == to) {
        return VoidResult::make(Empty{});
    }
    auto upd = rr_.updateStatus(conn, resourceId,
                               model::resourceStateFrom(to).value_or(model::ResourceState::FREE));
    if (!upd.ok()) {
        return VoidResult::fail(upd.code, upd.msg);
    }
    return writeStatusLog(conn, "RESOURCE", resourceId, from, to, 0, why, "");
}

VoidResult MaintenanceJob::releaseAndOffer(DbConn& conn, int64_t reservationId, int64_t resourceId,
                                           const std::string& slotDate, int fromIndex, int toIndex,
                                           bool dispatchOffer) {
    auto del = rrepo_.deleteSlots(conn, reservationId);
    if (!del.ok()) {
        return VoidResult::fail(del.code, del.msg);
    }
    auto conv = convergeResource(conn, resourceId, reservationId, "定时任务：资源状态收敛");
    if (!conv.ok()) {
        return conv;
    }
    if (dispatchOffer && !slotDate.empty() && fromIndex < toIndex) {
        auto disp = waitlist_.dispatchOffers(conn, resourceId, slotDate, fromIndex, toIndex);
        if (!disp.ok()) {
            return VoidResult::fail(disp.code, disp.msg);
        }
    }
    return VoidResult::make(Empty{});
}

bool MaintenanceJob::withLockedReservation(
    int64_t reservationId, MaintenanceStats& st,
    const std::function<VoidResult(DbConn&, const model::Reservation&)>& body) {
    // ---- ① 短读定位资源（读完立刻还连接，等锁期间零连接占用）
    int64_t resourceId = 0;
    {
        DbPool::Guard g = pool_.acquire(3000);
        if (!g) {
            ++st.errors;
            return false;
        }
        auto cur = rrepo_.findById(*g, reservationId);
        if (!cur.ok()) {
            ++st.errors;
            return false;
        }
        if (!cur.data.has_value()) {
            return false;   // 单据已不存在：不算错误
        }
        resourceId = cur.data->resourceId;
    }

    // ---- ② 短锁：抢不到就跳过本轮（cfg_.lockWaitMs=500），绝不阻塞用户
    auto lockRes = lock_.acquireResource(resourceId, cfg_.lockWaitMs, kMaintLockTtlMs);
    if (!lockRes.ok()) {
        ++st.skippedBusy;
        return false;
    }
    LockHandle lock = std::move(lockRes.data);

    // ---- ③ 取连接 + 事务
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        ++st.errors;
        return false;
    }
    DbConn& conn = *guard;

    Transaction tx(conn);
    if (!tx.active()) {
        ++st.errors;
        return false;
    }

    // ---- ④ 锁内重读真相（候选是锁外算的）
    auto cur = rrepo_.findById(conn, reservationId);
    if (!cur.ok()) {
        tx.rollback();
        ++st.errors;
        return false;
    }
    if (!cur.data.has_value()) {
        tx.rollback();
        return false;
    }

    // ---- ⑤ 业务回调（CAS + 副作用）
    auto r = body(conn, *cur.data);
    if (!r.ok()) {
        tx.rollback();
        ++st.errors;
        LOG_WARN("维护任务处理单据 {} 失败（已回滚）：{}", reservationId, r.msg);
        return false;
    }

    // ---- ⑥ 提交前查锁
    if (lock.lost()) {
        tx.rollback();
        ++st.errors;
        LOG_WARN("维护任务提交前发现资源锁已丢失，回滚单据 {}", reservationId);
        return false;
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        ++st.errors;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ 引擎辅助（T07）
/** 距 end_time 的逾期分钟数（仅作展示/日志用的 fact） */
static int overdueMinutesOf(const std::string& endIso, const std::string& nowIso) {
    std::time_t e = 0;
    std::time_t n = 0;
    if (!TimeUtil::parseIso(endIso, e) || !TimeUtil::parseIso(nowIso, n)) {
        return 0;
    }
    if (n <= e) {
        return 0;
    }
    return static_cast<int>((n - e) / 60);
}

FactBag MaintenanceJob::timerFacts(DbConn& /*conn*/, const model::Reservation& cur,
                                   int deductSeq) const {
    FactBag f;
    const std::string nowIso = TimeUtil::nowString();
    f.setTime("now", nowIso);
    f.set("reservation.id", Fact::of(static_cast<double>(cur.id)));
    f.set("reservation.status", Fact::of(model::toStr(cur.status)));
    f.setTime("reservation.startTime", cur.startTime);
    f.setTime("reservation.endTime", cur.endTime);
    f.setTime("reservation.createdAt", cur.createdAt);
    f.set("reservation.deductSeq", Fact::of(static_cast<double>(deductSeq)));
    f.set("reservation.overdueMinutes", Fact::of(static_cast<double>(overdueMinutesOf(cur.endTime, nowIso))));
    return f;
}

MaintenanceJob::TimerVerdict MaintenanceJob::judgeTimer(DbConn& conn,
                                                        const model::Reservation& cur,
                                                        ActionType want, int deductSeq,
                                                        const std::string& expectEvent) {
    TimerVerdict v;
    if (!engine_.enabled()) {
        return v;   // active=false → 调用方走 P2 硬编码逻辑（byte-identical）
    }
    const EvalResult ev = engine_.evaluate(EvalPhase::TIMER, timerFacts(conn, cur, deductSeq));
    if (ev.degraded) {
        LOG_WARN("规则引擎降级，单据 {} 本轮走 P2 硬编码判定", cur.id);
        return v;
    }
    v.active = true;
    if (ev.rejected) {
        // 时间类规则都是 adjust/transition；万一被管理员加了 reject 类规则命中，
        // 语义是「不要迁移这张单」，按不命中处理（fail-safe，不阻断整轮）。
        v.hit = false;
        return v;
    }
    if (const ResolvedAction* a = ev.findAction(ActionType::Transition); a != nullptr) {
        if (want != ActionType::Transition || (!expectEvent.empty() && a->transitEvent != expectEvent)) {
            v.hit = false;
            return v;
        }
        v.hit          = true;
        v.ruleCode     = a->ruleCode;
        v.event        = a->transitEvent;
        v.targetStatus = a->targetStatus;
        v.releaseSlots = a->releaseSlots;
        v.deductCredit = a->deductCredit;
        v.deductScore  = a->deductScore;
        return v;
    }
    if (const ResolvedAction* d = ev.findAction(ActionType::DeductOnly); d != nullptr) {
        if (want != ActionType::DeductOnly) {
            v.hit = false;
            return v;
        }
        v.hit          = true;
        v.ruleCode     = d->ruleCode;
        v.deductCredit = true;
        v.deductScore  = d->deductScore;
        return v;
    }
    v.hit = false;   // SQL 粗筛命中，但引擎认为不该动 → 跳过（下一轮再扫）
    return v;
}

// ------------------------------------------------------------------ ① 待审核超时
MaintenanceStats MaintenanceJob::runPendingReject(const std::string& tickId) {
    MaintenanceStats st;

    // D-P3-11：窗口以规则参数为唯一真相，config 只作兜底默认值
    const int ttlMin = engine_.enabled()
                           ? engine_.paramInt("RESERVE_REVIEW_TIMEOUT", "pendingReviewTtlMin",
                                              cfg_.pendingReviewTtlMin)
                           : cfg_.pendingReviewTtlMin;
    const std::string cutoff = isoAfterMin(-ttlMin);

    std::vector<int64_t> ids;
    {
        DbPool::Guard g = pool_.acquire(3000);
        if (!g) {
            ++st.errors;
            return st;
        }
        auto r = rrepo_.listStalePending(*g, cutoff, cfg_.batchSize);
        if (!r.ok()) {
            ++st.errors;
            return st;
        }
        ids = r.data;
    }
    if (ids.empty()) {
        return st;
    }
    LOG_INFO("[{}] 待审核超时候选 {} 单（TTL={}min, cutoff={}）", tickId, ids.size(), ttlMin, cutoff);

    for (const int64_t id : ids) {
        bool       applied = false;
        const bool ran     = withLockedReservation(
            id, st, [&](DbConn& conn, const model::Reservation& cur) -> VoidResult {
                if (cur.status != model::ReservationStatus::PENDING_REVIEW) {
                    return VoidResult::make(Empty{});
                }
                // ★ 引擎精判（SQL 只是粗筛）：关闭/降级时 verdict.active=false → 走 P2
                const TimerVerdict vd = judgeTimer(conn, cur, ActionType::Transition, 0, "TIMEOUT");
                if (vd.active && !vd.hit) {
                    LOG_DEBUG("[{}] 单据 {} 未被规则命中，本轮不处理", tickId, id);
                    return VoidResult::make(Empty{});
                }
                const std::string target   = vd.active && !vd.targetStatus.empty()
                                                 ? vd.targetStatus
                                                 : std::string("REJECTED");
                const std::string ruleCode = vd.active ? vd.ruleCode : std::string(kRuleReviewTimeout);
                const bool        release  = vd.active ? vd.releaseSlots : true;

                auto cas = rrepo_.casUpdateStatus(conn, id, "PENDING_REVIEW", target);
                if (!cas.ok()) {
                    return VoidResult::fail(cas.code, cas.msg);
                }
                if (cas.data == 0) {
                    return VoidResult::make(Empty{});   // 别人先处理 → 跳过全部副作用
                }
                auto log = writeStatusLog(conn, "RESERVATION", id, "PENDING_REVIEW", target, 0,
                                          "待审核超时，系统自动驳回", ruleCode);
                if (!log.ok()) {
                    return log;
                }
                std::string d;
                int         from = 0;
                int         to   = 0;
                const bool  hasRange = slotRangeOf(cur.startTime, cur.endTime, d, from, to);
                auto rel = releaseAndOffer(conn, id, cur.resourceId, hasRange ? d : std::string(),
                                           from, to, release);
                if (!rel.ok()) {
                    return rel;
                }
                applied = true;
                return VoidResult::make(Empty{});
            });
        if (ran && applied) {
            ++st.pendingRejected;
        }
    }
    return st;
}

// ------------------------------------------------------------------ ② 爽约
MaintenanceStats MaintenanceJob::runNoShow(const std::string& tickId) {
    MaintenanceStats st;
    const std::string nowIso = isoAfterMin(0);

    // 爽约窗口同样以规则参数为准（#6 RESERVE_NO_SHOW.pickupLateMin）
    const int lateMin = engine_.enabled()
                            ? engine_.paramInt("RESERVE_NO_SHOW", "pickupLateMin", pickupCfg_.lateMin)
                            : pickupCfg_.lateMin;

    std::vector<int64_t> ids;
    {
        DbPool::Guard g = pool_.acquire(3000);
        if (!g) {
            ++st.errors;
            return st;
        }
        auto r = rrepo_.listNoShow(*g, lateMin, nowIso, cfg_.batchSize);
        if (!r.ok()) {
            ++st.errors;
            return st;
        }
        ids = r.data;
    }
    if (ids.empty()) {
        return st;
    }
    LOG_INFO("[{}] 爽约候选 {} 单（start_time + {}min < now）", tickId, ids.size(), lateMin);

    for (const int64_t id : ids) {
        bool       applied = false;
        const bool ran     = withLockedReservation(
            id, st, [&](DbConn& conn, const model::Reservation& cur) -> VoidResult {
                if (cur.status != model::ReservationStatus::LOCKED) {
                    return VoidResult::make(Empty{});
                }
                const TimerVerdict vd = judgeTimer(conn, cur, ActionType::Transition, 0, "TIMEOUT");
                if (vd.active && !vd.hit) {
                    return VoidResult::make(Empty{});
                }
                const std::string target = vd.active && !vd.targetStatus.empty()
                                               ? vd.targetStatus
                                               : std::string("CANCELLED");
                const std::string ruleCode = vd.active ? vd.ruleCode : std::string(kRuleNoShow);
                const bool        release  = vd.active ? vd.releaseSlots : true;
                const int         score    = vd.active && vd.deductScore > 0 ? vd.deductScore
                                                                           : cfg_.noShowDeductScore;

                auto cas = rrepo_.casUpdateStatus(conn, id, "LOCKED", target);
                if (!cas.ok()) {
                    return VoidResult::fail(cas.code, cas.msg);
                }
                if (cas.data == 0) {
                    return VoidResult::make(Empty{});
                }
                auto log = writeStatusLog(conn, "RESERVATION", id, "LOCKED", target, 0,
                                          "超时未取用，系统取消预约", ruleCode);
                if (!log.ok()) {
                    return log;
                }
                auto pen = settlement_.penalize(conn, id, cur.userId,
                                               model::toStr(model::ViolationType::NO_SHOW), 0, score,
                                               0, "爽约未取用", ruleCode);
                if (!pen.ok() && pen.code != kSlotUnavailable) {
                    return VoidResult::fail(pen.code, pen.msg);
                }
                std::string d;
                int         from = 0;
                int         to   = 0;
                const bool  hasRange = slotRangeOf(cur.startTime, cur.endTime, d, from, to);
                auto rel = releaseAndOffer(conn, id, cur.resourceId, hasRange ? d : std::string(),
                                           from, to, release);
                if (!rel.ok()) {
                    return rel;
                }
                applied = true;
                return VoidResult::make(Empty{});
            });
        if (ran && applied) {
            ++st.noShowCancelled;
        }
    }
    return st;
}

// ------------------------------------------------------------------ ③ 逾期迁移 + ④ 逾期待续
MaintenanceStats MaintenanceJob::runOverdue(const std::string& tickId) {
    MaintenanceStats st;
    const std::string nowIso = isoAfterMin(0);

    // ---- ③ IN_USE → DUE_RETURN（不释放槽：时段仍归该单）
    {
        std::vector<int64_t> ids;
        {
            DbPool::Guard g = pool_.acquire(3000);
            if (!g) {
                ++st.errors;
                return st;
            }
            auto r = rrepo_.listOverdueInUse(*g, nowIso, cfg_.batchSize);
            if (!r.ok()) {
                ++st.errors;
                return st;
            }
            ids = r.data;
        }
        if (!ids.empty()) {
            LOG_INFO("[{}] 逾期未还候选 {} 单", tickId, ids.size());
        }
        for (const int64_t id : ids) {
            bool       applied = false;
            const bool ran     = withLockedReservation(
                id, st, [&](DbConn& conn, const model::Reservation& cur) -> VoidResult {
                    if (cur.status != model::ReservationStatus::IN_USE) {
                        return VoidResult::make(Empty{});
                    }
                    const TimerVerdict vd =
                        judgeTimer(conn, cur, ActionType::Transition, 0, "REPORT_DUE");
                    if (vd.active && !vd.hit) {
                        return VoidResult::make(Empty{});
                    }
                    const std::string target = vd.active && !vd.targetStatus.empty()
                                                   ? vd.targetStatus
                                                   : std::string("DUE_RETURN");
                    const std::string ruleCode = vd.active ? vd.ruleCode : std::string(kRuleOverdue);
                    const int         score    = vd.active && vd.deductScore > 0
                                                     ? vd.deductScore
                                                     : cfg_.overdueDeductScore;

                    auto cas = rrepo_.casUpdateStatus(conn, id, "IN_USE", target);
                    if (!cas.ok()) {
                        return VoidResult::fail(cas.code, cas.msg);
                    }
                    if (cas.data == 0) {
                        return VoidResult::make(Empty{});
                    }
                    auto log = writeStatusLog(conn, "RESERVATION", id, "IN_USE", target, 0,
                                              "已过结束时间仍未归还", ruleCode);
                    if (!log.ok()) {
                        return log;
                    }
                    // 本周期 seq=0 的扣分在迁移时完成（④ 会跳过 seq==0，避免双扣）
                    auto pen = settlement_.penalize(conn, id, cur.userId,
                                                   model::toStr(model::ViolationType::OVERDUE), 0,
                                                   score, 0, "逾期未还", ruleCode);
                    if (!pen.ok() && pen.code != kSlotUnavailable) {
                        return VoidResult::fail(pen.code, pen.msg);
                    }
                    auto conv = convergeResource(conn, cur.resourceId, 0, "定时任务：逾期，资源收敛");
                    if (!conv.ok()) {
                        return conv;
                    }
                    applied = true;
                    return VoidResult::make(Empty{});
                });
            if (ran && applied) {
                ++st.overdueMarked;
            }
        }
    }

    // ---- ④ 逾期待续：不迁移状态，只按周期序号扣分
    {
        std::vector<repo::ReservationRepo::OverdueCandidate> cands;
        DbPool::Guard g = pool_.acquire(3000);
        if (!g) {
            ++st.errors;
            return st;
        }
        DbConn& conn    = *g;
        const int maxTimes = engine_.enabled()
                                 ? engine_.paramInt("RESERVE_OVERDUE_REPEAT", "maxTimes",
                                                    cfg_.overdueDeductMaxTimes)
                                 : cfg_.overdueDeductMaxTimes;
        auto r = rrepo_.listOverdueDueReturn(conn, cfg_.overdueDeductPeriodMin, nowIso,
                                             cfg_.batchSize);
        if (!r.ok()) {
            ++st.errors;
            return st;
        }
        cands = r.data;
        if (cands.empty()) {
            return st;
        }
        LOG_INFO("[{}] 逾期待续候选 {} 单（period={}min，上限 {} 次）", tickId, cands.size(),
                 cfg_.overdueDeductPeriodMin, maxTimes);

        for (const auto& c : cands) {
            if (c.deductSeq <= 0) {
                continue;   // seq=0 已在 ③ 迁移时扣过
            }
            if (maxTimes > 0 && c.deductSeq > maxTimes) {
                continue;   // 超过上限：只停扣分，不迁移状态（D-P2-7）
            }
            // 精判：把「该扣不该扣 / 扣多少」交给规则（#8 RESERVE_OVERDUE_REPEAT）
            auto curRes = rrepo_.findById(conn, c.id);
            if (!curRes.ok() || !curRes.data.has_value()) {
                continue;
            }
            const TimerVerdict vd =
                judgeTimer(conn, *curRes.data, ActionType::DeductOnly, c.deductSeq, "");
            if (vd.active && !vd.hit) {
                continue;
            }
            const std::string ruleCode = vd.active && !vd.ruleCode.empty() ? vd.ruleCode
                                                                          : std::string(kRuleOverdue);
            const int score = vd.active && vd.deductScore > 0 ? vd.deductScore
                                                              : cfg_.overdueDeductScore;
            // 扣分靠 uk_violation_dedup / uk_credit_source 幂等，无需状态锁
            auto pen = settlement_.penalize(conn, c.id, c.userId,
                                           model::toStr(model::ViolationType::OVERDUE), c.deductSeq,
                                           score, 0,
                                           "逾期未还，第 " + std::to_string(c.deductSeq) + " 周期",
                                           ruleCode);
            if (pen.ok()) {
                ++st.overdueDeducted;
            } else if (pen.code != kSlotUnavailable) {
                ++st.errors;
                LOG_WARN("逾期待续扣分失败（单 {}）：{}", c.id, pen.msg);
            }
        }
    }
    return st;
}

// ------------------------------------------------------------------ ⑤ offer 过期
MaintenanceStats MaintenanceJob::runOfferExpiry(const std::string& tickId) {
    MaintenanceStats st;
    const std::string nowIso = isoAfterMin(0);

    std::vector<model::WaitlistItem> rows;
    {
        DbPool::Guard g = pool_.acquire(3000);
        if (!g) {
            ++st.errors;
            return st;
        }
        auto r = wrepo_.listExpiredOffers(*g, nowIso, cfg_.batchSize);
        if (!r.ok()) {
            ++st.errors;
            return st;
        }
        rows = r.data;
    }
    if (rows.empty()) {
        return st;
    }
    LOG_INFO("[{}] offer 过期候选 {} 条", tickId, rows.size());

    for (const auto& w : rows) {
        auto lockRes = lock_.acquireResource(w.resourceId, cfg_.lockWaitMs, kMaintLockTtlMs);
        if (!lockRes.ok()) {
            ++st.skippedBusy;
            continue;
        }
        LockHandle lock = std::move(lockRes.data);

        DbPool::Guard guard = pool_.acquire(3000);
        if (!guard) {
            ++st.errors;
            continue;
        }
        DbConn& conn = *guard;

        Transaction tx(conn);
        if (!tx.active()) {
            ++st.errors;
            continue;
        }
        auto cas = wrepo_.casUpdateStatus(conn, w.id, model::toStr(model::WaitlistStatus::OFFERED),
                                          model::toStr(model::WaitlistStatus::EXPIRED), std::nullopt,
                                          std::nullopt);
        if (!cas.ok()) {
            tx.rollback();
            ++st.errors;
            continue;
        }
        if (cas.data == 0) {
            tx.rollback();   // 别人已处理（用户已 confirm 或已退出）
            continue;
        }
        // 用该行原区间链式派给下一位
        auto disp = waitlist_.dispatchOffers(conn, w.resourceId, w.slotDate, w.startIndex,
                                             w.endIndex);
        if (!disp.ok()) {
            tx.rollback();
            ++st.errors;
            continue;
        }
        if (lock.lost()) {
            tx.rollback();
            ++st.errors;
            continue;
        }
        auto committed = tx.commit();
        if (!committed.ok()) {
            ++st.errors;
            continue;
        }
        ++st.offersExpired;
        st.offersDispatched += disp.data;
    }
    return st;
}

// ------------------------------------------------------------------ ⑤ RETURN_CHECK 告警
MaintenanceStats MaintenanceJob::runReturnCheckWarn(const std::string& tickId) {
    MaintenanceStats st;
    const std::string cutoff = isoAfterMin(-cfg_.returnCheckWarnMin);

    std::vector<int64_t> ids;
    {
        DbPool::Guard g = pool_.acquire(3000);
        if (!g) {
            ++st.errors;
            return st;
        }
        auto r = rrepo_.listStuckReturnCheck(*g, cutoff, cfg_.batchSize);
        if (!r.ok()) {
            ++st.errors;
            return st;
        }
        ids = r.data;
    }
    for (const int64_t id : ids) {
        // 主理人决策 2：只告警，不自动通过、不改状态（自动通过等于绕过管理员实物验收）
        LOG_WARN("[{}] 单据 {} 已在 RETURN_CHECK 停留超过 {} 分钟，请管理员尽快验收（本任务不做处理）",
                 tickId, id, cfg_.returnCheckWarnMin);
        ++st.returnCheckWarned;
    }
    return st;
}

// ------------------------------------------------------------------ 一轮全跑
MaintenanceStats MaintenanceJob::runOnce(const std::string& tickId) {
    MaintenanceStats total;
    total.add(runPendingReject(tickId));
    total.add(runNoShow(tickId));
    total.add(runOverdue(tickId));
    total.add(runOfferExpiry(tickId));
    total.add(runReturnCheckWarn(tickId));
    {
        std::lock_guard<std::mutex> lk(statsMu_);
        lastStats_ = total;
    }
    LOG_INFO("[{}] 维护轮次完成：待审驳回={} 爽约={} 逾期={} 逾期待扣={} offer过期={} 派发={} 抢锁跳过={} 告警={} 错误={}",
             tickId, total.pendingRejected, total.noShowCancelled, total.overdueMarked,
             total.overdueDeducted, total.offersExpired, total.offersDispatched, total.skippedBusy,
             total.returnCheckWarned, total.errors);
    return total;
}

MaintenanceStats MaintenanceJob::lastStats() const {
    std::lock_guard<std::mutex> lk(statsMu_);
    return lastStats_;
}

// ============================================================================
// MaintenanceController（ARCH-P2 §3.9：手动跑一轮，验收/排障用）
// 放在本文件而非新文件：文档的「新增文件」清单里没有 MaintenanceController.cpp
// ============================================================================
MaintenanceController::MaintenanceController(Scheduler& scheduler, MaintenanceJob& job)
    : scheduler_(scheduler), job_(job) {}

Result<Json> MaintenanceController::runOnce(const httplib::Request& req, const AuthContext& ctx) {
    (void)req;
    const std::string tickId = "manual-" + Str::randomHex(4);   // 与后台 tick 区分开

    const auto t0 = std::chrono::steady_clock::now();
    // Scheduler 的 job 就是 job_.runOnce(...)，所以这里只调一次即可（不会双跑）。
    // runMu_ 抢不到 → 后台 tick 正在跑 → 3022（非阻塞，不排队）。
    if (!scheduler_.runOnce(tickId)) {
        return Result<Json>::fail(kMaintenanceBusy, "维护任务正在执行，请稍后重试");
    }
    const MaintenanceStats stats = job_.lastStats();
    const auto             ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();

    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "MAINTENANCE_RUN", kOk, 0);

    Json data;
    data["tickId"]     = tickId;
    data["durationMs"] = static_cast<long long>(ms);
    data["stats"]      = stats.toJson();
    return Result<Json>::make(std::move(data));
}

} // namespace rf
