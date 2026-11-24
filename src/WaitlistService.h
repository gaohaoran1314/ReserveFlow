// =============================================================================
// src/WaitlistService.h  —  候补队列业务（入队 / 退队 / 我的候补 / 派发 / 转正）
// 对应 ARCH-P2 §3.6 / §7.1 / §7.3 / §7.4 / §7.5 / D-P2-13
//
// 本轮（T05）实现：join / leave / list。
// confirm / dispatchOffers / createReservationForOffer 属 T08，**声明已按 §3.6 落定**，
// 定义在 T08 补齐（当前无调用点，不影响链接）。
//
// 加锁顺序沿用 P1 铁律：纯校验 → 抢资源锁（零连接）→ 取连接 → 锁内真相 → 事务 → CAS。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "RedisClient.h"
#include "Model.h"
#include "AuthService.h"        // AuthContext
#include "WaitlistRepo.h"
#include "ReservationRepo.h"
#include "ResourceRepo.h"
#include "UserRepo.h"
#include "AuditRepo.h"
#include "StateMachine.h"
#include "DistributedLock.h"
#include "SettlementService.h"
#include "Config.h"

namespace rf {

/** 加入候补入参 */
struct JoinWaitlistReq {
    int64_t     resourceId = 0;
    std::string startTime;      ///< "yyyy-MM-ddTHH:mm:ss"，与 POST /reservations 同格式
    std::string endTime;
    std::string remark;
};

class WaitlistService {
public:
    WaitlistService(DbPool& pool, RedisClient& redis, repo::WaitlistRepo& wrepo,
                    repo::ReservationRepo& rrepo, repo::ResourceRepo& rr, repo::UserRepo& ur,
                    repo::AuditRepo& ar, StateMachine& sm, DistributedLock& lock,
                    SettlementService& settlement, const WaitlistConfig& wcfg,
                    int lockWaitMs = 3000, int lockTtlMs = 30000, int minCreditScore = 60);

    /** 加入候补（去重 + 配额 + 时段合法性 + 资源存在） */
    Result<model::WaitlistItem> join(const JoinWaitlistReq& req, const AuthContext& ctx);

    /** 我的候补列表（管理员可按 resourceId / userId 看全部） */
    Result<repo::WaitlistPage> list(const repo::WaitlistQuery& q, const AuthContext& ctx);

    /** 退出候补（本人或管理员）：WAITING / OFFERED → CANCELLED */
    Result<Empty> leave(int64_t waitlistId, const AuthContext& ctx);

    /**
     * 确认 offer → 转正式预约（§7.5）。【T08 实现】
     * 失败：3019 状态不对 / 3021 offer 已过期 / 3010 黑名单 / 3011 信用分 / 3005 槽位又被抢
     */
    Result<model::WaitlistItem> confirm(int64_t waitlistId, const AuthContext& ctx);

    /**
     * 派发 offer（§7.4 贪心 FIFO）。【T08 实现】
     * 调用点：取消 / 驳回 / 超时驳回 / 爽约 / 验收通过 / offer 过期。
     * @param fromIndex,toIndex 被释放的区间（闭开）
     * @param maxOffers 本次最多派几个（默认 5，防长事务）
     * @return 实际派发数
     */
    Result<int> dispatchOffers(DbConn& conn, int64_t resourceId, const std::string& slotDate,
                               int fromIndex, int toIndex, int maxOffers = 5);

    /**
     * 资源变为不可预约时，作废其全部 WAITING/OFFERED 候补（损坏 / 停用场景）。
     * 由 ReservationService::damage 在同一事务内调用；返回作废条数。
     */
    Result<int64_t> cancelAllForResource(DbConn& conn, int64_t resourceId,
                                         const std::string& reason);

    /**
     * 手动派发（ADMIN 调试/验收入口）：自己取连接 + 开事务，再调 dispatchOffers。
     * 生产路径不经过这里——派发都由释放方在同一个事务内调 dispatchOffers。
     */
    Result<int> dispatchDebug(int64_t resourceId, const std::string& slotDate, int fromIndex,
                              int toIndex, const AuthContext& ctx);

    const WaitlistConfig& config() const { return cfg_; }

private:
    /** confirm 内部：真正创建预约（复用 ReservationService 的校验与插槽逻辑）。【T08 实现】 */
    Result<int64_t> createReservationForOffer(DbConn& conn, const model::WaitlistItem& w,
                                              const AuthContext& ctx);

    DbPool&                pool_;
    RedisClient&           redis_;
    repo::WaitlistRepo&    wrepo_;
    repo::ReservationRepo& rrepo_;
    repo::ResourceRepo&    rr_;
    repo::UserRepo&        ur_;
    repo::AuditRepo&       ar_;
    StateMachine&          sm_;
    DistributedLock&       lock_;
    SettlementService&     settlement_;
    WaitlistConfig         cfg_;
    int                    lockWaitMs_ = 3000;
    int                    lockTtlMs_  = 30000;
    int                    minCreditScore_ = 60;   ///< confirm 时的信用分门槛（与下单一一致）
};

} // namespace rf
