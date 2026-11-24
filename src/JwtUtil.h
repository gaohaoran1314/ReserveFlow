// =============================================================================
// src/security/JwtUtil.h  —  自研 HS256 JWT 签发与校验
// 对应 ARCH-P0 §3.7 / D-1 / D-2
// 设计约束：无状态纯函数（除持有配置副本），CNG 每次调用新建 handle，可并发调用
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "Config.h"
#include "Model.h"

namespace rf {

/** 令牌载荷（只放定位身份所需的最小字段，不放敏感信息） */
struct JwtPayload {
    int64_t     uid   = 0;                        ///< user.id
    std::string uname;                            ///< username
    model::Role role  = model::Role::USER;        ///< 角色
    int64_t     iat   = 0;                        ///< 签发时间 epoch 秒
    int64_t     exp   = 0;                        ///< 过期时间 epoch 秒
    std::string jti;                              ///< 随机 16 hex，仅用于日志串联
};

/**
 * HS256 JWT 工具。
 * 令牌结构：base64url(header).base64url(payload).base64url(HMAC-SHA256(signingInput))
 */
class JwtUtil {
public:
    /** 校验结果。顺序语义：结构 → 签名 → 有效期 */
    enum class VerifyStatus { OK, MALFORMED, BAD_SIGNATURE, EXPIRED };

    explicit JwtUtil(const JwtConfig& cfg) : cfg_(cfg) {}

    /**
     * 签发令牌。
     * @param p 载荷（iat/exp 由调用方填，或留 0 由本函数按 cfg_.ttlSeconds 补齐）
     * @return "header.payload.signature"；失败返回空串
     */
    std::string sign(const JwtPayload& p) const;

    /**
     * 校验令牌。
     * @param token 原始令牌（不含 "Bearer " 前缀）
     * @param out   校验通过时写入载荷
     * @param err   可选，失败时写入可读原因
     * @return VerifyStatus
     */
    VerifyStatus verify(const std::string& token, JwtPayload& out,
                        std::string* err = nullptr) const;

    /** sha256 → 64 位小写 hex（复用 crypto::sha256Hex） */
    static std::string sha256Hex(const std::string& s);

    /** 失效名单 key：rf:jwt:blacklist:<sha256hex(token)>（D-2 / §8.9） */
    static std::string blacklistKey(const std::string& token);

    /** 令牌有效期（秒） */
    int64_t ttlSeconds() const { return cfg_.ttlSeconds; }

    /** 签发者标识 */
    const std::string& issuer() const { return cfg_.issuer; }

private:
    JwtConfig cfg_;
};

} // namespace rf
