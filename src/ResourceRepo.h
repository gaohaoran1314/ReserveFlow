// =============================================================================
// src/repo/ResourceRepo.h  —  资源仓储（无状态，方法首参 DbConn&）
// 对应 ARCH-P0 §3.9 / D-4 / D-8 / D-10
// 关键约定：
//   1) 列表与详情查询必须过滤 deleted_at IS NULL；
//   2) findByCode「含已删除」，用于编号唯一性判重（编号永久不复用，D-10）；
//   3) search 的动态 WHERE 与 total 的 COUNT(*) 使用同一条件，值全部绑定。
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

/** 资源 CRUD 与检索 */
class ResourceRepo {
public:
    ResourceRepo()  = default;
    ~ResourceRepo() = default;
    ResourceRepo(const ResourceRepo&)            = delete;
    ResourceRepo& operator=(const ResourceRepo&) = delete;

    /** 按 id 查资源（过滤 deleted_at IS NULL）；data 为 nullopt 表示不存在或已删除 */
    Result<std::optional<model::Resource>> findById(DbConn& db, int64_t id);

    /**
     * 按 id 查资源，**包含已逻辑删除**。
     * 用途：DELETE 的幂等分支需要拿到已删资源的 code 拼提示语（D-10）。
     */
    Result<std::optional<model::Resource>> findByIdAny(DbConn& db, int64_t id);

    /** 按编号查资源，**包含已逻辑删除**（编号唯一性判重，D-10） */
    Result<std::optional<model::Resource>> findByCode(DbConn& db, const std::string& code);

    /**
     * 分页检索：type / status / location / keyword 四种过滤任意组合。
     * total 由同条件的 SELECT COUNT(*) 单独取得（不复用列表长度，§8.4）。
     */
    Result<model::PageResult> search(DbConn& db, const model::ResourceQuery& q);

    /** 新增资源，返回自增 id */
    Result<int64_t> insert(DbConn& db, const model::Resource& r);

    /** 半更新：只 SET 实际提交（optional 有值）的字段 */
    VoidResult update(DbConn& db, int64_t id, const model::ResourceUpdateReq& req);

    /** 逻辑删除：写 deleted_at */
    VoidResult softDelete(DbConn& db, int64_t id);

    /** 只改状态列 */
    VoidResult updateStatus(DbConn& db, int64_t id, model::ResourceState st);

    /**
     * D-4 删除前置校验：统计该资源下「有效预约」条数。
     * 有效集合 = PENDING_REVIEW / LOCKED / IN_USE / DUE_RETURN / RETURN_CHECK。
     * @return 空表返回 0
     */
    Result<int64_t> countActiveReservations(DbConn& db, int64_t resourceId);

    /** DISTINCT 地点清单（未删除、非空地点），按名称升序 */
    Result<std::vector<std::string>> listLocations(DbConn& db);

    /** 存在且未逻辑删除（幂等删除用） */
    Result<bool> existsNotDeleted(DbConn& db, int64_t id);

private:
    /** 行 → 资源实体（复用 model::Resource::fromRow 的列序契约，见 Model.h） */
    static model::Resource fromRow(const Row& row) { return model::Resource::fromRow(row); }
};

} // namespace rf::repo
