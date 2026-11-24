// =============================================================================
// src/controller/AuthController.cpp  —  /auth/login|logout|me|password
// 对应 ARCH-P0 §3.11 / §5（#1..#4）/ §8.2
// =============================================================================
#include "Controllers.h"

#include <exception>
#include <string>

#include "AuthMiddleware.h"
#include "Base.h"

namespace rf {

namespace {

/** 口令长度约束（与 AuthService 保持一致，Controller 侧先做一次友好提示） */
constexpr size_t kPwdMinLen = 8;
constexpr size_t kPwdMaxLen = 64;

}  // namespace

AuthController::AuthController(AuthService& svc) : svc_(svc) {}

Result<Json> AuthController::login(const httplib::Request& req) {
    Json      body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }

    const auto username = JsonUtil::getStr(body, "username");
    const auto password = JsonUtil::getStr(body, "password");
    if (!username.has_value() || Str::trim(*username).empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：username 不能为空");
    }
    if (!password.has_value() || password->empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：password 不能为空");
    }

    auto r = svc_.login(*username, *password, requestId());
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }

    Json userJson         = r.data.user.toBriefJson();
    userJson["realName"]  = r.data.user.realName;
    userJson["status"]    = model::toStr(r.data.user.status);

    Json data;
    data["token"]     = r.data.token;
    data["tokenType"] = r.data.tokenType;
    data["expiresIn"] = r.data.expiresIn;
    data["expiresAt"] = r.data.expiresAt;
    data["user"]      = std::move(userJson);
    return Result<Json>::make(std::move(data));
}

// ------------------------------------------------------------------ 公开注册（P6）
Result<Json> AuthController::registerUser(const httplib::Request& req) {
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }

    // 只读这三项：role / creditScore / status **即使传了也忽略**（D-P6-7）
    const auto username = JsonUtil::getStr(body, "username");
    const auto realName = JsonUtil::getStr(body, "realName");
    const auto password = JsonUtil::getStr(body, "password");

    if (!username.has_value() || Str::trim(*username).empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：username 不能为空");
    }
    if (!realName.has_value() || Str::trim(*realName).empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：realName 不能为空");
    }
    if (!password.has_value() || password->empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：password 不能为空");
    }

    auto r = svc_.registerUser(*username, *realName, *password, requestId());
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }

    // 兼容两处口径：文档写 `id`，任务说明写 `userId` —— 两个都给，前端取哪个都行
    Json data;
    data["id"]          = r.data.user.id;
    data["userId"]      = r.data.user.id;
    data["username"]    = r.data.user.username;
    data["status"]      = model::toStr(r.data.user.status);
    data["statusLabel"] = model::labelOf(r.data.user.status);
    return Result<Json>::make(std::move(data));
}

Result<Json> AuthController::logout(const httplib::Request& req, const AuthContext& ctx) {
    const std::string token = AuthMiddleware::extractToken(req);
    if (token.empty()) {
        return Result<Json>::fail(kUnauthorized, "未提供有效登录令牌");
    }
    auto r = svc_.logout(token, ctx.expEpoch, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    Json data;
    data["loggedOut"] = true;
    return Result<Json>::make(std::move(data));
}

Result<Json> AuthController::me(const httplib::Request& /*req*/, const AuthContext& ctx) {
    auto r = svc_.me(ctx.userId);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    return Result<Json>::make(r.data.toJson());
}

Result<Json> AuthController::changePassword(const httplib::Request& req, const AuthContext& ctx) {
    Json         body;
    Result<Json> parseErr;
    if (!detail::parseJsonBody(req, body, parseErr)) {
        return parseErr;
    }

    const auto oldPwd = JsonUtil::getStr(body, "oldPassword");
    const auto newPwd = JsonUtil::getStr(body, "newPassword");
    if (!oldPwd.has_value() || oldPwd->empty()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：oldPassword 不能为空");
    }
    if (!newPwd.has_value()) {
        return Result<Json>::fail(kInvalidParam, "参数校验失败：newPassword 不能为空");
    }
    if (newPwd->size() < kPwdMinLen || newPwd->size() > kPwdMaxLen) {
        return Result<Json>::fail(kInvalidParam,
                                  "参数校验失败：newPassword 长度必须为 8..64 个字符");
    }

    auto r = svc_.changePassword(ctx.userId, *oldPwd, *newPwd, ctx);
    if (!r.ok()) {
        return Result<Json>::fail(r.code, r.msg);
    }
    Json data;
    data["changed"] = true;
    return Result<Json>::make(std::move(data));
}

} // namespace rf
