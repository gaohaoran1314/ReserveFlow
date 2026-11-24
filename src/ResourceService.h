// =============================================================================
// src/service/ResourceService.h  —  资源服务（检索 / 详情 / 字典 / 增改删 / 改状态）
// 对应 ARCH-P0 §3.10 / D-3 / D-4 / D-5 / D-8 / D-10 / D-13
// 职责边界：唯一性校验、半更新合并、删除保护、事务内写 status_log + AUDIT 日志。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"
#include "AuditRepo.h"
#include "ResourceRepo.h"
#include "AuthService.h"   // AuthContext

namespace rf {

/** 资源服务 */
class ResourceService {
public:
    ResourceService(DbPool& pool, repo::ResourceRepo& rr, repo::AuditRepo& ar);
    ~ResourceService() = default;
    ResourceService(const ResourceService&)            = delete;
    ResourceService& operator=(const ResourceService&) = delete;

    /** 分页检索：page/size 越界在此收敛（D-8） */
    Result<model::PageResult> list(const model::ResourceQuery& q);

    /** 详情；已删除或不存在 → 3000 */
    Result<model::Resource> detail(int64_t id);

    /** 字典：类型枚举 / 状态枚举（含 selectable）/ 地点候选 */
    Result<nlohmann::json> meta();

    /**
     * 新增资源（管理员）。
     * 事务：insert + status_log('' → FREE)；编号重复 → 3001（D-3 / D-10）。
     * @return 新资源 id
     */
    Result<int64_t> create(const model::ResourceCreateReq& req, const AuthContext& ctx);

    /** 半更新（管理员）：只改提交字段；不存在的 id → 3000 */
    Result<model::Resource> update(int64_t id, const model::ResourceUpdateReq& req,
                                   const AuthContext& ctx);

    /**
     * 逻辑删除（管理员）。
     * 存在有效预约 → 3003；已删 → 幂等成功；成功 msg 带「编号 XXX 将永久保留，不可再次使用」。
     */
    Result<Empty> remove(int64_t id, const AuthContext& ctx);

    /**
     * 手工改状态（管理员，D-5）。
     * 非法枚举字面量 → 1000；合法枚举但 P0 不可写（IN_USE 等）→ 3002。
     */
    Result<model::Resource> changeStatus(int64_t id, const std::string& rawStatus,
                                         const AuthContext& ctx);

private:
    /** 事务内写一条 status_log（ruleCode 恒为 nullopt，D-12） */
    Result<Empty> writeStatusLog(DbConn& conn, int64_t resourceId, const std::string& from,
                                 const std::string& to, const AuthContext& ctx,
                                 const std::string& reason);

    DbPool&             pool_;
    repo::ResourceRepo& rr_;
    repo::AuditRepo&    ar_;
};

} // namespace rf
