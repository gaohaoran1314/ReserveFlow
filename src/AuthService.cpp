// =============================================================================
// src/service/AuthService.cpp  —  账号服务实现
// 对应 ARCH-P0 §3.10 / §6.1 / §8.7
// 关键点：
//   1) 用户不存在也走一次 PBKDF2 校验，使耗时与「密码错」一致，抵御用户名枚举；
//   2) 黑名单（BLACKLIST）允许登录，只有 DISABLED 拦截登录；
//   3) 登出写 Redis 失效名单，Redis 故障返回 1010 而非假装成功。
// =============================================================================
#include "AuthService.h"

#include <ctime>
#include <string>

#include "Base.h"
#include "CryptoUtil.h"

namespace rf {

namespace {

/**
 * 诱饵哈希（一个真实的 pbkdf2_sha256 串）。
 * 账号不存在时对明文跑一次等价耗时的 verifyPassword：迭代轮数一致，
 * 因此「账号不存在」与「密码错误」的响应时间无法被区分。
 * 该串对应一个随机口令，任何用户密码都不会命中它。
 */
constexpr const char* kDecoyHash =
    "pbkdf2_sha256$120000$Nt5GvQOPkbeoIqp+wsc87w==$3dQLQSCH7d178uU8T5Q1G4KkOKqhYTckgb8kYnKJlPA=";

/** 口令长度约束（改密校验） */
constexpr size_t kPwdMinLen = 8;
constexpr size_t kPwdMaxLen = 64;

/** 注册时的学号约束（ARCH-P6 §3.1）：3–32 位字母/数字/下划线 */
constexpr size_t kUsernameMinLen = 3;
constexpr size_t kUsernameMaxLen = 32;
/** 姓名约束：trim 后 1–64 字符 */
constexpr size_t kRealNameMaxLen = 64;

/** 学号字符集校验：^[A-Za-z0-9_]{3,32}$ */
bool validUsernameCharset(const std::string& s) {
    if (s.size() < kUsernameMinLen || s.size() > kUsernameMaxLen) {
        return false;
    }
    for (const char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/** 组合出审计用的 rid：优先使用调用方显式传入的，其次线程内 rid */
std::string auditRid(const std::string& explicitRid) {
    return explicitRid.empty() ? requestId() : explicitRid;
}

}  // namespace

AuthService::AuthService(DbPool& pool, RedisClient& redis, JwtUtil& jwt)
    : pool_(pool), redis_(redis), jwt_(jwt) {}

Result<LoginData> AuthService::login(const std::string& username, const std::string& password,
                                     const std::string& rid) {
    const std::string user = Str::trim(username);
    if (user.empty()) {
        return Result<LoginData>::fail(kInvalidParam, "参数校验失败：username 不能为空");
    }
    if (password.empty()) {
        return Result<LoginData>::fail(kInvalidParam, "参数校验失败：password 不能为空");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<LoginData>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }

    auto found = userRepo_.findByUsername(*guard, user);
    if (!found.ok()) {
        return Result<LoginData>::fail(found.code, found.msg);
    }

    if (!found.data.has_value()) {
        // ---- 防用户名枚举：等价耗时的假校验（结果必然为 false，仅用于消耗同样的 CPU）
        const bool decoy = crypto::verifyPassword(password, kDecoyHash);
        (void)decoy;
        LOG_INFO("rid={} action=AUTH_LOGIN code={} username={} reason=账号不存在（已做等价耗时假校验）",
                 auditRid(rid), static_cast<int>(kInvalidCredential), user);
        return Result<LoginData>::fail(kInvalidCredential, "账号或密码错误");
    }

    const model::User u = *found.data;
    if (!crypto::verifyPassword(password, u.passwordHash)) {
        LOG_INFO("rid={} action=AUTH_LOGIN code={} uid={} reason=密码错误", auditRid(rid),
                 static_cast<int>(kInvalidCredential), u.id);
        return Result<LoginData>::fail(kInvalidCredential, "账号或密码错误");
    }

    // ---- 状态闸门（P6：从 1 个分支扩到 4 个；**位置不变 = 在密码校验之后**，D-P6-5）
    // 放在密码之后是为了保留反枚举性质：不知道密码的人无法区分
    // 「账号不存在 / 待审核 / 已拒绝 / 已禁用」。
    switch (u.status) {
        case model::UserStatus::DISABLED:
            LOG_INFO("rid={} action=AUTH_LOGIN code={} uid={} reason=账号已禁用", auditRid(rid),
                     static_cast<int>(kAccountDisabled), u.id);
            return Result<LoginData>::fail(kAccountDisabled, "账号已被禁用");
        case model::UserStatus::PENDING:
            LOG_INFO("rid={} action=AUTH_LOGIN code={} uid={} reason=账号待审核", auditRid(rid),
                     static_cast<int>(kAccountPending), u.id);
            return Result<LoginData>::fail(kAccountPending, "账号待审核，请等待管理员通过后登录");
        case model::UserStatus::REJECTED:
            LOG_INFO("rid={} action=AUTH_LOGIN code={} uid={} reason=注册审核未通过", auditRid(rid),
                     static_cast<int>(kAccountRejected), u.id);
            return Result<LoginData>::fail(kAccountRejected,
                                           "注册审核未通过，请修正信息后重新注册");
        case model::UserStatus::NORMAL:
        case model::UserStatus::BLACKLIST:
        default:
            break;   // 放行：黑名单只拦预约（P0 决策不变）
    }

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    JwtPayload    p;
    p.uid   = u.id;
    p.uname = u.username;
    p.role  = u.role;
    p.iat   = now;
    p.exp   = now + jwt_.ttlSeconds();
    p.jti   = Str::randomHex(8);

    const std::string token = jwt_.sign(p);
    if (token.empty()) {
        return Result<LoginData>::fail(kInternalError, "服务内部错误：登录令牌签发失败");
    }

    LoginData data;
    data.token     = token;
    data.tokenType = "Bearer";
    data.expiresIn = jwt_.ttlSeconds();
    data.expiresAt = TimeUtil::format(static_cast<std::time_t>(p.exp));
    data.user      = u;

    LOG_AUDIT(auditRid(rid), u.id, model::toStr(u.role), "AUTH_LOGIN", kOk, u.id);
    return Result<LoginData>::make(std::move(data));
}

Result<RegisterData> AuthService::registerUser(const std::string& username,
                                                const std::string& realName,
                                                const std::string& password,
                                                const std::string& rid) {
    // ---- ① 参数校验（客户端传的 role/creditScore/status 一概不读：本方法没有这些入参）
    const std::string name = Str::trim(username);
    const std::string real = Str::trim(realName);
    if (name.empty()) {
        return Result<RegisterData>::fail(kInvalidParam, "参数校验失败：username 不能为空");
    }
    if (!validUsernameCharset(name)) {
        return Result<RegisterData>::fail(
            kInvalidParam, "参数校验失败：username 必须为 3–32 位字母、数字或下划线");
    }
    if (real.empty()) {
        return Result<RegisterData>::fail(kInvalidParam, "参数校验失败：realName 不能为空");
    }
    if (real.size() > kRealNameMaxLen) {
        return Result<RegisterData>::fail(kInvalidParam,
                                          "参数校验失败：realName 长度不能超过 64 个字符");
    }
    if (password.size() < kPwdMinLen || password.size() > kPwdMaxLen) {
        return Result<RegisterData>::fail(kInvalidParam,
                                          "参数校验失败：password 长度必须为 8..64 个字符");
    }

    // ---- ② 口令哈希（失败即内部错误，不落库）
    const std::string hash = crypto::hashPassword(password);
    if (hash.empty()) {
        return Result<RegisterData>::fail(kInternalError, "服务内部错误：口令哈希生成失败");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<RegisterData>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    // ---- ③ 直接插：唯一性由 uk_user_username 兜底（**不做"先查后插"**，并发下那种写法必漏）
    repo::UserRepo::NewUser nu;
    nu.username     = name;
    nu.passwordHash = hash;
    nu.realName     = real;
    nu.role         = model::Role::USER;          // 固定普通用户
    nu.creditScore  = 100;                        // 固定初始信用分
    nu.status       = model::UserStatus::PENDING; // 必须审核通过才能登录

    auto created = userRepo_.create(conn, nu);
    if (created.ok()) {
        auto fresh = userRepo_.findById(conn, created.data);
        RegisterData out;
        if (fresh.ok() && fresh.data.has_value()) {
            out.user = *fresh.data;
        } else {
            out.user.id       = created.data;
            out.user.username = name;
            out.user.realName = real;
            out.user.role     = nu.role;
            out.user.status   = nu.status;
        }
        LOG_AUDIT(auditRid(rid), 0, "PUBLIC", "AUTH_REGISTER", kOk, out.user.id);
        LOG_INFO("rid={} action=AUTH_REGISTER uid={} username={} status=PENDING（待审核）",
                 auditRid(rid), out.user.id, name);
        return Result<RegisterData>::make(std::move(out));
    }

    // ---- ④ 不是"已注册"的失败，直接上报
    if (created.code != kUsernameTaken) {
        return Result<RegisterData>::fail(created.code, created.msg);
    }

    // ---- ⑤ 学号已存在：按状态分流（REJECTED 允许自助重提）
    auto found = userRepo_.findByUsername(conn, name);
    if (!found.ok()) {
        return Result<RegisterData>::fail(found.code, found.msg);
    }
    if (!found.data.has_value()) {
        // 极窄的竞态窗口：刚插入的账号又被删了。按已注册处理，不给攻击者可利用的差异化信息。
        return Result<RegisterData>::fail(kUsernameTaken, "该学号已被注册");
    }

    const model::User exist = *found.data;

    if (exist.status == model::UserStatus::PENDING) {
        LOG_INFO("rid={} action=AUTH_REGISTER uid={} code={} reason=重复提交且仍在待审核",
                 auditRid(rid), exist.id, static_cast<int>(kAccountPending));
        return Result<RegisterData>::fail(kAccountPending, "该学号已提交注册，正在等待审核");
    }

    if (exist.status == model::UserStatus::REJECTED) {
        // CAS：只有当前仍是 REJECTED 才覆盖；否则说明并发下被别人改过
        auto re = userRepo_.resubmit(conn, exist.id, real, hash);
        if (!re.ok()) {
            return Result<RegisterData>::fail(re.code, re.msg);
        }
        if (!re.data) {
            LOG_INFO("rid={} action=AUTH_REGISTER uid={} code={} reason=重提时状态已变（CAS 未命中）",
                     auditRid(rid), exist.id, static_cast<int>(kUsernameTaken));
            return Result<RegisterData>::fail(kUsernameTaken, "该学号已被注册");
        }
        auto fresh = userRepo_.findById(conn, exist.id);
        RegisterData out;
        if (fresh.ok() && fresh.data.has_value()) {
            out.user = *fresh.data;
        } else {
            out.user          = exist;
            out.user.realName = real;
            out.user.status   = model::UserStatus::PENDING;
        }
        LOG_AUDIT(auditRid(rid), 0, "PUBLIC", "AUTH_RESUBMIT", kOk, exist.id);
        LOG_INFO("rid={} action=AUTH_RESUBMIT uid={} username={} REJECTED→PENDING", auditRid(rid),
                 exist.id, name);
        return Result<RegisterData>::make(std::move(out));
    }

    // NORMAL / BLACKLIST / DISABLED：学号确实被占了
    LOG_INFO("rid={} action=AUTH_REGISTER uid={} code={} reason=学号已被注册（status={}）",
             auditRid(rid), exist.id, static_cast<int>(kUsernameTaken),
             model::toStr(exist.status));
    return Result<RegisterData>::fail(kUsernameTaken, "该学号已被注册");
}

Result<model::User> AuthService::me(int64_t userId) {
    if (userId <= 0) {
        return Result<model::User>::fail(kUnauthorized, "未提供有效登录令牌");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<model::User>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    auto found = userRepo_.findById(*guard, userId);
    if (!found.ok()) {
        return Result<model::User>::fail(found.code, found.msg);
    }
    if (!found.data.has_value()) {
        // 令牌合法但账号已被物理删除：按「令牌无效」处理，避免泄露内部状态
        return Result<model::User>::fail(kUnauthorized, "登录令牌对应的账号不存在，请重新登录");
    }
    return Result<model::User>::make(*found.data);
}

Result<Empty> AuthService::logout(const std::string& rawToken, int64_t expEpoch,
                                  const AuthContext& ctx) {
    if (rawToken.empty()) {
        return Result<Empty>::fail(kUnauthorized, "未提供有效登录令牌");
    }
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t       remain = expEpoch - now;
    if (remain < 1) {
        remain = 1;   // 已过期令牌也写一个最短 TTL，保证幂等且不报错
    }
    const int ttl = static_cast<int>(remain > 86400 ? 86400 : remain);

    auto r = redis_.setEx(JwtUtil::blacklistKey(rawToken), "1", ttl);
    if (!r.ok()) {
        LOG_ERROR("rid={} action=AUTH_LOGOUT code={} uid={} 失效名单写入失败：{}", requestId(),
                  static_cast<int>(kCacheError), ctx.userId, r.msg);
        return Result<Empty>::fail(kCacheError, "缓存访问失败：登出未完成，请稍后重试");
    }

    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "AUTH_LOGOUT", kOk, ctx.userId);
    return Result<Empty>::make(Empty{});
}

Result<Empty> AuthService::changePassword(int64_t userId, const std::string& oldPwd,
                                          const std::string& newPwd, const AuthContext& ctx) {
    if (userId <= 0) {
        return Result<Empty>::fail(kUnauthorized, "未提供有效登录令牌");
    }
    if (newPwd.size() < kPwdMinLen || newPwd.size() > kPwdMaxLen) {
        return Result<Empty>::fail(kInvalidParam,
                                   "参数校验失败：newPassword 长度必须为 8..64 个字符");
    }
    if (oldPwd.empty()) {
        return Result<Empty>::fail(kInvalidParam, "参数校验失败：oldPassword 不能为空");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Empty>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }

    auto found = userRepo_.findById(*guard, userId);
    if (!found.ok()) {
        return Result<Empty>::fail(found.code, found.msg);
    }
    if (!found.data.has_value()) {
        return Result<Empty>::fail(kUnauthorized, "登录令牌对应的账号不存在，请重新登录");
    }
    if (!crypto::verifyPassword(oldPwd, found.data->passwordHash)) {
        LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "AUTH_PASSWORD",
                  kOldPasswordWrong, userId);
        return Result<Empty>::fail(kOldPasswordWrong, "原密码错误");
    }
    if (newPwd == oldPwd) {
        return Result<Empty>::fail(kInvalidParam, "参数校验失败：新密码不能与原密码相同");
    }

    const std::string newHash = crypto::hashPassword(newPwd);
    if (newHash.empty()) {
        return Result<Empty>::fail(kInternalError, "服务内部错误：口令哈希生成失败");
    }
    auto upd = userRepo_.updatePasswordHash(*guard, userId, newHash);
    if (!upd.ok()) {
        LOG_ERROR("rid={} action=AUTH_PASSWORD uid={} 写库失败：{}", requestId(), userId, upd.msg);
        return Result<Empty>::fail(upd.code, upd.msg);
    }

    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), "AUTH_PASSWORD", kOk, userId);
    return Result<Empty>::make(Empty{});
}

} // namespace rf
