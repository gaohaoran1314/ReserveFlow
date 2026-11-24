// =============================================================================
// src/service/ResourceService.cpp  —  资源服务实现
// 对应 ARCH-P0 §3.10 / §6.2 / D-3 / D-4 / D-5 / D-8 / D-10 / D-13
// 事务口径：create / remove / changeStatus 在「同一连接 + 同一事务」内完成
//           业务写 + status_log 写；任一失败整体回滚。
// =============================================================================
#include "ResourceService.h"

#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf {

namespace {

/** 分页参数收敛（D-8 / §8.4） */
int clampPage(int page) { return page < 1 ? 1 : page; }

int clampSize(int size) {
    if (size < 1) {
        return 10;
    }
    if (size > 100) {
        return 100;
    }
    return size;
}

/** 审计动作名（grep 用，保持稳定字面量） */
constexpr const char* kActCreate = "RESOURCE_CREATE";
constexpr const char* kActUpdate = "RESOURCE_UPDATE";
constexpr const char* kActDelete = "RESOURCE_DELETE";
constexpr const char* kActStatus = "RESOURCE_STATUS";

/** 字段长度上限（与 DDL 对齐，避免被 MySQL 静默截断） */
constexpr size_t kMaxCodeLen  = 64;
constexpr size_t kMaxNameLen  = 128;
constexpr size_t kMaxLocLen   = 128;
constexpr size_t kMaxDescLen  = 512;
constexpr int    kMaxCapacity = 100000;

/** 删除成功的提示语（D-10：编号永久保留） */
std::string deleteOkMsg(const std::string& code) {
    return "删除成功；编号 " + code + " 将永久保留，不可再次使用";
}

}  // namespace

ResourceService::ResourceService(DbPool& pool, repo::ResourceRepo& rr, repo::AuditRepo& ar)
    : pool_(pool), rr_(rr), ar_(ar) {}

Result<model::PageResult> ResourceService::list(const model::ResourceQuery& q) {
    model::ResourceQuery qq = q;
    qq.page = clampPage(q.page);
    qq.size = clampSize(q.size);
    if (qq.location.has_value()) {
        qq.location = Str::trim(*qq.location);
    }
    if (qq.keyword.has_value()) {
        qq.keyword = Str::trim(*qq.keyword);
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<model::PageResult>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto r = rr_.search(*guard, qq);
    if (!r.ok()) {
        return Result<model::PageResult>::fail(r.code, r.msg);
    }
    return Result<model::PageResult>::make(std::move(r.data));
}

Result<model::Resource> ResourceService::detail(int64_t id) {
    if (id <= 0) {
        return Result<model::Resource>::fail(kResourceNotFound, "资源不存在");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<model::Resource>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto r = rr_.findById(*guard, id);
    if (!r.ok()) {
        return Result<model::Resource>::fail(r.code, r.msg);
    }
    if (!r.data.has_value()) {
        return Result<model::Resource>::fail(kResourceNotFound, "资源不存在");
    }
    return Result<model::Resource>::make(*r.data);
}

Result<nlohmann::json> ResourceService::meta() {
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<nlohmann::json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto locs = rr_.listLocations(*guard);
    if (!locs.ok()) {
        return Result<nlohmann::json>::fail(locs.code, locs.msg);
    }

    nlohmann::json data = nlohmann::json::object();

    // 类型枚举：全部可选（新增/修改资源时都允许选）
    nlohmann::json types = nlohmann::json::array();
    for (const auto t : {model::ResourceType::LAB_EQUIPMENT, model::ResourceType::MEETING_ROOM,
                         model::ResourceType::LOCKER}) {
        nlohmann::json item = nlohmann::json::object();
        item["value"]       = model::toStr(t);
        item["label"]       = model::labelOf(t);
        types.push_back(std::move(item));
    }
    data["types"] = std::move(types);

    // 状态枚举：全量列出（P1-P3 会用到），selectable 标记 P0 是否允许手工设置
    nlohmann::json statuses = nlohmann::json::array();
    for (const auto s : {model::ResourceState::FREE, model::ResourceState::MAINTENANCE,
                         model::ResourceState::PENDING_REVIEW, model::ResourceState::LOCKED,
                         model::ResourceState::IN_USE, model::ResourceState::DUE_RETURN,
                         model::ResourceState::RETURN_CHECK, model::ResourceState::DAMAGE_CLAIM,
                         model::ResourceState::DISABLED, model::ResourceState::DELETED}) {
        nlohmann::json item = nlohmann::json::object();
        item["value"]       = model::toStr(s);
        item["label"]       = model::labelOf(s);
        item["selectable"]  = model::isP0WritableState(s);
        statuses.push_back(std::move(item));
    }
    data["statuses"] = std::move(statuses);

    nlohmann::json locations = nlohmann::json::array();
    for (const auto& loc : locs.data) {
        locations.push_back(loc);
    }
    data["locations"] = std::move(locations);

    return Result<nlohmann::json>::make(std::move(data));
}

Result<int64_t> ResourceService::create(const model::ResourceCreateReq& req,
                                        const AuthContext& ctx) {
    // ---- 服务层再校验一遍（Controller 已校验，这里防绕过与并发误用）
    const std::string code = Str::trim(req.code);
    const std::string name = Str::trim(req.name);
    if (code.empty() || code.size() > kMaxCodeLen) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActCreate, kInvalidParam,
                  "code:" + code);
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：code 长度必须为 1..64");
    }
    if (name.empty() || name.size() > kMaxNameLen) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActCreate, kInvalidParam,
                  "code:" + code);
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：name 长度必须为 1..128");
    }
    if (req.capacity < 1 || req.capacity > kMaxCapacity) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActCreate, kInvalidParam,
                  "code:" + code);
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：capacity 必须为 1..100000");
    }
    if (req.depositAmount < 0.0) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActCreate, kInvalidParam,
                  "code:" + code);
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：depositAmount 不能为负数");
    }
    if (req.description.size() > kMaxDescLen || req.location.size() > kMaxLocLen) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActCreate, kInvalidParam,
                  "code:" + code);
        return Result<int64_t>::fail(kInvalidParam,
                                     "参数校验失败：description 上限 512、location 上限 128");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<int64_t>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    // ---- 编号唯一性（含已逻辑删除的历史编号，D-10）
    auto dup = rr_.findByCode(conn, code);
    if (!dup.ok()) {
        return Result<int64_t>::fail(dup.code, dup.msg);
    }
    if (dup.data.has_value()) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActCreate, kResourceCodeDup,
                  "code:" + code);
        return Result<int64_t>::fail(kResourceCodeDup, "资源编号已存在");
    }

    model::Resource r;
    r.code          = code;
    r.name          = name;
    r.type          = req.type;
    r.status        = model::ResourceState::FREE;   // 新建资源一律从 FREE 开始
    r.location      = req.location;
    r.capacity      = req.capacity;
    r.depositAmount = req.depositAmount;
    r.description   = req.description;
    r.ownerAdminId  = ctx.userId;

    // ---- 事务：insert + status_log('' → FREE)
    Transaction tx(conn);
    if (!tx.active()) {
        return Result<int64_t>::fail(kDbError, "数据库访问失败：事务开启失败");
    }

    auto ins = rr_.insert(conn, r);
    if (!ins.ok()) {
        tx.rollback();
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActCreate, ins.code,
                  "code:" + code);
        return Result<int64_t>::fail(ins.code, ins.msg);
    }
    const int64_t newId = ins.data;

    auto logWrite = writeStatusLog(conn, newId, std::string(), model::toStr(r.status), ctx,
                                   "创建资源");
    if (!logWrite.ok()) {
        tx.rollback();
        return Result<int64_t>::fail(logWrite.code, logWrite.msg);
    }

    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<int64_t>::fail(committed.code, committed.msg);
    }

    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActCreate, kOk, newId);
    return Result<int64_t>::make(newId);
}

Result<model::Resource> ResourceService::update(int64_t id, const model::ResourceUpdateReq& req,
                                                const AuthContext& ctx) {
    if (id <= 0) {
        return Result<model::Resource>::fail(kResourceNotFound, "资源不存在");
    }

    // ---- 提交字段的取值校验（半更新：只校验提交了的部分）
    if (req.name.has_value()) {
        const std::string v = Str::trim(*req.name);
        if (v.empty() || v.size() > kMaxNameLen) {
            LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActUpdate, kInvalidParam, id);
            return Result<model::Resource>::fail(kInvalidParam,
                                                 "参数校验失败：name 长度必须为 1..128");
        }
    }
    if (req.location.has_value() && req.location->size() > kMaxLocLen) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActUpdate, kInvalidParam, id);
        return Result<model::Resource>::fail(kInvalidParam, "参数校验失败：location 上限 128");
    }
    if (req.description.has_value() && req.description->size() > kMaxDescLen) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActUpdate, kInvalidParam, id);
        return Result<model::Resource>::fail(kInvalidParam, "参数校验失败：description 上限 512");
    }
    if (req.capacity.has_value() && (*req.capacity < 1 || *req.capacity > kMaxCapacity)) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActUpdate, kInvalidParam, id);
        return Result<model::Resource>::fail(kInvalidParam, "参数校验失败：capacity 必须为 1..100000");
    }
    if (req.depositAmount.has_value() && *req.depositAmount < 0.0) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActUpdate, kInvalidParam, id);
        return Result<model::Resource>::fail(kInvalidParam, "参数校验失败：depositAmount 不能为负数");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<model::Resource>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rr_.findById(conn, id);
    if (!cur.ok()) {
        return Result<model::Resource>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActUpdate, kResourceNotFound, id);
        return Result<model::Resource>::fail(kResourceNotFound, "资源不存在");
    }

    model::ResourceUpdateReq q = req;
    if (q.name.has_value()) {
        q.name = Str::trim(*q.name);   // 与 create 一致：名称去首尾空白
    }

    auto upd = rr_.update(conn, id, q);
    if (!upd.ok()) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActUpdate, upd.code, id);
        return Result<model::Resource>::fail(upd.code, upd.msg);
    }

    auto after = rr_.findById(conn, id);
    if (!after.ok()) {
        return Result<model::Resource>::fail(after.code, after.msg);
    }
    if (!after.data.has_value()) {
        return Result<model::Resource>::fail(kResourceNotFound, "资源不存在");
    }

    // 无状态变化 → 只写应用日志 AUDIT，不写 status_log（D-13）
    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActUpdate, kOk, id);
    return Result<model::Resource>::make(*after.data);
}

Result<Empty> ResourceService::remove(int64_t id, const AuthContext& ctx) {
    if (id <= 0) {
        return Result<Empty>::fail(kResourceNotFound, "资源不存在");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Empty>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    // 含已删除查询：用于区分「从未存在」(3000) 与「已删除」(幂等成功)
    auto any = rr_.findByIdAny(conn, id);
    if (!any.ok()) {
        return Result<Empty>::fail(any.code, any.msg);
    }
    if (!any.data.has_value()) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActDelete, kResourceNotFound, id);
        return Result<Empty>::fail(kResourceNotFound, "资源不存在");
    }

    const model::Resource res = *any.data;
    if (!res.deletedAt.empty()) {
        // 已逻辑删除：幂等成功，不再重复写 status_log（避免审计噪音）
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActDelete, kOk, id);
        Result<Empty> ok = Result<Empty>::make(Empty{});
        ok.msg = deleteOkMsg(res.code) + "（该资源此前已删除）";
        return ok;
    }

    // ---- D-4：存在有效预约则拒绝删除
    auto active = rr_.countActiveReservations(conn, id);
    if (!active.ok()) {
        return Result<Empty>::fail(active.code, active.msg);
    }
    if (active.data > 0) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActDelete,
                  kResourceHasActiveReservation, id);
        return Result<Empty>::fail(kResourceHasActiveReservation, "该资源存在有效预约，暂不能删除");
    }

    // ---- 事务：softDelete + status_log(原状态 → DELETED)
    Transaction tx(conn);
    if (!tx.active()) {
        return Result<Empty>::fail(kDbError, "数据库访问失败：事务开启失败");
    }

    auto del = rr_.softDelete(conn, id);
    if (!del.ok()) {
        tx.rollback();
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActDelete, del.code, id);
        return Result<Empty>::fail(del.code, del.msg);
    }

    auto logWrite = writeStatusLog(conn, id, model::toStr(res.status), "DELETED", ctx,
                                   "删除资源（逻辑删除）");
    if (!logWrite.ok()) {
        tx.rollback();
        return Result<Empty>::fail(logWrite.code, logWrite.msg);
    }

    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<Empty>::fail(committed.code, committed.msg);
    }

    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActDelete, kOk, id);
    Result<Empty> ok = Result<Empty>::make(Empty{});
    ok.msg = deleteOkMsg(res.code);
    return ok;
}

Result<model::Resource> ResourceService::changeStatus(int64_t id, const std::string& rawStatus,
                                                      const AuthContext& ctx) {
    // ---- 1) 字面量合法性：不是枚举成员 → 1000
    const std::optional<model::ResourceState> parsed =
        model::resourceStateFrom(Str::trim(rawStatus));
    if (!parsed.has_value()) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActStatus, kInvalidParam, id);
        return Result<model::Resource>::fail(kInvalidParam,
                                             "参数校验失败：status 取值非法（" + rawStatus + "）");
    }
    // ---- 2) 合法枚举但 P0 不允许手工写 → 3002
    if (!model::isP0WritableState(*parsed)) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActStatus,
                  kResourceStatusDenied, id);
        return Result<model::Resource>::fail(kResourceStatusDenied, "仅允许设置为 FREE 或 MAINTENANCE");
    }
    if (id <= 0) {
        return Result<model::Resource>::fail(kResourceNotFound, "资源不存在");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<model::Resource>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rr_.findById(conn, id);
    if (!cur.ok()) {
        return Result<model::Resource>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActStatus, kResourceNotFound, id);
        return Result<model::Resource>::fail(kResourceNotFound, "资源不存在");
    }

    const std::string from = model::toStr(cur.data->status);
    const std::string to   = model::toStr(*parsed);
    if (from == to) {
        // 无真实状态迁移：不写 status_log（D-13），只写 AUDIT，返回当前资源
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActStatus, kOk, id);
        return Result<model::Resource>::make(*cur.data);
    }

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<model::Resource>::fail(kDbError, "数据库访问失败：事务开启失败");
    }

    auto upd = rr_.updateStatus(conn, id, *parsed);
    if (!upd.ok()) {
        tx.rollback();
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActStatus, upd.code, id);
        return Result<model::Resource>::fail(upd.code, upd.msg);
    }

    auto logWrite = writeStatusLog(conn, id, from, to, ctx, "手工标记资源状态");
    if (!logWrite.ok()) {
        tx.rollback();
        return Result<model::Resource>::fail(logWrite.code, logWrite.msg);
    }

    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<model::Resource>::fail(committed.code, committed.msg);
    }

    auto after = rr_.findById(conn, id);
    if (!after.ok()) {
        return Result<model::Resource>::fail(after.code, after.msg);
    }
    if (!after.data.has_value()) {
        return Result<model::Resource>::fail(kResourceNotFound, "资源不存在");
    }

    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActStatus, kOk, id);
    return Result<model::Resource>::make(*after.data);
}

Result<Empty> ResourceService::writeStatusLog(DbConn& conn, int64_t resourceId,
                                              const std::string& from, const std::string& to,
                                              const AuthContext& ctx, const std::string& reason) {
    repo::StatusLogEntry e;
    e.businessType = "RESOURCE";
    e.businessId   = resourceId;
    e.fromStatus   = from;
    e.toStatus     = to;
    e.operatorId   = ctx.userId;
    e.reason       = reason;
    e.ruleCode     = std::nullopt;   // P0 全部为手工操作（D-12）

    auto r = ar_.insertStatusLog(conn, e);
    if (!r.ok()) {
        return Result<Empty>::fail(r.code, r.msg);
    }
    return Result<Empty>::make(Empty{});
}

} // namespace rf
