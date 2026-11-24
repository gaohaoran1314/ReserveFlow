// =============================================================================
// src/ReservationService.h  —  预约服务（下单 / 列表 / 详情 / 取消 / 审核 / 驳回 / 占用）
// 对应 ARCH-P1 §3.7 / §1.2 / §1.3 / §5 / D-P1-3 / D-P1-4 / D-P1-5 / D-P1-11
//
// 事务与加锁的铁律（违反会全局死锁，见 D-P1-4）：
//   ① validate（纯计算，无锁无连接）→ ② 抢 Redis 锁（此阶段零连接占用）
//   → ③ 取 DB 连接 → ④ 查库（事务外）→ ⑤ 开事务（唯一事务范围）→ ⑥ 提交前查 lock.lost()
//   任何退出路径都由 RAII 保证「先还连接、再放锁」。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RedisClient.h"
#include "Result.h"
#include "DbPool.h"
#include "Model.h"
#include "AuthService.h"          // AuthContext
#include "AuditRepo.h"
#include "ResourceRepo.h"
#include "UserRepo.h"
#include "ReservationRepo.h"
#include "StateMachine.h"
#include "DistributedLock.h"
#include "SettlementService.h"
#include "WaitlistService.h"
#include "RuleEngine.h"
#include "Config.h"

namespace rf {

/** 下单入参（Controller 解析后填入） */
struct CreateReservationReq {
    int64_t     resourceId = 0;
    std::string startTime;    ///< "yyyy-MM-ddTHH:mm:ss"
    std::string endTime;
    std::string remark;       ///< 可空，<=255
};

/** 对外投影（列表/详情/审核返回） */
struct ReservationView {
    int64_t             id = 0;
    std::string         reservationNo;
    int64_t             userId = 0;
    std::string         username;
    int64_t             resourceId = 0;
    std::string         resourceCode, resourceName;
    model::ResourceType resourceType = model::ResourceType::MEETING_ROOM;
    std::string         startTime, endTime;
    std::string         status, statusLabel;
    double              depositAmount = 0.0;
    std::string         remark, createdAt, updatedAt;
    bool                needReview = false;          ///< LAB_EQUIPMENT 为 true
    std::string         slotDate;                    ///< "yyyy-MM-dd"
    std::vector<int>    slotIndexes;
    std::vector<std::string> allowedActions;         ///< 当前可用事件名，供前端置灰

    // ---- P2 增量（ARCH-P2 附录 B）：验收 / 损坏响应需要的附加字段 ----
    // 为空 / 未设置时 toJson 不输出这些键，保证 P1 的响应形状完全不变。
    nlohmann::json deposit;                          ///< 押金账目（accept/damage 填充）
    std::string    resourceStatus;                   ///< 资源最终状态（accept/damage 填充）
    int            offersDispatched = -1;            ///< 派发候补数（-1 表示不输出）
    nlohmann::json violation;                        ///< 违约信息（damage 填充）
    int            creditScoreAfter = -1;            ///< 扣分后信用分（damage 填充）

    nlohmann::json toJson() const;
    /** 列表用轻量 JSON（不含 slotIndexes / allowedActions） */
    nlohmann::json toListJson() const;
};

/** 预约服务 */
class ReservationService {
public:
    /**
     * @param pool      连接池
     * @param redis     Redis（单号 Lua；Redis 不可用即拒单，D-P1-11）
     * @param rrepo     预约仓储
     * @param rr        资源仓储
     * @param ur        账号仓储
     * @param ar        留痕仓储
     * @param sm        状态机（phase 来自 config.feature.phase）
     * @param lock      分布式锁（waitMs / ttlMs 来自 config.lock）
     * @param lockWaitMs 抢锁总等待上限（默认 3000，与 config.lock.waitMs 一致）
     * @param lockTtlMs  锁 TTL（默认 30000，与 config.lock.ttlMs 一致）
     * @param minCreditScore 低于该信用分禁止下单（默认 60，0 表示不拦）
     */
    ReservationService(DbPool& pool, RedisClient& redis, repo::ReservationRepo& rrepo,
                       repo::ResourceRepo& rr, repo::UserRepo& ur, repo::AuditRepo& ar,
                       StateMachine& sm, DistributedLock& lock, SettlementService& settlement,
                       WaitlistService& waitlist, RuleEngine& engine,
                       const PickupConfig& pickupCfg, const MaintenanceConfig& maintCfg,
                       int lockWaitMs = 3000, int lockTtlMs = 30000, int minCreditScore = 60);

    /** 下单（§1.2 的 11 步）；成功返回含单号与最终状态的视图 */
    Result<ReservationView> create(const CreateReservationReq& req, const AuthContext& ctx);

    /** 我的预约 / 管理员全量（按 ctx.role 决定 userId 过滤） */
    Result<repo::ReservationPage> list(const repo::ReservationQuery& q, const AuthContext& ctx);

    /**
     * 列表的完整 JSON（P3 T10）：在 list() 基础上补 `resourceStatus` 与 `deposit`。
     * 押金用**一次** GROUP BY 覆盖整页单据，不做 N+1。
     */
    Result<nlohmann::json> listJson(const repo::ReservationQuery& q, const AuthContext& ctx);

    /** 详情：本人或管理员，否则 3008 */
    Result<ReservationView> detail(int64_t id, const AuthContext& ctx);

    /** 取消：本人或管理员；释放槽 + CANCELLED 迁移 + 资源收敛 */
    Result<Empty> cancel(int64_t id, const std::string& reason, const AuthContext& ctx);

    // ==================== P2 增量：取用 / 归还 / 验收 / 损坏 / 押金流水 ====================

    /**
     * 取用（START_USE）。本人或管理员。
     * 时间窗：now ∈ [start_time - pickup.earlyMin, start_time + pickup.lateMin]
     *   太早 → 3015；太晚 → 3016（提示将被系统取消，本方法不迁移状态）
     */
    Result<ReservationView> pickup(int64_t id, const AuthContext& ctx);

    /**
     * 归还（SUBMIT_RETURN）：IN_USE / DUE_RETURN → RETURN_CHECK。
     * 同时释放槽位（D-P2-8，让时段立刻可给别人），资源收敛为 RETURN_CHECK，**不派发候补**。
     */
    Result<ReservationView> returnReservation(int64_t id, const std::string& remark,
                                              const AuthContext& ctx);

    /**
     * 验收通过（PASS_CHECK，仅管理员）：RETURN_CHECK → COMPLETED。
     * 释放剩余槽（幂等，D-P2-9）→ 资源收敛 FREE → 押金退还剩余 → 派发候补。
     */
    Result<ReservationView> accept(int64_t id, const std::string& remark, const AuthContext& ctx);

    /**
     * 标记损坏（MARK_DAMAGE，仅管理员）：RETURN_CHECK / DUE_RETURN → COMPLETED。
     * 资源 → DAMAGE_CLAIM；押金按比例/指定额扣减后退还剩余；写 violation(DAMAGE)；
     * 扣信用分；作废该资源全部候补。
     * @param deductAmount <=0 表示按 deposit.damageDeductRatio 自动计算
     */
    Result<ReservationView> damage(int64_t id, double deductAmount, const std::string& reason,
                                   const AuthContext& ctx);

    /** 该单押金流水 + 账目聚合（本人或管理员） */
    Result<nlohmann::json> deposits(int64_t id, const AuthContext& ctx);

    /**
     * 该单状态时间线（P4 T00 新增，只读）。本人或管理员。
     * @return `{reservationId, total, list:[{fromStatus,toStatus,operatorId,reason,ruleCode?,createdAt}]}`
     * @note 按时间**倒序**（最新在前），与管理后台"最近发生了什么"的阅读习惯一致。
     */
    Result<nlohmann::json> timeline(int64_t id, const AuthContext& ctx);

    /**
     * 单号生成（P2 抽成 static，供 WaitlistService 的 offer 转正复用，避免两处格式漂移）。
     * 格式 RV + yyyyMMdd + 4 位流水；Redis 不可用即失败（D-P1-11）。
     */
    static Result<std::string> generateReservationNo(RedisClient& redis,
                                                     const std::string& yyyymmdd);

    /** 管理员审核通过：PENDING_REVIEW → LOCKED（槽位不变） */
    Result<ReservationView> approve(int64_t id, const std::string& remark, const AuthContext& ctx);

    /** 管理员驳回：PENDING_REVIEW → REJECTED（释放槽） */
    Result<ReservationView> reject(int64_t id, const std::string& reason, const AuthContext& ctx);

    /** 某资源某日已占用槽位（前端画可选时段） */
    Result<nlohmann::json> occupiedSlots(int64_t resourceId, const std::string& date,
                                         const AuthContext& ctx);

private:
    /** 单号：RV + yyyyMMdd + 4 位（Lua INCR，D-P1-5 / D-P1-11） */
    Result<std::string> nextReservationNo(const std::string& yyyymmdd);

    /**
     * 事务内写一条 status_log。
     * @param ruleCode P3 起由引擎填充命中规则的 `rule_code`（如 RESERVE_NO_SHOW）；
     *                 非规则触发的迁移传空串 → 写 NULL（P0/P1/P2 行为不变）
     */
    Result<Empty> writeStatusLog(DbConn& conn, BizType biz, int64_t businessId,
                                 const std::string& from, const std::string& to,
                                 const AuthContext& ctx, const std::string& reason,
                                 const std::string& ruleCode = std::string());

    /** 资源状态收敛并落库 + 留痕（D-P1-2）；状态未变化则跳过写库 */
    Result<Empty> convergeResource(DbConn& conn, int64_t resourceId, int64_t excludeReservationId,
                                   const AuthContext& ctx, const std::string& why);

    /** 组装视图（含槽位与 allowedActions） */
    Result<ReservationView> buildView(DbConn& conn, int64_t id);

    /**
     * 短连接读一次预约（读完立刻归还连接）。
     * 用途：取消/审核/驳回需要先拿到 resourceId 才能加锁，而等锁期间**不得**占连接
     * （D-P1-4）。故先短读定位资源，再抢锁，最后在锁内用持锁连接重读真相。
     */
    Result<std::optional<model::Reservation>> loadReservation(int64_t id);

    /** 终点后统一收口：删槽 + 资源收敛 + 押金/留痕都在事务内（P2） */
    Result<Empty> releaseSlotsQuietly(DbConn& conn, int64_t reservationId);

    /** 规范化列表查询（角色过滤 + 字面量校验），list / listJson 共用 */
    Result<repo::ReservationQuery> normalizeQuery(const repo::ReservationQuery& q,
                                                  const AuthContext& ctx) const;

    /** 把 ReservationRow + 押金聚合组装成一个 view（列表与详情共用口径） */
    static ReservationView viewOfRow(const repo::ReservationRow& row);

    DbPool&                pool_;
    RedisClient&           redis_;
    repo::ReservationRepo& rrepo_;
    repo::ResourceRepo&    rr_;
    repo::UserRepo&        ur_;
    repo::AuditRepo&       ar_;
    StateMachine&          sm_;
    DistributedLock&       lock_;
    SettlementService&     settlement_;
    WaitlistService&       waitlist_;
    RuleEngine&            engine_;
    PickupConfig           pickupCfg_;
    MaintenanceConfig      maintCfg_;
    int                    lockWaitMs_      = 3000;
    int                    lockTtlMs_       = 30000;
    int                    minCreditScore_  = 60;
};

} // namespace rf
