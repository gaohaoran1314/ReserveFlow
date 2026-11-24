// =============================================================================
// src/MaintenanceJob.h  —  维护任务：四类扫描 + RETURN_CHECK 告警（ARCH-P2 §3.2 / §5）
//
// 本类**不自带线程**（线程在 Scheduler），因此可在单测 / ADMIN 接口里直接 runOnce()。
//
// 三层递进裁决（D-P2-4）：
//   ① 同一把资源锁把定时任务与用户操作串行化（waitMs=500，抢不到就跳过，绝不阻塞用户）；
//   ② 进锁后**重读**真相（候选是锁外算的，可能已过期）；
//   ③ CAS `WHERE status=?` + affectedRows 裁决，0 即「别人已处理」→ 跳过全部副作用。
//
// 副作用只在 CAS 成功之后（D-P2-5）：删槽 / 收敛 / 扣分 / violation / 派候补。
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"
#include "DbPool.h"
#include "Model.h"
#include "DistributedLock.h"
#include "StateMachine.h"
#include "ReservationRepo.h"
#include "ResourceRepo.h"
#include "AuditRepo.h"
#include "WaitlistRepo.h"
#include "SettlementService.h"
#include "WaitlistService.h"
#include "RuleEngine.h"
#include "Config.h"

namespace rf {

/** 一轮维护的统计（写日志 + 手动触发接口返回） */
struct MaintenanceStats {
    int pendingRejected  = 0;   ///< 待审核超时 → REJECTED
    int noShowCancelled  = 0;   ///< 爽约 → CANCELLED
    int overdueMarked    = 0;   ///< IN_USE → DUE_RETURN
    int overdueDeducted  = 0;   ///< 逾期待续扣分成功次数
    int offersExpired    = 0;   ///< offer 过期 → EXPIRED
    int offersDispatched = 0;   ///< 新派发 offer 数
    int skippedBusy      = 0;   ///< 抢锁失败跳过
    int errors           = 0;   ///< 单条处理异常（不中断整轮）
    int returnCheckWarned = 0;  ///< 长期停在 RETURN_CHECK 的单据（**只告警，不改状态**）

    void add(const MaintenanceStats& o);
    nlohmann::json toJson() const;
};

/**
 * 维护任务：四类扫描 + 收口动作。
 * 本类**不自带线程**（线程在 Scheduler），因此可在单测里直接调 runOnce()。
 */
class MaintenanceJob {
public:
    MaintenanceJob(DbPool& pool, DistributedLock& lock, StateMachine& sm,
                   repo::ReservationRepo& rrepo, repo::ResourceRepo& rr, repo::AuditRepo& ar,
                   repo::WaitlistRepo& wrepo, SettlementService& settlement,
                   WaitlistService& waitlist, RuleEngine& engine, const MaintenanceConfig& cfg,
                   const PickupConfig& pickupCfg);

    /** 执行一轮全部扫描；任何单条异常只计 errors，不抛出 */
    MaintenanceStats runOnce(const std::string& tickId);

    /** 最近一轮的统计（供 ADMIN 手动触发接口回显；runMu_ 保证同时只有一轮在跑） */
    MaintenanceStats lastStats() const;

    /** 单独触发某一类（供精准测试，顺序无关） */
    MaintenanceStats runPendingReject(const std::string& tickId);
    MaintenanceStats runNoShow(const std::string& tickId);
    MaintenanceStats runOverdue(const std::string& tickId);
    MaintenanceStats runOfferExpiry(const std::string& tickId);
    /** RETURN_CHECK 长期无人验收：**只写 WARN + 计数，不改状态**（主理人决策 2） */
    MaintenanceStats runReturnCheckWarn(const std::string& tickId);

private:
    /**
     * 逐单执行骨架：短读定位资源 → 短锁（抢不到计 skippedBusy）→ 取连接 → 事务
     *  → 锁内重读 → 回调（在这里做 CAS + 副作用）→ lock.lost 检查 → 提交。
     * @return true 表示回调真的执行并提交了
     */
    bool withLockedReservation(
        int64_t reservationId, MaintenanceStats& st,
        const std::function<VoidResult(DbConn&, const model::Reservation&)>& body);

    /**
     * 统一收口：释放槽位 + 资源收敛 + 派发候补。
     * 调用前提：已持有资源锁、已在事务内、预约单已 CAS 到终态。
     */
    VoidResult releaseAndOffer(DbConn& conn, int64_t reservationId, int64_t resourceId,
                               const std::string& slotDate, int fromIndex, int toIndex,
                               bool dispatchOffer);

    /** 带 rule_code 的留痕（定时任务写 RESERVE_* 规则码，供 P3 统计） */
    VoidResult writeStatusLog(DbConn& conn, const char* bizType, int64_t businessId,
                              const std::string& from, const std::string& to, int64_t operatorId,
                              const std::string& reason, const std::string& ruleCode);

    /**
     * 装配 TIMER 阶段的事实（§5.3）。
     * notes：候选是 SQL 粗筛出来的，`deductSeq` 由 SQL 算好传进来（引擎不自己算）；
     *        user.* 与 resource.* 也要给全 —— 管理员可能新增引用它们的时间类规则。
     */
    FactBag timerFacts(DbConn& conn, const model::Reservation& cur, int deductSeq) const;

    /**
     * TIMER 精判：返回「引擎是否认为该单该动」+ 命中规则码 + 分值。
     * 引擎关闭或降级时 `active=false`，调用方直接走 P2 原逻辑（byte-identical）。
     */
    struct TimerVerdict {
        bool        active   = false;   ///< false = 引擎未参与，走 P2
        bool        hit      = true;    ///< 是否该执行动作
        std::string ruleCode;           ///< 命中规则码（P2 时为空，用常量兜底）
        std::string event;              ///< transition 的 event（TIMEOUT / REPORT_DUE）
        std::string targetStatus;       ///< transition 的目标态
        bool        releaseSlots = false;
        bool        deductCredit = false;
        int         deductScore  = 0;
    };
    TimerVerdict judgeTimer(DbConn& conn, const model::Reservation& cur, ActionType want,
                            int deductSeq, const std::string& expectEvent);

    /** 资源态收敛 + 资源留痕（与 ReservationService::convergeResource 同口径） */
    VoidResult convergeResource(DbConn& conn, int64_t resourceId, int64_t excludeReservationId,
                                const std::string& why);

    DbPool&                pool_;
    DistributedLock&       lock_;
    StateMachine&          sm_;
    repo::ReservationRepo& rrepo_;
    repo::ResourceRepo&    rr_;
    repo::AuditRepo&       ar_;
    repo::WaitlistRepo&    wrepo_;
    SettlementService&     settlement_;
    WaitlistService&       waitlist_;
    RuleEngine&            engine_;
    MaintenanceConfig      cfg_;
    PickupConfig           pickupCfg_;
    mutable std::mutex     statsMu_;
    MaintenanceStats       lastStats_;
};

} // namespace rf
