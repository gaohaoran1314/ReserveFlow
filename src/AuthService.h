// =============================================================================
// src/service/AuthService.h  —  账号服务（登录 / 当前用户 / 登出 / 改密）
// 对应 ARCH-P0 §3.10 / D-1 / D-2 / §8.7
// 职责边界：业务规则 + 事务边界 + 审计日志；不拼 SQL（交给 Repo），不引用 httplib。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "RedisClient.h"
#include "Result.h"
#include "DbPool.h"
#include "Model.h"
#include "UserRepo.h"
#include "JwtUtil.h"

namespace rf {

/**
 * 鉴权上下文：由中间件产出，服务层只读。
 * tokenHash = sha256Hex(原始令牌)，登出与审计用（不落令牌全文，§8.6）。
 */
struct AuthContext {
    int64_t     userId   = 0;
    std::string username;
    model::Role role     = model::Role::USER;
    std::string tokenHash;
    int64_t     expEpoch = 0;
};

/** 登录成功载荷 */
struct LoginData {
    std::string token;
    std::string tokenType = "Bearer";
    int64_t     expiresIn = 28800;
    std::string expiresAt;          ///< ISO 字符串
    model::User user;
};

/** 注册成功载荷：**不含 token**（PENDING 登不进去，发 token 反而是错的） */
struct RegisterData {
    model::User user;   ///< 新建或重新提交后的账号（status 必为 PENDING）
};

/** 账号服务 */
class AuthService {
public:
    AuthService(DbPool& pool, RedisClient& redis, JwtUtil& jwt);
    ~AuthService() = default;
    AuthService(const AuthService&)            = delete;
    AuthService& operator=(const AuthService&) = delete;

    /**
     * 账号密码登录。
     * 用户不存在时同样执行一次等价耗时的 PBKDF2 校验（防用户名枚举），
     * 错误一律返回 2000「账号或密码错误」，不区分账号是否存在。
     * 黑名单账号允许登录（只拦截预约，P0 无预约功能）。
     * @param rid 请求关联 ID（中间件生成），仅用于审计日志
     */
    Result<LoginData> login(const std::string& username, const std::string& password,
                            const std::string& rid = std::string());

    /**
     * 学生自助注册（ARCH-P6 §3.1 / D-P6-1/5/7）。
     *
     * 状态落库为 **PENDING**（必须管理员审核通过才能登录），角色固定 USER、信用分固定 100 ——
     * **客户端传的 role / creditScore / status 一律不读**（本方法根本没有这些入参）。
     *
     * 学号已存在时的分支：
     *   · 状态 REJECTED → 走 CAS 更新（姓名/口令覆盖，状态回 PENDING），复用原 id
     *   · 状态 PENDING  → 2004「已提交，等待审核」
     *   · 其它状态      → 2003「该学号已被注册」
     *
     * @param rid 请求关联 ID，仅用于审计日志
     * @return 成功时 data.user.status == PENDING；**不返回 token**
     */
    Result<RegisterData> registerUser(const std::string& username, const std::string& realName,
                                      const std::string& password,
                                      const std::string& rid = std::string());

    /** 当前登录用户信息（不返回 passwordHash） */
    Result<model::User> me(int64_t userId);

    /**
     * 登出：把令牌哈希写入 Redis 失效名单，TTL = 令牌剩余有效期（D-2）。
     * Redis 不可用时返回 1010（写路径不假装成功，§8.7）。
     * @param rawToken 原始令牌（不含 Bearer 前缀）
     * @param expEpoch 令牌过期时刻（epoch 秒）
     * @param ctx      鉴权上下文（审计日志需要 op / role）
     */
    Result<Empty> logout(const std::string& rawToken, int64_t expEpoch, const AuthContext& ctx);

    /**
     * 修改本人密码。
     * @param userId 目标账号
     * @param oldPwd 原密码（错 → 2001）
     * @param newPwd 新密码（8..64 且不得与原密码相同）
     * @param ctx    鉴权上下文（审计日志需要 op / role）
     */
    Result<Empty> changePassword(int64_t userId, const std::string& oldPwd,
                                 const std::string& newPwd, const AuthContext& ctx);

private:
    DbPool&        pool_;
    RedisClient&   redis_;
    JwtUtil&       jwt_;
    repo::UserRepo userRepo_;
};

} // namespace rf
