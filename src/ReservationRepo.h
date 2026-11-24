// =============================================================================
// src/ReservationRepo.h  —  预约仓储（reservation + reservation_slot，无状态）
// 对应 ARCH-P1 §3.6 / §1.5 / D-P1-3 / D-P1-6
//
// 职责边界（沿用 P0 约定）：
//   1) 只做参数化 SQL 与行 ↔ 结构体映射，不做业务判断、不开事务；
//   2) 唯一索引冲突（ER_DUP_ENTRY 1062）在此层就转成 3005，绝不漏成 1009
//      —— 这是 D-P1-3 的落地点：锁只是降低冲突率，唯一索引才是最终真相；
//   3) 所有方法首参 DbConn&，连接由 Service 持有，事务由 Service 掌握。
// =============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"

namespace rf::repo {

/** 列表/详情投影：reservation LEFT JOIN resource LEFT JOIN user */
struct ReservationRow {
    int64_t             id            = 0;
    std::string         reservationNo;
    int64_t             userId        = 0;
    std::string         username;
    int64_t             resourceId    = 0;
    std::string         resourceCode, resourceName;
    model::ResourceType resourceType  = model::ResourceType::MEETING_ROOM;
    std::string         startTime, endTime;
    std::string         status;                  ///< 字面量，避免二次转换
    std::string         resourceStatus;          ///< P3 新增：资源当前状态（detail/list 展示）
    double              depositAmount = 0.0;
    std::string         remark, createdAt, updatedAt;
};

/** 列表查询条件（D-8：page/size 越界由 Service 收敛） */
struct ReservationQuery {
    int                        page  = 1;
    int                        size  = 10;
    std::optional<int64_t>     userId;      ///< Service 按角色填：普通用户=自己，管理员=不填
    std::optional<std::string> status;      ///< 已由 Service 校验为合法字面量
    std::optional<int64_t>     resourceId;
};

/** 分页结果（total 由独立 COUNT(*) 得到） */
struct ReservationPage {
    std::vector<ReservationRow> list;
    int64_t                     total = 0;
    int                         page  = 1;
    int                         size  = 10;
};

/** 预约单据 + 时间槽仓储 */
class ReservationRepo {
public:
    ReservationRepo()  = default;
    ~ReservationRepo() = default;
    ReservationRepo(const ReservationRepo&)            = delete;
    ReservationRepo& operator=(const ReservationRepo&) = delete;

    // ---- reservation ----
    /** 插入主表，返回自增 id */
    Result<int64_t> insert(DbConn& db, const model::Reservation& r);

    /** 按 id 取主表实体（无 JOIN） */
    Result<std::optional<model::Reservation>> findById(DbConn& db, int64_t id);

    /** 按 id 取投影（含资源编号/名称/类型、下单人用户名） */
    Result<std::optional<ReservationRow>> findView(DbConn& db, int64_t id);

    /** 分页检索；total 用同条件 COUNT(*) 单独取（§8.4 沿用 P0） */
    Result<ReservationPage> search(DbConn& db, const ReservationQuery& q);

    /** 只改状态列 */
    VoidResult updateStatus(DbConn& db, int64_t id, const std::string& toStatus);

    /**
     * 查该资源下所有「有效预约」的 status（用于资源状态收敛，D-P1-2）。
     * 有效集合 = PENDING_REVIEW / LOCKED / IN_USE / DUE_RETURN / RETURN_CHECK。
     * @param excludeReservationId 排除自身（取消/驳回时用），0 表示不排除
     */
    Result<std::vector<std::string>> listActiveStatuses(DbConn& db, int64_t resourceId,
                                                        int64_t excludeReservationId = 0);

    /** 该用户当前有效预约数（P1 仅用于日志；规则执行属 P3） */
    Result<int64_t> countActiveByUser(DbConn& db, int64_t userId);

    /** 该资源下是否已有其它有效预约（删除保护的替代实现，可选） */
    Result<bool> hasOtherActive(DbConn& db, int64_t resourceId, int64_t excludeReservationId);

    // ---- reservation_slot ----
    /**
     * 查指定日期、指定候选槽中已被占用的槽序号（升序）。
     * 走 uk_slot_resource_date_index 最左前缀 (resource_id, slot_date, slot_index)。
     */
    Result<std::vector<int>> occupiedIndexes(DbConn& db, int64_t resourceId, const std::string& date,
                                             const std::vector<int>& candidates);

    /**
     * 批量插入槽位（单条 INSERT 多 VALUES，N×4 个占位符）。
     * 唯一索引冲突（ER_DUP_ENTRY 1062）→ 直接返回 kSlotUnavailable(3005)，msg 固定文案。
     * 其它 SQL 错误 → kDbError(1009)。
     */
    VoidResult insertSlots(DbConn& db, int64_t reservationId, int64_t resourceId,
                           const std::string& date, const std::vector<int>& indexes);

    /** 取某预约占用的槽序号（升序，详情接口展示用） */
    Result<std::vector<int>> listSlotIndexes(DbConn& db, int64_t reservationId);

    /** 取某预约占用的日期（槽位均在同一天，D-P1-8） */
    Result<std::string> slotDateOf(DbConn& db, int64_t reservationId);

    /** 释放槽位（取消/驳回）：DELETE WHERE reservation_id = ?，返回删除行数 */
    Result<int64_t> deleteSlots(DbConn& db, int64_t reservationId);

    // ========================================================================
    // P2 增量（ARCH-P2 §3.7）
    // 既有方法一字未改：P1 全部路径继续用 updateStatus（它们持锁，天然安全）。
    // ========================================================================

    /**
     * **CAS 状态迁移**：UPDATE reservation SET status=? WHERE id=? AND status=?
     * 这是 P2 全部幂等的基础（D-P2-4 / D-P2-5）。
     * @return data = 受影响行数：1 表示本线程赢下迁移；0 表示别人已处理
     *         （调用方**必须**跳过全部副作用）
     */
    Result<int64_t> casUpdateStatus(DbConn& db, int64_t id, const std::string& fromStatus,
                                    const std::string& toStatus);

    // ---- 定时任务候选查询（都带 LIMIT，走 04_p2_alter.sql 新增的两个索引）----

    /** status='PENDING_REVIEW' AND created_at < cutoffIso（待审核超时） */
    Result<std::vector<int64_t>> listStalePending(DbConn& db, const std::string& cutoffIso,
                                                  int limit);

    /** status='LOCKED' AND TIMESTAMPADD(MINUTE, pickupLateMin, start_time) < nowIso（爽约） */
    Result<std::vector<int64_t>> listNoShow(DbConn& db, int pickupLateMin,
                                            const std::string& nowIso, int limit);

    /** status='IN_USE' AND end_time < nowIso（逾期未还） */
    Result<std::vector<int64_t>> listOverdueInUse(DbConn& db, const std::string& nowIso, int limit);

    /** 逾期待续扣分的候选项 */
    struct OverdueCandidate {
        int64_t id         = 0;
        int64_t userId     = 0;
        int64_t resourceId = 0;
        int     deductSeq  = 0;   ///< TIMESTAMPDIFF(MINUTE, end_time, now) DIV periodMin
    };

    /**
     * status='DUE_RETURN' AND end_time < nowIso，并算出周期序号。
     * `deductSeq == 0` 的由调用方跳过（该周期已在 IN_USE→DUE_RETURN 迁移时扣过）。
     */
    Result<std::vector<OverdueCandidate>> listOverdueDueReturn(DbConn& db, int periodMin,
                                                               const std::string& nowIso, int limit);

    /**
     * status='RETURN_CHECK' AND updated_at < cutoffIso（长期无人验收）。
     * P2 只用于维护任务写 WARN + 计数，**不改状态**（主理人决策 2）。
     */
    Result<std::vector<int64_t>> listStuckReturnCheck(DbConn& db, const std::string& cutoffIso,
                                                      int limit);

    // ========================================================================
    // P3 增量（ARCH-P3 T10）
    // ========================================================================
    /** 押金聚合（一次 GROUP BY 拿一页单据，避免 N+1） */
    struct DepositSummary {
        double frozen   = 0.0;
        double deducted = 0.0;
        double refunded = 0.0;
    };

    /**
     * 批量取押金聚合：**一次查询**覆盖整页单据（T10 判据：列表不许 N+1）。
     * 只返回有流水的单据；无流水的单（零押金）不出现在 map 里。
     */
    Result<std::map<int64_t, DepositSummary>> depositSummaries(DbConn& db,
                                                              const std::vector<int64_t>& ids);

    /** status_log 时间线（detail 接口 timeline 用，**正序**） */
    Result<std::vector<model::StatusLogEntry>> listTimeline(DbConn& db, int64_t reservationId);
};

} // namespace rf::repo
