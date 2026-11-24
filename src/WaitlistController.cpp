// =============================================================================
// src/WaitlistController.cpp  —  候补接口（ARCH-P2 §3.9）
// T05 范围：join（POST /resources/{id}/waitlist）、list（GET /waitlist）、
//          leave（DELETE /waitlist/{id}）。
// confirm / dispatchDebug 属 T08，本文件在 T08 追加（声明已在 Controllers.h）。
//
// 分层约束：只做路径/查询/body 解析与 JSON 组装；不写 SQL、不碰 DbPool。
// =============================================================================
#include "Controllers.h"

#include <string>
#include <utility>

#include "Base.h"

namespace rf {

namespace {

constexpr size_t kMaxRemarkLen = 255;

/** 候补分页 → 接口 JSON：{list,total,page,size} */
Json pageToJson(const repo::WaitlistPage& p) {
    Json list = Json::array();
    for (const auto& item : p.list) {
        list.push_back(item.toJson());
    }
    Json data;
    data["list"]  = std::move(list);
    data["total"] = p.total;
    data["page"]  = p.page;
    data["size"]  = p.size;
    return data;
}

}  // namespace

WaitlistController::WaitlistController(WaitlistService& svc) : svc_(svc) {}

// ------------------------------------------------------------------ POST /resources/{id}/waitlist
Result<Json> WaitlistController::join(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }

    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }

    const auto start = JsonUtil::getStr(body, "startTime");
    if (!start.has_value() || Str::trim(*start).empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：startTime 必填且必须是字符串");
    }
    const auto end = JsonUtil::getStr(body, "endTime");
    if (!end.has_value() || Str::trim(*end).empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：endTime 必填且必须是字符串");
    }

    JoinWaitlistReq in;
    in.resourceId = *id;
    in.startTime  = Str::trim(*start);
    in.endTime    = Str::trim(*end);
    if (JsonUtil::has(body, "remark")) {
        const auto v = JsonUtil::getStr(body, "remark");
        if (!v.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：remark 必须是字符串");
        }
        if (v->size() > kMaxRemarkLen) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：remark 长度上限 255");
        }
        in.remark = Str::trim(*v);
    }

    auto r = svc_.join(in, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

// ------------------------------------------------------------------ GET /waitlist
Result<Json> WaitlistController::list(const httplib::Request& req, const AuthContext& ctx) {
    repo::WaitlistQuery q;
    q.page = detail::queryInt(req, "page", 1);
    q.size = detail::queryInt(req, "size", 10);

    if (const auto s = detail::queryStr(req, "status")) {
        if (!model::waitlistStatusFrom(*s).has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：status 取值非法（" + *s + "）");
        }
        q.status = *s;
    }
    if (req.has_param("resourceId")) {
        const int64_t v = static_cast<int64_t>(detail::queryInt(req, "resourceId", 0));
        if (v <= 0) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：resourceId 必须是正整数");
        }
        q.resourceId = v;
    }
    if (req.has_param("userId")) {
        const int64_t v = static_cast<int64_t>(detail::queryInt(req, "userId", 0));
        if (v <= 0) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：userId 必须是正整数");
        }
        q.userId = v;
    }

    auto r = svc_.list(q, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(pageToJson(r.data));
}

// ------------------------------------------------------------------ DELETE /waitlist/{id}
Result<Json> WaitlistController::leave(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    auto r = svc_.leave(*id, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    Result<Json> ok = Result<Json>::make(Json::object());
    ok.msg = "已退出候补";
    return ok;
}

// ------------------------------------------------------------------ POST /waitlist/{id}/confirm
Result<Json> WaitlistController::confirm(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    auto r = svc_.confirm(*id, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    Json data   = r.data.toJson();
    data["reservationId"] = r.data.convertedReservationId;
    return Result<Json>::make(std::move(data));
}

// ------------------------------------------------------------------ POST /waitlist/dispatch（ADMIN）
Result<Json> WaitlistController::dispatchDebug(const httplib::Request& req,
                                              const AuthContext& ctx) {
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    const auto resourceId = JsonUtil::getI64(body, "resourceId");
    const auto date       = JsonUtil::getStr(body, "date");
    const auto startIndex = JsonUtil::getI64(body, "startIndex");
    const auto endIndex   = JsonUtil::getI64(body, "endIndex");
    if (!resourceId.has_value() || *resourceId <= 0) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：resourceId 必填且必须是正整数");
    }
    if (!date.has_value() || Str::trim(*date).empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：date 必填（yyyy-MM-dd）");
    }
    if (!startIndex.has_value() || !endIndex.has_value() || *startIndex < 0 || *endIndex > 48 ||
        *startIndex >= *endIndex) {
        return Result<Json>::fail(kInvalidParam,
                                  "参数校验失败：startIndex/endIndex 非法（0<=start<end<=48）");
    }

    // 该接口只用于验/排障：自己拿连接 + 事务，不对外暴露业务语义
    auto r = svc_.dispatchDebug(*resourceId, Str::trim(*date), static_cast<int>(*startIndex),
                                static_cast<int>(*endIndex), ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    Json data;
    data["dispatched"] = r.data;
    return Result<Json>::make(std::move(data));
}

} // namespace rf
