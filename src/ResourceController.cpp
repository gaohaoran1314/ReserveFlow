// =============================================================================
// src/controller/ResourceController.cpp  —  /resources 全部 7 个接口
// 对应 ARCH-P0 §3.11 / §5（#6..#12）/ §8.2 / §8.4
// =============================================================================
#include "Controllers.h"

#include <string>
#include <utility>

#include "Base.h"

namespace rf {

namespace {

/** 字段长度上限（与 Service / DDL 对齐，Controller 侧先给友好提示） */
constexpr size_t kMaxCodeLen = 64;
constexpr size_t kMaxNameLen = 128;
constexpr size_t kMaxLocLen  = 128;
constexpr size_t kMaxDescLen = 512;

/** 取必填字符串字段；缺失 / 类型错 / 空白 都算参数错误 */
bool requireStr(const Json& body, const char* key, size_t maxLen, std::string& out,
                Result<Json>& err) {
    const auto v = JsonUtil::getStr(body, key);
    if (!v.has_value()) {
        err = Result<Json>::fail(kInvalidParam,
                                 std::string("参数校验失败：") + key + " 必填且必须是字符串");
        return false;
    }
    const std::string trimmed = Str::trim(*v);
    if (trimmed.empty()) {
        err = Result<Json>::fail(kInvalidParam, std::string("参数校验失败：") + key + " 不能为空");
        return false;
    }
    if (maxLen > 0 && trimmed.size() > maxLen) {
        err = Result<Json>::fail(kInvalidParam, std::string("参数校验失败：") + key + " 长度上限 " +
                                                   std::to_string(maxLen));
        return false;
    }
    out = trimmed;
    return true;
}

/** 分页结果 → 接口 JSON：{list,total,page,size} */
Json pageToJson(const model::PageResult& p) {
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

ResourceController::ResourceController(ResourceService& svc) : svc_(svc) {}

Result<Json> ResourceController::meta(const httplib::Request& /*req*/, const AuthContext& /*ctx*/) {
    auto r = svc_.meta();
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(std::move(r.data));
}

Result<Json> ResourceController::list(const httplib::Request& req, const AuthContext& /*ctx*/) {
    model::ResourceQuery q;
    q.page = detail::queryInt(req, "page", 1);
    q.size = detail::queryInt(req, "size", 10);

    if (const auto t = detail::queryStr(req, "type")) {
        const auto parsed = model::resourceTypeFrom(*t);
        if (!parsed.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：type 取值非法（" + *t + "）");
        }
        q.type = *parsed;
    }
    if (const auto s = detail::queryStr(req, "status")) {
        const auto parsed = model::resourceStateFrom(*s);
        if (!parsed.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：status 取值非法（" + *s + "）");
        }
        q.status = *parsed;
    }
    q.location = detail::queryStr(req, "location");
    q.keyword  = detail::queryStr(req, "keyword");

    auto r = svc_.list(q);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(pageToJson(r.data));
}

Result<Json> ResourceController::detail(const httplib::Request& req, const AuthContext& /*ctx*/) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    auto r = svc_.detail(*id);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

Result<Json> ResourceController::create(const httplib::Request& req, const AuthContext& ctx) {
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }

    model::ResourceCreateReq in;
    if (!requireStr(body, "code", kMaxCodeLen, in.code, parseErr)) {
        return parseErr;
    }
    if (!requireStr(body, "name", kMaxNameLen, in.name, parseErr)) {
        return parseErr;
    }
    const auto typeStr = JsonUtil::getStr(body, "type");
    if (!typeStr.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：type 必填且必须是字符串");
    }
    const auto parsedType = model::resourceTypeFrom(Str::trim(*typeStr));
    if (!parsedType.has_value()) {
        return Result<Json>::fail(kInvalidParam,
                                  "参数校验失败：type 必须是 LAB_EQUIPMENT / MEETING_ROOM / LOCKER");
    }
    in.type = *parsedType;

    if (const auto v = JsonUtil::getStr(body, "location")) {
        if (v->size() > kMaxLocLen) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：location 长度上限 128");
        }
        in.location = Str::trim(*v);
    }
    if (JsonUtil::has(body, "capacity")) {
        const auto v = JsonUtil::getI64(body, "capacity");
        if (!v.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：capacity 必须是整数");
        }
        in.capacity = static_cast<int>(*v);
    }
    if (JsonUtil::has(body, "depositAmount")) {
        const auto v = JsonUtil::getDbl(body, "depositAmount");
        if (!v.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：depositAmount 必须是数字");
        }
        in.depositAmount = *v;
    }
    if (const auto v = JsonUtil::getStr(body, "description")) {
        if (v->size() > kMaxDescLen) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：description 长度上限 512");
        }
        in.description = *v;
    }

    auto r = svc_.create(in, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    Json data;
    data["id"] = r.data;
    return Result<Json>::make(std::move(data));
}

Result<Json> ResourceController::update(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }

    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }

    model::ResourceUpdateReq in;
    // 半更新：只有「键存在」才写入 optional；键存在但类型错 → 1000
    if (JsonUtil::has(body, "name")) {
        std::string v;
        if (!requireStr(body, "name", kMaxNameLen, v, parseErr)) {
            return parseErr;
        }
        in.name = v;
    }
    if (JsonUtil::has(body, "location")) {
        const auto v = JsonUtil::getStr(body, "location");
        if (!v.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：location 必须是字符串");
        }
        if (v->size() > kMaxLocLen) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：location 长度上限 128");
        }
        in.location = Str::trim(*v);
    }
    if (JsonUtil::has(body, "description")) {
        const auto v = JsonUtil::getStr(body, "description");
        if (!v.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：description 必须是字符串");
        }
        if (v->size() > kMaxDescLen) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：description 长度上限 512");
        }
        in.description = *v;
    }
    if (JsonUtil::has(body, "capacity")) {
        const auto v = JsonUtil::getI64(body, "capacity");
        if (!v.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：capacity 必须是整数");
        }
        in.capacity = static_cast<int>(*v);
    }
    if (JsonUtil::has(body, "depositAmount")) {
        const auto v = JsonUtil::getDbl(body, "depositAmount");
        if (!v.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：depositAmount 必须是数字");
        }
        in.depositAmount = *v;
    }
    if (JsonUtil::has(body, "type")) {
        const auto v = JsonUtil::getStr(body, "type");
        if (!v.has_value()) {
            return Result<Json>::fail(kInvalidParam, "参数校验失败：type 必须是字符串");
        }
        const auto parsed = model::resourceTypeFrom(Str::trim(*v));
        if (!parsed.has_value()) {
            return Result<Json>::fail(
                kInvalidParam,
                "参数校验失败：type 必须是 LAB_EQUIPMENT / MEETING_ROOM / LOCKER");
        }
        in.type = *parsed;
    }

    auto r = svc_.update(*id, in, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

Result<Json> ResourceController::remove(const httplib::Request& req, const AuthContext& ctx) {
    const auto id = detail::pathId(req);
    if (!id.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：id 必须是正整数");
    }
    auto r = svc_.remove(*id, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    // 保留 Service 的成功提示（含「编号 XXX 将永久保留，不可再次使用」，D-10）
    Result<Json> ok = Result<Json>::make(Json::object());
    ok.msg = r.msg;
    return ok;
}

Result<Json> ResourceController::changeStatus(const httplib::Request& req,
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
    const auto status = JsonUtil::getStr(body, "status");
    if (!status.has_value() || Str::trim(*status).empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：status 必填且必须是字符串");
    }

    auto r = svc_.changeStatus(*id, *status, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

} // namespace rf
