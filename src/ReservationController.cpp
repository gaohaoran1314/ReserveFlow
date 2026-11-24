// =============================================================================
// src/ReservationController.cpp  —  预约 7 个接口（ARCH-P1 §3.8 / §3.11 / 附录 B）
// 分层约束：只做路径/查询/body 解析、字段校验与 JSON 组装；
//           不写 SQL、不碰 DbPool、不做业务判断（全在 ReservationService）。
// 错误码口径：
//   1000 缺字段 / 类型错 / 枚举字面量非法 / 路径 id 非法；
//   1005 body 不是合法 JSON；其余业务码由 Service 透传。
// =============================================================================
#include "Controllers.h"

#include <string>
#include <utility>

#include "Base.h"

namespace rf {

namespace {

constexpr size_t kMaxRemarkLen = 255;

/** 分页结果 → 轻量 JSON：{list,total,page,size}（列表不带槽位与 allowedActions） */
Json pageToJson(const repo::ReservationPage& p) {
    Json list = Json::array();
    for (const auto& item : p.list) {
        ReservationView v;
        v.id            = item.id;
        v.reservationNo = item.reservationNo;
        v.userId        = item.userId;
        v.username      = item.username;
        v.resourceId    = item.resourceId;
        v.resourceCode  = item.resourceCode;
        v.resourceName  = item.resourceName;
        v.resourceType  = item.resourceType;
        v.startTime     = item.startTime;
        v.endTime       = item.endTime;
        v.status        = item.status;
        v.statusLabel   = "未知";
        if (const auto st = model::reservationStatusFrom(item.status); st.has_value()) {
            v.statusLabel = model::labelOf(*st);
        }
        v.needReview = (item.resourceType == model::ResourceType::LAB_EQUIPMENT);
        v.createdAt  = item.createdAt;
        list.push_back(v.toListJson());
    }
    Json data;
    data["list"]  = std::move(list);
    data["total"] = p.total;
    data["page"]  = p.page;
    data["size"]  = p.size;
    return data;
}

/** 取可选字符串字段：键不存在 → nullopt；键存在但类型错 / 超长 → 1000 */
bool optionalStr(const Json& body, const char* key, size_t maxLen, std::optional<std::string>& out,
                 Result<Json>& err) {
    if (!JsonUtil::has(body, key)) {
        return true;
    }
    const auto v = JsonUtil::getStr(body, key);
    if (!v.has_value()) {
        err = Result<Json>::fail(kInvalidParam,
                                 std::string("参数校验失败：") + key + " 必须是字符串");
        return false;
    }
    if (maxLen > 0 && v->size() > maxLen) {
        err = Result<Json>::fail(
            kInvalidParam, std::string("参数校验失败：") + key + " 长度上限 " + std::to_string(maxLen));
        return false;
    }
    out = Str::trim(*v);
    return true;
}

}  // namespace

ReservationController::ReservationController(ReservationService& svc) : svc_(svc) {}

// ------------------------------------------------------------------ POST /reservations
Result<Json> ReservationController::create(const httplib::Request& req, const AuthContext& ctx) {
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }

    // resourceId：必须是正整数
    const auto rid = JsonUtil::getI64(body, "resourceId");
    if (!rid.has_value() || *rid <= 0) {
        return Result<Json>::fail(kInvalidParam,
                                  "参数校验失败：resourceId 必填且必须是正整数");
    }
    // startTime / endTime：必填字符串
    const auto start = JsonUtil::getStr(body, "startTime");
    if (!start.has_value() || Str::trim(*start).empty()) {
        return Result<Json>::fail(kInvalidParam,
                                  "参数校验失败：startTime 必填且必须是字符串");
    }
    const auto end = JsonUtil::getStr(body, "endTime");
    if (!end.has_value() || Str::trim(*end).empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：endTime 必填且必须是字符串");
    }

    CreateReservationReq in;
    in.resourceId = *rid;
    in.startTime  = Str::trim(*start);
    in.endTime    = Str::trim(*end);

    std::optional<std::string> remark;
    if (!optionalStr(body, "remark", kMaxRemarkLen, remark, parseErr)) {
        return parseErr;
    }
    if (remark.has_value()) {
        in.remark = *remark;
    }

    auto r = svc_.create(in, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

// ------------------------------------------------------------------ GET /reservations
Result<Json> ReservationController::list(const httplib::Request& req, const AuthContext& ctx) {
    repo::ReservationQuery q;
    q.page = detail::queryInt(req, "page", 1);
    q.size = detail::queryInt(req, "size", 10);

    if (const auto s = detail::queryStr(req, "status")) {
        if (!model::reservationStatusFrom(*s).has_value()) {
            return Result<Json>::fail(kInvalidParam,
                                      "参数校验失败：status 取值非法（" + *s + "）");
        }
        q.status = *s;
    }
    if (req.has_param("resourceId")) {
        const int64_t v = static_cast<int64_t>(detail::queryInt(req, "resourceId", 0));
        if (v <= 0) {
            return Result<Json>::fail(kInvalidParam,
                                      "参数校验失败：resourceId 必须是正整数");
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

    // P3 T10：列表要带 resourceStatus + deposit，交给 service 组装（押金一次批量聚合）
    auto r = svc_.listJson(q, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(std::move(r.data));
}

// ------------------------------------------------------------------ GET /reservations/{id}
Result<Json> ReservationController::detail(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    auto r = svc_.detail(*id, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

// ------------------------------------------------------------------ POST /reservations/{id}/cancel
Result<Json> ReservationController::cancel(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    std::optional<std::string> reason;
    if (!optionalStr(body, "reason", kMaxRemarkLen, reason, parseErr)) {
        return parseErr;
    }

    auto r = svc_.cancel(*id, reason.value_or(std::string()), ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    Result<Json> ok = Result<Json>::make(Json::object());
    ok.msg = "取消成功";
    return ok;
}

// ------------------------------------------------------------------ POST /reservations/{id}/approve
Result<Json> ReservationController::approve(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    std::optional<std::string> remark;
    if (!optionalStr(body, "remark", kMaxRemarkLen, remark, parseErr)) {
        return parseErr;
    }

    auto r = svc_.approve(*id, remark.value_or(std::string()), ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

// ------------------------------------------------------------------ POST /reservations/{id}/reject
Result<Json> ReservationController::reject(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    std::optional<std::string> reason;
    if (!optionalStr(body, "reason", kMaxRemarkLen, reason, parseErr)) {
        return parseErr;
    }

    auto r = svc_.reject(*id, reason.value_or(std::string()), ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

// ------------------------------------------------------------------ GET /resources/{id}/slots
Result<Json> ReservationController::occupiedSlots(const httplib::Request& req,
                                                  const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    const auto date = detail::queryStr(req, "date");
    if (!date.has_value()) {
        return Result<Json>::fail(kInvalidParam,
                                  "参数校验失败：date 必填，格式为 yyyy-MM-dd");
    }
    auto r = svc_.occupiedSlots(*id, *date, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(std::move(r.data));
}

// ============================================================================
// P2 增量：取用 / 归还 / 验收 / 损坏 / 押金流水（ARCH-P2 §3.9 / 附录 B）
// ============================================================================

Result<Json> ReservationController::pickup(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    auto r = svc_.pickup(*id, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

Result<Json> ReservationController::returnBack(const httplib::Request& req,
                                              const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    std::optional<std::string> remark;
    if (!optionalStr(body, "remark", kMaxRemarkLen, remark, parseErr)) {
        return parseErr;
    }
    auto r = svc_.returnReservation(*id, remark.value_or(std::string()), ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

Result<Json> ReservationController::accept(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    std::optional<std::string> remark;
    if (!optionalStr(body, "remark", kMaxRemarkLen, remark, parseErr)) {
        return parseErr;
    }
    auto r = svc_.accept(*id, remark.value_or(std::string()), ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

Result<Json> ReservationController::damage(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }
    double deductAmount = 0.0;
    if (JsonUtil::has(body, "deductAmount")) {
        const auto v = JsonUtil::getDbl(body, "deductAmount");
        if (!v.has_value() || *v < 0.0) {
            return Result<Json>::fail(kInvalidParam,
                                      "参数校验失败：deductAmount 必须是非负数");
        }
        deductAmount = *v;
    }
    std::optional<std::string> reason;
    if (!optionalStr(body, "reason", kMaxRemarkLen, reason, parseErr)) {
        return parseErr;
    }
    auto r = svc_.damage(*id, deductAmount, reason.value_or(std::string()), ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

Result<Json> ReservationController::deposits(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    auto r = svc_.deposits(*id, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(std::move(r.data));
}

// ------------------------------------------------------------------ GET /reservations/{id}/timeline（P4 T00）
Result<Json> ReservationController::timeline(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    auto r = svc_.timeline(*id, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(std::move(r.data));
}

} // namespace rf
