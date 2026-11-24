// =============================================================================
// src/app/AuthMiddleware.cpp  —  鉴权中间件与统一响应出口实现
// 对应 ARCH-P0 §1.2 / §3.12 / §8.5 / §8.6 / §8.7
// 关键约定：
//   1) 无令牌 / 格式错 / 签名错 → 401 + 1001；过期 → 401 + 1002；命中失效名单 → 401 + 1003；
//   2) 角色不足 → 403 + 1004；
//   3) 业务失败一律 HTTP 200 + code != 0（D-6，由 httpStatusOf 统一决定）；
//   4) 未捕获异常 → 500 + 1008，进程存活；
//   5) Redis 不可用时失效名单检查 fail-open（WARN 日志后放行，§8.7）。
// =============================================================================
#include "AuthMiddleware.h"

#include <exception>
#include <string>

#include "Base.h"

namespace rf {

namespace {

/** 角色强弱次序：USER(0) < ADMIN(1) */
int roleRank(model::Role r) { return r == model::Role::ADMIN ? 1 : 0; }

/** 响应体序列化：ensure_ascii=false（中文不转义），非法 UTF-8 用替换字符兜底 */
std::string dumpJson(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace

AuthMiddleware::AuthMiddleware(JwtUtil& jwt, RedisClient& redis) : jwt_(jwt), redis_(redis) {}

std::string AuthMiddleware::extractToken(const httplib::Request& req) {
    if (!req.has_header("Authorization")) {
        return std::string();
    }
    const std::string raw = Str::trim(req.get_header_value("Authorization"));
    if (raw.size() < 8) {
        return std::string();
    }
    const std::string prefix = Str::toLower(raw.substr(0, 7));   // "bearer "
    if (prefix != "bearer ") {
        return std::string();
    }
    return Str::trim(raw.substr(7));
}

Result<AuthContext> AuthMiddleware::authenticate(const httplib::Request& req) {
    const std::string token = extractToken(req);
    if (token.empty()) {
        return Result<AuthContext>::fail(kUnauthorized, "未提供有效登录令牌");
    }

    JwtPayload  payload;
    std::string err;
    const auto  st = jwt_.verify(token, payload, &err);
    switch (st) {
        case JwtUtil::VerifyStatus::OK:
            break;
        case JwtUtil::VerifyStatus::EXPIRED:
            return Result<AuthContext>::fail(kTokenExpired, "登录令牌已过期，请重新登录");
        case JwtUtil::VerifyStatus::MALFORMED:
            return Result<AuthContext>::fail(kUnauthorized, "未提供有效登录令牌");
        case JwtUtil::VerifyStatus::BAD_SIGNATURE:
        default:
            return Result<AuthContext>::fail(kUnauthorized, "未提供有效登录令牌");
    }

    // ---- 失效名单（D-2）：Redis 不可用时 fail-open 放行（§8.7）
    const std::string key = JwtUtil::blacklistKey(token);
    auto revoked = redis_.exists(key);
    if (!revoked.ok()) {
        LOG_WARN("rid={} redis blacklist check failed, fail-open: {}", requestId(), revoked.msg);
    } else if (revoked.data) {
        return Result<AuthContext>::fail(kTokenRevoked, "登录令牌已失效，请重新登录");
    }

    AuthContext ctx;
    ctx.userId    = payload.uid;
    ctx.username  = payload.uname;
    ctx.role      = payload.role;
    ctx.tokenHash = JwtUtil::sha256Hex(token);
    ctx.expEpoch  = payload.exp;
    return Result<AuthContext>::make(std::move(ctx));
}

void AuthMiddleware::writeResult(httplib::Response& res, const Result<nlohmann::json>& result) {
    nlohmann::json body = nlohmann::json::object();
    body["code"] = result.code;
    body["msg"]  = result.msg;
    // 失败与无数据成功都给 {}，绝不返回 null（§8.2）
    if (result.ok() && !result.data.is_null()) {
        body["data"] = result.data;
    } else {
        body["data"] = nlohmann::json::object();
    }

    res.status = httpStatusOf(result.code);
    res.set_content(dumpJson(body), "application/json; charset=utf-8");
}

httplib::Server::Handler AuthMiddleware::protect(const JsonHandler& inner, model::Role minRole) {
    // 捕获 jwt_/redis_ 的引用与 minRole；inner 按值复制，避免调用方生命周期问题
    return [this, inner, minRole](const httplib::Request& req, httplib::Response& res) {
        const std::string rid = Str::randomHex(8);   // 16 hex
        setRequestId(rid);
        res.set_header("X-Request-Id", rid);

        try {
            auto ctx = authenticate(req);
            if (!ctx.ok()) {
                LOG_DEBUG("rid={} action=AUTH_DENY code={} path={}", rid, ctx.code, req.path);
                writeResult(res, Result<nlohmann::json>::fail(ctx.code, ctx.msg));
                return;
            }
            if (roleRank(ctx.data.role) < roleRank(minRole)) {
                LOG_WARN("rid={} action=AUTH_FORBIDDEN uid={} role={} path={}", rid, ctx.data.userId,
                         model::toStr(ctx.data.role), req.path);
                writeResult(res, Result<nlohmann::json>::fail(kForbidden, "权限不足，需要管理员角色"));
                return;
            }
            writeResult(res, inner(req, ctx.data));
        } catch (const std::exception& e) {
            LOG_ERROR("rid={} action=UNCAUGHT_EXCEPTION path={} err={}", rid, req.path, e.what());
            writeResult(res, Result<nlohmann::json>::fail(kInternalError, "服务内部错误"));
        } catch (...) {
            LOG_ERROR("rid={} action=UNCAUGHT_EXCEPTION path={} err=未知类型", rid, req.path);
            writeResult(res, Result<nlohmann::json>::fail(kInternalError, "服务内部错误"));
        }
    };
}

httplib::Server::Handler AuthMiddleware::open(const OpenHandler& inner) {
    return [inner](const httplib::Request& req, httplib::Response& res) {
        const std::string rid = Str::randomHex(8);
        setRequestId(rid);
        res.set_header("X-Request-Id", rid);

        try {
            writeResult(res, inner(req));
        } catch (const std::exception& e) {
            LOG_ERROR("rid={} action=UNCAUGHT_EXCEPTION path={} err={}", rid, req.path, e.what());
            writeResult(res, Result<nlohmann::json>::fail(kInternalError, "服务内部错误"));
        } catch (...) {
            LOG_ERROR("rid={} action=UNCAUGHT_EXCEPTION path={} err=未知类型", rid, req.path);
            writeResult(res, Result<nlohmann::json>::fail(kInternalError, "服务内部错误"));
        }
    };
}

} // namespace rf
