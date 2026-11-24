// =============================================================================
// src/WaitlistRepo.h  —  候补队列 SQL（无状态）
// 对应 ARCH-P2 §3.5 / §7 / D-P2-13
//
// 为什么用表而不是 Redis ZSET（§7.1）：offer 转正必须在同一事务里完成
// 「CAS 候补状态 + 插预约 + 占槽 + 写流水 + 写留痕」，Redis 无法与 MySQL 事务对齐。
//
// FIFO 语义：ORDER BY created_at ASC, id ASC —— 严格先进先出，
// verify.mjs 可直接断言「第 N 个拿到的就是第 N 个入队的」。
// 全部状态迁移走 CAS（WHERE id=? AND status=?），affectedRows=0 即跳过。
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"

namespace rf::repo {

/** 候补查询条件 */
struct WaitlistQuery {
    int                        page = 1;
    int                        size = 10;
    std::optional<int64_t>     userId;      ///< Service 按角色填：普通用户=自己
    std::optional<int64_t>     resourceId;
    std::optional<std::string> status;      ///< WAITING / OFFERED / CONVERTED / CANCELLED / EXPIRED
};

struct WaitlistPage {
    std::vector<model::WaitlistItem> list;
    int64_t                          total = 0;
    int                              page  = 1;
    int                              size  = 10;
};

class WaitlistRepo {
public:
    WaitlistRepo()  = default;
    ~WaitlistRepo() = default;
    WaitlistRepo(const WaitlistRepo&)            = delete;
    WaitlistRepo& operator=(const WaitlistRepo&) = delete;

    Result<int64_t> insert(DbConn& db, const model::WaitlistItem& w);

    Result<std::optional<model::WaitlistItem>> findById(DbConn& db, int64_t id);

    /**
     * 锁内重读（SELECT ... FOR UPDATE）。调用方必须已在事务内。
     * 用途：confirm / 退队 / 派发前的真相读取。
     */
    Result<std::optional<model::WaitlistItem>> findByIdForUpdate(DbConn& db, int64_t id);

    /** 分页查（total 同条件 COUNT(*)）；附带资源编号/名称 */
    Result<WaitlistPage> search(DbConn& db, const WaitlistQuery& q);

    /** 同用户同资源同日同窗口是否已有 WAITING 行（入队去重，D-P2-13） */
    Result<bool> existsWaiting(DbConn& db, int64_t resourceId, int64_t userId,
                               const std::string& slotDate, int startIndex, int endIndex);

    /** 同用户 WAITING + OFFERED 总数（配额检查） */
    Result<int64_t> countActiveByUser(DbConn& db, int64_t userId);

    /**
     * FIFO 名次（1 起）：同资源同日、created_at 更早的 WAITING 行数 + 1。
     * 非 WAITING 行返回 0。
     */
    Result<int64_t> positionOf(DbConn& db, const model::WaitlistItem& w);

    /**
     * FIFO 选人：窗口完整落在 [fromIndex, toIndex) 内的最早 WAITING 行。
     * 走 idx_wait_ready(resource_id, slot_date, status, created_at)。
     * @param forUpdate  true 时加 FOR UPDATE（事务内使用）
     * @param excludeIds 本次派发中已跳过（窗口被占）的 id，避免反复选中同一条
     */
    Result<std::optional<model::WaitlistItem>> pickFirstEligible(
        DbConn& db, int64_t resourceId, const std::string& slotDate, int fromIndex, int toIndex,
        bool forUpdate, const std::vector<int64_t>& excludeIds);

    /**
     * 同资源同日是否已有 **OFFERED** 窗口与本区间重叠。
     * 用途：派发时防「同一时段重复发 offer」——槽表里看不到尚未 confirm 的 offer。
     */
    Result<bool> hasOfferedOverlap(DbConn& db, int64_t resourceId, const std::string& slotDate,
                                   int fromIndex, int toIndex);

    /** CAS 改状态（带可选的 offer 过期时间 / 转正单号） */
    Result<int64_t> casUpdateStatus(DbConn& db, int64_t id, const std::string& fromStatus,
                                    const std::string& toStatus,
                                    const std::optional<std::string>& offerExpiresAt,
                                    const std::optional<int64_t>& convertedReservationId);

    /** 查已过期的 OFFERED（定时任务扫描） */
    Result<std::vector<model::WaitlistItem>> listExpiredOffers(DbConn& db,
                                                               const std::string& nowIso, int limit);

    /** 资源不可预约时作废其全部 WAITING/OFFERED（损坏/停用场景） */
    Result<int64_t> cancelAllForResource(DbConn& db, int64_t resourceId, const std::string& reason);
};

} // namespace rf::repo
