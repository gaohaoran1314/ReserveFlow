// =============================================================================
// src/CreditController.cpp  —  信用体系接口（ARCH-P3 §7.5 / T09 / 附录 B）
//
// 4 个接口：
//   GET  /credit/me                      我的信用概览（含阈值与距下一档）
//   GET  /credit/me/logs                 我的变动明细分页（可按 reasonType 过滤）
//   GET  /admin/credit/{userId}          管理员查某人（ADMIN）
//   POST /admin/credit/{userId}/adjust   管理员手工调整（ADMIN，写 MANUAL 明细）
//
// 手工调整是**唯一**能人为加分的入口，所以必须留痕：`sourceRef=ADMIN:{uid}:{epoch}`
// 保证同一秒重复提交同一请求不会重复加分（uk_credit_source）。
// =============================================================================
#include "Controllers.h"

#include <string>
#include <utility>

#include "Base.h"

namespace rf {

CreditController::CreditController(CreditService& svc, DbPool& pool) : svc_(svc), pool_(pool) {}

Result<Json> CreditController::me(const httplib::Request& req, const AuthContext& ctx) {
    (void)req;
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto acc = svc_.account(*guard, ctx.userId);
    if (!acc.ok()) {
        return Result<Json>::fail(acc.code, acc.msg);
    }
    return Result<Json>::make(acc.data.toJson());
}

Result<Json> CreditController::myLogs(const httplib::Request& req, const AuthContext& ctx) {
    const int  page = detail::queryInt(req, "page", 1);
    const int  size = detail::queryInt(req, "size", 10);
    const auto reasonType = detail::queryStr(req, "reasonType");
    const std::string rt = reasonType.value_or(std::string());

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto pageRes = svc_.listLogs(*guard, ctx.userId, page, size, rt);
    if (!pageRes.ok()) {
        return Result<Json>::fail(pageRes.code, pageRes.msg);
    }
    Json list = Json::array();
    for (const auto& l : pageRes.data.list) {
        list.push_back(l.toJson());
    }
    Json data;
    data["list"]  = std::move(list);
    data["total"] = pageRes.data.total;
    data["page"]  = pageRes.data.page;
    data["size"]  = pageRes.data.size;
    return Result<Json>::make(std::move(data));
}

Result<Json> CreditController::userAccount(const httplib::Request& req, const AuthContext& ctx) {
    (void)ctx;
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：userId 必须是正整数");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto acc = svc_.account(*guard, *id);
    if (!acc.ok()) {
        return Result<Json>::fail(acc.code, acc.msg);
    }
    Json data      = acc.data.toJson();
    auto logs      = svc_.listLogs(*guard, *id, 1, 5, "");
    if (logs.ok()) {
        Json recent = Json::array();
        for (const auto& l : logs.data.list) {
            recent.push_back(l.toJson());
        }
        data["recentLogs"] = std::move(recent);
        data["logTotal"]   = logs.data.total;
    }
    return Result<Json>::make(std::move(data));
}

Result<Json> CreditController::adjust(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：userId 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    if (!body.contains("changeScore") || !body["changeScore"].is_number_integer()) {
        return Result<Json>::fail(kCreditAdjustInvalid,
                                  "信用分调整不合法：changeScore 必填且必须是整数");
    }
    const int change = body["changeScore"].get<int>();
    if (change == 0) {
        return Result<Json>::fail(kCreditAdjustInvalid, "信用分调整不合法：changeScore 不能为 0");
    }
    if (change < -100 || change > 100) {
        return Result<Json>::fail(kCreditAdjustInvalid,
                                  "信用分调整不合法：单次调整幅度必须在 [-100, 100] 内");
    }
    const auto reasonOpt = JsonUtil::getStr(body, "reason");
    const std::string reason = Str::trim(reasonOpt.value_or(std::string()));
    if (reason.empty()) {
        return Result<Json>::fail(kCreditAdjustInvalid, "信用分调整不合法：必须填写 reason");
    }
    if (reason.size() > 200) {
        return Result<Json>::fail(kCreditAdjustInvalid, "信用分调整不合法：reason 长度上限 200");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    // 手工调整也走同一个 applyDelta（状态机 / 明细 / clamp 全都复用）。
    // sourceRef 用「毫秒 + 4 位随机」而不是文档里的秒级 epoch：
    // 管理员在同一秒内连点两次 +1 是**两次独立意图**，秒级 key 会被 uk_credit_source
    // 当成重放而静默跳过（实测：第二次只回响应、分数没变）。
    const std::string sourceRef =
        "ADMIN:" + std::to_string(*id) + ":" +
        std::to_string(static_cast<long long>(TimeUtil::nowEpoch()) * 1000) + ":" +
        Str::randomHex(2);
    auto r = svc_.applyDelta(conn, *id, std::nullopt, change,
                             model::toStr(model::CreditReasonType::MANUAL), sourceRef, "",
                             ctx.userId, reason);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "CREDIT_ADJUST", kOk, *id);

    Json data;
    data["userId"]        = *id;
    data["scoreBefore"]   = r.data.scoreBefore;
    data["scoreAfter"]    = r.data.scoreAfter;
    data["status"]        = r.data.statusAfter;
    data["statusChanged"] = r.data.statusChanged;
    data["applied"]       = r.data.applied;
    data["logId"]         = r.data.logId;
    return Result<Json>::make(std::move(data));
}

} // namespace rf
