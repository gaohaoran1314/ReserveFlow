// =============================================================================
// src/app/AuthMiddleware.h  —  鉴权中间件 + 统一 HTTP 出口
// 对应 ARCH-P0 §3.12 / §1.2 / D-2 / §8.7
// 唯一职责：解析令牌 → 校验（签名/有效期/失效名单）→ 角色比较 → 调 handler →
//           把 Result<Json> 写成 {code,msg,data} 响应体（全进程唯一写响应处）。
// =============================================================================
#pragma once

#include <string>

#include "RedisClient.h"
#include "Result.h"
#include "Controllers.h"
#include "httplib.h"
#include "JwtUtil.h"

namespace rf {

/** 鉴权中间件 */
class AuthMiddleware {
public:
    AuthMiddleware(JwtUtil& jwt, RedisClient& redis);
    ~AuthMiddleware() = default;
    AuthMiddleware(const AuthMiddleware&)            = delete;
    AuthMiddleware& operator=(const AuthMiddleware&) = delete;

    /**
     * 包装受保护路由。
     * @param inner   业务 handler
     * @param minRole 最小角色要求（Role::ADMIN 表示仅管理员）
     */
    httplib::Server::Handler protect(const JsonHandler& inner,
                                     model::Role minRole = model::Role::USER);

    /** 包装公开路由：跳过鉴权，但仍统一写响应与异常兜底 */
    httplib::Server::Handler open(const OpenHandler& inner);

    /**
     * 单独暴露的令牌校验（便于自测与单测）。
     * @return code=0 且 data 为鉴权上下文；失败时 code ∈ {1001,1002,1003}
     */
    Result<AuthContext> authenticate(const httplib::Request& req);

    /**
     * 从 Authorization 头取原始令牌（不含 "Bearer "），缺失或格式错返回空串。
     * logout 需要原始令牌来算失效名单 key，故公开此工具。
     */
    static std::string extractToken(const httplib::Request& req);

    /** 统一响应出口：状态码 = httpStatusOf(code)，响应体 = {code,msg,data} */
    static void writeResult(httplib::Response& res, const Result<nlohmann::json>& result);

private:
    JwtUtil&      jwt_;
    RedisClient&  redis_;
};

} // namespace rf
