// =============================================================================
// src/repo/AuditRepo.h  —  status_log 写入与查询（无状态）
// 对应 ARCH-P0 §3.9 / D-12 / D-13 / §8.2
// 口径：只有「真实状态迁移」（创建 ''→FREE、改状态、删除）才写本表；
//       PUT 这类无状态变化的写操作只写应用日志 AUDIT 行。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"

namespace rf::repo {

/** 实体别名：定义在 rf::model（Model.h），此处保留 rf::repo::StatusLogEntry 写法 */
using StatusLogEntry = model::StatusLogEntry;

/** status_log 仓储 */
class AuditRepo {
public:
    AuditRepo()  = default;
    ~AuditRepo() = default;
    AuditRepo(const AuditRepo&)            = delete;
    AuditRepo& operator=(const AuditRepo&) = delete;

    /**
     * 写一条状态留痕。
     * @param db 连接（事务内调用时由 Service 保证仍在同一事务）
     * @param e  留痕内容；e.ruleCode 为 std::nullopt 时写 SQL NULL（P0 恒为 NULL）
     */
    VoidResult insertStatusLog(DbConn& db, const StatusLogEntry& e);

    /**
     * 按业务对象查留痕（新的在前）。
     * @param type       RESOURCE / RESERVATION
     * @param businessId 被变更对象主键
     * @param limit      条数上限（1..500，越界截断）
     */
    Result<std::vector<StatusLogEntry>> listByBusiness(DbConn& db, const std::string& type,
                                                       int64_t businessId, int limit = 50);
};

} // namespace rf::repo
