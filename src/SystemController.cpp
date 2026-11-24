// =============================================================================
// src/controller/SystemController.cpp  —  GET /health/deps 依赖连通性探针
// 对应 ARCH-P0 §3.11 / §5（#5）/ §6.3 / §8.7
// 语义：探针自身永不失败 —— 无论依赖是否可用，一律 HTTP 200 + code=0，
//       把被探对象的真实状态放进 data（mysql/redis 各自独立，互不影响）。
// =============================================================================
#include "Controllers.h"

#include <string>

#include "Base.h"

namespace rf {

namespace {

/**
 * 组装单个依赖的探测结果。
 * @param up        探测是否成功
 * @param latencyMs 成功耗时（毫秒）；失败为 -1
 * @param err       失败原因摘要
 */
Json depJson(bool up, int latencyMs, const std::string& err) {
    Json j;
    j["status"]    = up ? "up" : "down";
    j["latencyMs"] = latencyMs;
    if (!up && !err.empty()) {
        j["error"] = err;
    }
    return j;
}

}  // namespace

SystemController::SystemController(DbPool& pool, RedisClient& redis, repo::UserRepo& userRepo)
    : pool_(pool), redis_(redis), userRepo_(userRepo) {}

Result<Json> SystemController::healthDeps(const httplib::Request& /*req*/) {
    // 两路探测互相独立：任一路失败只体现为自身的 status=down，不影响另一路，
    // 也不影响接口本身的 code（§6.3）。
    int         myLatency = -1;
    std::string myErr;
    const bool  mysqlUp = pool_.ping(&myLatency, myErr);
    if (!mysqlUp) {
        LOG_WARN("health/deps：MySQL down（{}）", myErr);
    }

    int         rdLatency = -1;
    std::string rdErr;
    const bool  redisUp = redis_.ping(&rdLatency, rdErr);
    if (!redisUp) {
        LOG_WARN("health/deps：Redis down（{}）", rdErr);
    }

    Json data;
    data["mysql"] = depJson(mysqlUp, myLatency, myErr);
    data["redis"] = depJson(redisUp, rdLatency, rdErr);
    return Result<Json>::make(std::move(data));
}

// ============================================================================
// P4 T00 新增（只读）：用户列表
// 之所以放在 SystemController 而不是新开一个 controller：前端把它归在 system.js 域，
// 且它只有一个方法，单独建文件反而增加装配成本。
// ============================================================================
Result<Json> SystemController::users(const httplib::Request& req, const AuthContext& ctx) {
    if (ctx.role != model::Role::ADMIN) {
        return Result<Json>::fail(kForbidden, "权限不足，需要管理员角色");
    }
    const int  page = detail::queryInt(req, "page", 1);
    const int  size = detail::queryInt(req, "size", 10);
    const auto statusOpt  = detail::queryStr(req, "status");
    const auto keywordOpt = detail::queryStr(req, "keyword");
    const std::string status  = statusOpt.value_or(std::string());
    const std::string keyword = Str::trim(keywordOpt.value_or(std::string()));

    if (!status.empty() && !model::userStatusFrom(status).has_value()) {
        return Result<Json>::fail(kInvalidParam,
                                  "参数校验失败：status 取值非法（" + status + "）");
    }
    if (keyword.size() > 64) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：keyword 长度上限 64");
    }
    if (page < 1 || size < 1 || size > 100) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：page/size 超出允许范围");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto r = userRepo_.list(*guard, page, size, status, keyword);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    Json list = Json::array();
    for (const auto& u : r.data.list) {
        list.push_back(u.toJson());   // toJson 不输出 passwordHash
    }
    Json data;
    data["list"]  = std::move(list);
    data["total"] = r.data.total;
    data["page"]  = r.data.page;
    data["size"]  = r.data.size;
    return Result<Json>::make(std::move(data));
}

// ============================================================================
// P6 新增：账号审核（ARCH-P6 §3.2）
//   POST /admin/users/{id}/approve → PENDING → NORMAL
//   POST /admin/users/{id}/reject  → PENDING → REJECTED
// 两条都是 CAS：`UPDATE ... WHERE id=? AND status='PENDING'`，
// 非 PENDING 返回 2007（并发重复审核也不会把已生效账号改坏）。
// ============================================================================
Result<Json> SystemController::reviewUser(const httplib::Request& req, const AuthContext& ctx,
                                          bool approve) {
    if (ctx.role != model::Role::ADMIN) {
        return Result<Json>::fail(kForbidden, "权限不足，需要管理员角色");
    }
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }

    // 拒绝理由可选，**只进日志不落库**（user 表没有 review_remark 列，D-P6-4）
    std::string reason;
    if (!req.body.empty()) {
        Json         body;
        Result<Json> parseErr;
        if (detail::parseJsonBody(req, body, parseErr) && body.is_object()) {
            reason = Str::trim(JsonUtil::getStr(body, "reason").value_or(std::string()));
        }
        // 请求体不是合法 JSON 时**不阻断**：理由是可选的，缺了照样能审核
    }
    if (reason.size() > 255) {
        reason = reason.substr(0, 255);
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    // 先确认账号存在：不存在沿用 1004（既有的"目标不存在/无权"口径，不新造码）
    auto found = userRepo_.findById(conn, *id);
    if (!found.ok()) {
        return Result<Json>::fail(found.code, found.msg);
    }
    if (!found.data.has_value()) {
        return Result<Json>::fail(kForbidden, "账号不存在");
    }
    // 已经是目标状态 → 幂等成功（重复点"通过"不该报错）
    const model::UserStatus want =
        approve ? model::UserStatus::NORMAL : model::UserStatus::REJECTED;
    if (found.data->status == want) {
        Json data;
        data["id"]          = found.data->id;
        data["username"]    = found.data->username;
        data["status"]      = model::toStr(found.data->status);
        data["statusLabel"] = model::labelOf(found.data->status);
        return Result<Json>::make(std::move(data));
    }

    auto r = approve ? userRepo_.approve(conn, *id) : userRepo_.reject(conn, *id);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    if (!r.data) {
        // CAS 未命中：当前不是 PENDING（并发下被别人审过/本来就是别的状态）
        LOG_INFO("rid={} action={} uid={} code={} reason=当前状态不是 PENDING（status={}）",
                 requestId(), approve ? "USER_APPROVE" : "USER_REJECT", *id,
                 static_cast<int>(kUserReviewDenied), model::toStr(found.data->status));
        return Result<Json>::fail(kUserReviewDenied,
                                 "该账号当前状态不允许审核（只有待审核账号可以通过/拒绝）");
    }

    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role),
              approve ? "USER_APPROVE" : "USER_REJECT", kOk, *id);
    LOG_INFO("rid={} action={} uid={} username={} {}→{}{}{}", requestId(),
             approve ? "USER_APPROVE" : "USER_REJECT", *id, found.data->username,
             model::toStr(found.data->status), model::toStr(want),
             reason.empty() ? "" : " reason=", reason);

    auto after = userRepo_.findById(conn, *id);
    if (!after.ok() || !after.data.has_value()) {
        return Result<Json>::fail(kForbidden, "账号不存在");
    }
    Json data;
    data["id"]          = after.data->id;
    data["username"]    = after.data->username;
    data["status"]      = model::toStr(after.data->status);
    data["statusLabel"] = model::labelOf(after.data->status);
    return Result<Json>::make(std::move(data));
}

} // namespace rf
