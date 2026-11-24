// =============================================================================
// src/controller/Controllers.h  —  三个控制器的公开声明 + 入参解析小工具
// 对应 ARCH-P0 §3.11 / §5 接口映射表
// 分层约束：只做 HTTP 出入参、JSON 编解码、路径/查询参数提取、字段校验；
//           不写 SQL、不碰 DbPool、不做业务判断（业务在 Service）。
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>

#include "RedisClient.h"
#include "Base.h"
#include "Result.h"
#include "DbPool.h"
#include "httplib.h"
#include "json.hpp"
#include "AuthService.h"
#include "ResourceService.h"
#include "ReservationService.h"
#include "WaitlistService.h"
#include "Scheduler.h"
#include "MaintenanceJob.h"
#include "RuleCache.h"
#include "RuleRepo.h"
#include "CreditService.h"

namespace rf {

/** 唯一允许出现 nlohmann::json 类型别名的地方（§8.8） */
using Json = nlohmann::json;

// 中间件与 Controller 之间的统一契约：Controller 只返回 Result<Json>，不碰 Response
using OpenHandler = std::function<Result<Json>(const httplib::Request&)>;
using JsonHandler = std::function<Result<Json>(const httplib::Request&, const AuthContext&)>;

/** 入参解析小工具（header-only，仅 Controller 层使用） */
namespace detail {

/**
 * 解析请求体为 JSON 对象。
 * 空体视为 {}（交给字段校验报 1000）；非法 JSON → 1005；不是对象 → 1000。
 * @param out 成功时写入解析结果
 * @param err 失败时写入可直接返回的 Result
 * @return true 表示解析成功
 */
inline bool parseJsonBody(const httplib::Request& req, Json& out, Result<Json>& err) {
    out = Json::object();
    if (req.body.empty()) {
        return true;
    }
    try {
        out = Json::parse(req.body);
    } catch (const std::exception&) {
        err = Result<Json>::fail(kMalformedJson, "请求体不是合法的 JSON");
        return false;
    }
    if (out.is_null()) {
        out = Json::object();
        return true;
    }
    if (!out.is_object()) {
        err = Result<Json>::fail(kInvalidParam, "参数校验失败：请求体必须是 JSON 对象");
        return false;
    }
    return true;
}

/**
 * 取路径参数 id（httplib 正则捕获组 1）。
 * @return 合法正整数 id；缺失 / 非数字 / 超长 / <=0 返回 nullopt
 */
inline std::optional<int64_t> pathId(const httplib::Request& req) {
    if (req.matches.size() < 2) {
        return std::nullopt;
    }
    const std::string s = req.matches[1].str();
    if (s.empty() || s.size() > 19) {
        return std::nullopt;   // 19 位十进制上限，防 stoll 溢出
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    const int64_t v = static_cast<int64_t>(std::strtoll(s.c_str(), nullptr, 10));
    if (v <= 0) {
        return std::nullopt;
    }
    return v;
}

/** 取查询参数整数；缺失或非数字返回默认值（D-8：非法页码按默认处理） */
inline int queryInt(const httplib::Request& req, const char* key, int def) {
    if (!req.has_param(key)) {
        return def;
    }
    const std::string v = req.get_param_value(key);
    if (v.empty()) {
        return def;
    }
    try {
        size_t        used = 0;
        const long long parsed = std::stoll(v, &used);
        if (used != v.size()) {
            return def;
        }
        return static_cast<int>(parsed);
    } catch (const std::exception&) {
        return def;
    }
}

/** 取查询参数字符串（去首尾空白）；缺失或空串返回 nullopt */
inline std::optional<std::string> queryStr(const httplib::Request& req, const char* key) {
    if (!req.has_param(key)) {
        return std::nullopt;
    }
    const std::string v = Str::trim(req.get_param_value(key));
    if (v.empty()) {
        return std::nullopt;
    }
    return v;
}

}  // namespace detail

/** 账号相关接口 */
class AuthController {
public:
    explicit AuthController(AuthService& svc);
    ~AuthController() = default;
    AuthController(const AuthController&)            = delete;
    AuthController& operator=(const AuthController&) = delete;

    Result<Json> login(const httplib::Request& req);                                    // 公开
    Result<Json> registerUser(const httplib::Request& req);                             // 公开（P6）
    Result<Json> logout(const httplib::Request& req, const AuthContext& ctx);            // 登录态
    Result<Json> me(const httplib::Request& req, const AuthContext& ctx);                // 登录态
    Result<Json> changePassword(const httplib::Request& req, const AuthContext& ctx);    // 登录态

private:
    AuthService& svc_;
};

/** 资源相关接口（共 7 个） */
class ResourceController {
public:
    explicit ResourceController(ResourceService& svc);
    ~ResourceController() = default;
    ResourceController(const ResourceController&)            = delete;
    ResourceController& operator=(const ResourceController&) = delete;

    Result<Json> meta(const httplib::Request& req, const AuthContext& ctx);          // GET   /resources/meta
    Result<Json> list(const httplib::Request& req, const AuthContext& ctx);          // GET   /resources
    Result<Json> detail(const httplib::Request& req, const AuthContext& ctx);        // GET   /resources/{id}
    Result<Json> create(const httplib::Request& req, const AuthContext& ctx);        // POST  /resources
    Result<Json> update(const httplib::Request& req, const AuthContext& ctx);        // PUT   /resources/{id}
    Result<Json> remove(const httplib::Request& req, const AuthContext& ctx);        // DELETE
    Result<Json> changeStatus(const httplib::Request& req, const AuthContext& ctx);  // PATCH /{id}/status

private:
    ResourceService& svc_;
};

/** 系统探针 */
class SystemController {
public:
    SystemController(DbPool& pool, RedisClient& redis, repo::UserRepo& userRepo);
    ~SystemController() = default;
    SystemController(const SystemController&)            = delete;
    SystemController& operator=(const SystemController&) = delete;

    Result<Json> healthDeps(const httplib::Request& req);   // 公开
    /**
     * P4 T00 新增（只读）：GET /admin/users?page&size&status&keyword
     * 管理后台「用户与信用」页的表格数据源；也用于概览的「黑名单人数」。
     */
    Result<Json> users(const httplib::Request& req, const AuthContext& ctx);

    /**
     * P6 新增：审核通过 / 审核拒绝（仅 ADMIN）。
     * 语义是 CAS（`WHERE status='PENDING'`）：当前不是 PENDING → 2007。
     * 不存在的 id → 1004（沿用既有的"目标不存在/无权"口径，不新造码）。
     * @param approve true=通过（PENDING→NORMAL），false=拒绝（PENDING→REJECTED）
     * @param req     请求体可选：{"reason":"..."}，**只进日志不落库**（ARCH-P6 §3.2）
     */
    Result<Json> reviewUser(const httplib::Request& req, const AuthContext& ctx, bool approve);

private:
    DbPool&          pool_;
    RedisClient&     redis_;
    repo::UserRepo&  userRepo_;
};

/** 预约相关接口（7 个，ARCH-P1 §3.8） */
class ReservationController {
public:
    explicit ReservationController(ReservationService& svc);
    ~ReservationController() = default;
    ReservationController(const ReservationController&)            = delete;
    ReservationController& operator=(const ReservationController&) = delete;

    Result<Json> create(const httplib::Request& req, const AuthContext& ctx);        // POST   /reservations
    Result<Json> list(const httplib::Request& req, const AuthContext& ctx);          // GET    /reservations
    Result<Json> detail(const httplib::Request& req, const AuthContext& ctx);        // GET    /reservations/{id}
    Result<Json> cancel(const httplib::Request& req, const AuthContext& ctx);        // POST   /reservations/{id}/cancel
    Result<Json> approve(const httplib::Request& req, const AuthContext& ctx);       // POST   /reservations/{id}/approve
    Result<Json> reject(const httplib::Request& req, const AuthContext& ctx);        // POST   /reservations/{id}/reject
    Result<Json> occupiedSlots(const httplib::Request& req, const AuthContext& ctx); // GET    /resources/{id}/slots

    // ---- P2 增量（ARCH-P2 §3.9）：取用 / 归还 / 验收 / 损坏 / 押金流水 ----
    Result<Json> pickup(const httplib::Request& req, const AuthContext& ctx);    // POST /reservations/{id}/pickup
    Result<Json> returnBack(const httplib::Request& req, const AuthContext& ctx);// POST /reservations/{id}/return
    Result<Json> accept(const httplib::Request& req, const AuthContext& ctx);    // POST /reservations/{id}/accept
    Result<Json> damage(const httplib::Request& req, const AuthContext& ctx);    // POST /reservations/{id}/damage
    Result<Json> deposits(const httplib::Request& req, const AuthContext& ctx);  // GET  /reservations/{id}/deposits
    Result<Json> timeline(const httplib::Request& req, const AuthContext& ctx);  // GET  /reservations/{id}/timeline（P4 T00 新增）

private:
    ReservationService& svc_;
};

/**
 * 候补相关接口（5 个，ARCH-P2 §3.9）。
 * T05 实现 join / list / leave；confirm / dispatchDebug 属 T08（声明先落定）。
 */
class WaitlistController {
public:
    explicit WaitlistController(WaitlistService& svc);
    ~WaitlistController() = default;
    WaitlistController(const WaitlistController&)            = delete;
    WaitlistController& operator=(const WaitlistController&) = delete;

    Result<Json> join(const httplib::Request& req, const AuthContext& ctx);          // POST   /resources/{id}/waitlist
    Result<Json> list(const httplib::Request& req, const AuthContext& ctx);          // GET    /waitlist
    Result<Json> leave(const httplib::Request& req, const AuthContext& ctx);         // DELETE /waitlist/{id}
    Result<Json> confirm(const httplib::Request& req, const AuthContext& ctx);       // POST   /waitlist/{id}/confirm  【T08】
    Result<Json> dispatchDebug(const httplib::Request& req, const AuthContext& ctx); // POST   /waitlist/dispatch   【T08，ADMIN】

private:
    WaitlistService& svc_;
};

/** 运维接口（ADMIN）：手动跑一轮维护，用于验收与排障（ARCH-P2 §3.9） */
class MaintenanceController {
public:
    MaintenanceController(Scheduler& scheduler, MaintenanceJob& job);
    ~MaintenanceController() = default;
    MaintenanceController(const MaintenanceController&)            = delete;
    MaintenanceController& operator=(const MaintenanceController&) = delete;

    Result<Json> runOnce(const httplib::Request& req, const AuthContext& ctx);  // POST /admin/jobs/maintenance

private:
    Scheduler&      scheduler_;
    MaintenanceJob& job_;
};

/**
 * 规则管理接口（ARCH-P3 §7.5 / T08）。全部 ADMIN。
 * 与文档签名的差异：多一个 DbPool& —— 按 id 读单条规则需要一个连接，
 * RuleCache 只提供整表快照。
 */
class RuleController {
public:
    RuleController(RuleCache& cache, repo::RuleRepo& repo, DbPool& pool);
    ~RuleController() = default;
    RuleController(const RuleController&)            = delete;
    RuleController& operator=(const RuleController&) = delete;

    Result<Json> list(const httplib::Request& req, const AuthContext& ctx);        // GET    /admin/rules
    Result<Json> detail(const httplib::Request& req, const AuthContext& ctx);      // GET    /admin/rules/{id}
    Result<Json> create(const httplib::Request& req, const AuthContext& ctx);      // POST   /admin/rules
    Result<Json> update(const httplib::Request& req, const AuthContext& ctx);      // PUT    /admin/rules/{id}
    Result<Json> setEnabled(const httplib::Request& req, const AuthContext& ctx);  // PATCH  /admin/rules/{id}/enabled
    Result<Json> setPriority(const httplib::Request& req, const AuthContext& ctx); // PATCH  /admin/rules/{id}/priority
    Result<Json> remove(const httplib::Request& req, const AuthContext& ctx);      // DELETE /admin/rules/{id}
    Result<Json> validate(const httplib::Request& req, const AuthContext& ctx);    // POST   /admin/rules/validate

private:
    RuleCache&        cache_;
    repo::RuleRepo&   repo_;
    DbPool&           pool_;
};

/**
 * 信用体系接口（ARCH-P3 §7.5 / T09）。
 * 与文档签名的差异：多一个 DbPool& —— applyDelta/account 都要连接。
 */
class CreditController {
public:
    CreditController(CreditService& svc, DbPool& pool);
    ~CreditController() = default;
    CreditController(const CreditController&)            = delete;
    CreditController& operator=(const CreditController&) = delete;

    Result<Json> me(const httplib::Request& req, const AuthContext& ctx);          // GET  /credit/me
    Result<Json> myLogs(const httplib::Request& req, const AuthContext& ctx);      // GET  /credit/me/logs
    Result<Json> userAccount(const httplib::Request& req, const AuthContext& ctx); // GET  /admin/credit/{userId}
    Result<Json> adjust(const httplib::Request& req, const AuthContext& ctx);      // POST /admin/credit/{userId}/adjust

private:
    CreditService& svc_;
    DbPool&        pool_;
};

} // namespace rf
