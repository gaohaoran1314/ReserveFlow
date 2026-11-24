// =============================================================================
// src/app/Application.cpp  —  依赖装配、12 条路由注册、CORS、/debug 挂载、优雅退出
// 对应 ARCH-P0 §3.13 / §1.2 / §1.4.8 / §5
// 关键点：
//   1) /resources/meta 必须注册在 /resources/(\d+) 之前（httplib 按注册顺序匹配）；
//   2) OPTIONS 预检返回 204 + Access-Control-*（页面可能以 file:// 打开）；
//   3) 全局默认响应头带 CORS；
//   4) 未捕获异常 → 500 + 1008，进程存活；
//   5) MySQL / Redis 初始化失败不阻止 HTTP 起服务（依赖故障不自杀，§8.7）。
// =============================================================================
#include "Application.h"

#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "AuthMiddleware.h"
#include "RedisClient.h"
#include "Base.h"
#include "Result.h"
#include "Config.h"
#include "Controllers.h"
#include "DbPool.h"
#include "httplib.h"
#include "json.hpp"
#include "AuditRepo.h"
#include "ResourceRepo.h"
#include "UserRepo.h"
#include "JwtUtil.h"
#include "AuthService.h"
#include "ResourceService.h"
#include "ReservationRepo.h"
#include "ReservationService.h"
#include "StateMachine.h"
#include "DistributedLock.h"
#include "SettlementRepo.h"
#include "SettlementService.h"
#include "WaitlistRepo.h"
#include "WaitlistService.h"
#include "Scheduler.h"
#include "MaintenanceJob.h"
#include "RuleRepo.h"
#include "RuleCache.h"
#include "RuleEngine.h"
#include "CreditRepo.h"
#include "CreditService.h"

namespace rf {

namespace {

/** 供信号处理使用的全局句柄（进程内唯一 Server 实例） */
httplib::Server*    g_server      = nullptr;
volatile std::sig_atomic_t g_stoppedBySignal = 0;

void onTerminateSignal(int /*sig*/) {
    g_stoppedBySignal = 1;
    if (g_server != nullptr) {
        g_server->stop();
    }
}

/** 可执行文件所在目录（末带分隔符由 filesystem 处理） */
std::filesystem::path exeDir() {
#ifdef _WIN32
    const char* p = _pgmptr;   // CRT 提供的可执行文件全路径
    if (p != nullptr) {
        return std::filesystem::path(p).parent_path();
    }
#endif
    return std::filesystem::path();
}

/**
 * 定位 /debug 静态目录。依次尝试：
 *   1) config.server.debugRoot（相对配置文件/CWD）
 *   2) <exeDir>/debug            （CMake POST_BUILD 拷贝目标）
 *   3) <exeDir>/../web/debug     （从 build/ 运行时回退到源码树）
 *   4) ./web/debug
 */
std::string resolveDebugRoot(const ServerConfig& sc) {
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (!sc.debugRoot.empty()) {
        candidates.emplace_back(sc.debugRoot);
    }
    const auto dir = exeDir();
    if (!dir.empty()) {
        candidates.push_back(dir / "debug");
        candidates.push_back(dir / ".." / "web" / "debug");
    }
    candidates.emplace_back("./web/debug");

    for (const auto& c : candidates) {
        if (std::filesystem::is_directory(c, ec)) {
            return c.string();
        }
        ec.clear();
    }
    return std::string();
}

/**
 * 定位管理后台静态目录（P4 T12，与 resolveDebugRoot 同构）。
 * 依次尝试：
 *   1) config.server.adminRoot
 *   2) <exeDir>/admin                       （CMake POST_BUILD 拷贝目标）
 *   3) <exeDir>/../front/admin/dist         （从 back/ReserveFlow/rfbuild 运行时）
 *   4) <exeDir>/../../../front/admin/dist   （更深的构建目录）
 *   5) ./front/admin/dist
 * 找不到返回空串 → 只 WARN，服务照常启动（与 /debug 一致）。
 */
std::string resolveAdminRoot(const ServerConfig& sc) {
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (!sc.adminRoot.empty()) {
        candidates.emplace_back(sc.adminRoot);
    }
    const auto dir = exeDir();
    if (!dir.empty()) {
        candidates.push_back(dir / "admin");                                         // POST_BUILD 拷贝目标
        candidates.push_back(dir / ".." / ".." / ".." / "front" / "admin" / "dist");  // rfbuild → 仓库根
        candidates.push_back(dir / ".." / ".." / "front" / "admin" / "dist");        // cmake-build-debug/debug 之类更深一层
    }
    candidates.emplace_back("./front/admin/dist");

    for (const auto& c : candidates) {
        if (std::filesystem::is_directory(c, ec)) {
            // 产物目录必须真的有 index.html，否则挂了也是 404
            if (std::filesystem::exists(c / "index.html", ec)) {
                return c.string();
            }
        }
        ec.clear();
    }
    return std::string();
}

/**
 * 定位用户端 H5 静态目录（P5 T12，与 resolveAdminRoot 同构）。
 * 依次尝试：
 *   1) config.server.h5Root
 *   2) <exeDir>/h5                        （CMake POST_BUILD 拷贝目标）
 *   3) <exeDir>/../../../front/h5/dist    （rfbuild → 仓库根）
 *   4) <exeDir>/../../front/h5/dist
 *   5) ./front/h5/dist
 * 找不到返回空串 → 只 WARN，服务照常启动（与 /debug、/admin 一致）。
 */
std::string resolveH5Root(const ServerConfig& sc) {
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (!sc.h5Root.empty()) {
        candidates.emplace_back(sc.h5Root);
    }
    const auto dir = exeDir();
    if (!dir.empty()) {
        candidates.push_back(dir / "h5");
        candidates.push_back(dir / ".." / ".." / ".." / "front" / "h5" / "dist");
        candidates.push_back(dir / ".." / ".." / "front" / "h5" / "dist");
    }
    candidates.emplace_back("./front/h5/dist");

    for (const auto& c : candidates) {
        if (std::filesystem::is_directory(c, ec)) {
            if (std::filesystem::exists(c / "index.html", ec)) {
                return c.string();
            }
        }
        ec.clear();
    }
    return std::string();
}

/** 统一错误响应体（错误处理与异常处理共用） */
void writeErrorBody(httplib::Response& res, int code, const std::string& msg) {
    nlohmann::json body = nlohmann::json::object();
    body["code"] = code;
    body["msg"]  = msg;
    body["data"] = nlohmann::json::object();
    res.set_content(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                    "application/json; charset=utf-8");
}

}  // namespace

// ============================================================================
// Impl：成员声明顺序即构造顺序；析构逆序 → Controller/Service 先于 Repo/Pool 销毁
// ============================================================================
struct Application::Impl {
    Config      cfg;
    std::string configPath;

    std::unique_ptr<DbPool>      pool;
    std::unique_ptr<RedisClient> redis;
    std::unique_ptr<JwtUtil>     jwt;

    // ---- P1/P2：声明顺序即构造顺序，析构逆序 →
    //      maintenanceJob → scheduler → distributedLock → lockWatchdog → redis
    //      即「先停调度线程（任务还在借 Redis），再停看门狗，最后关 Redis」（§5.1）
    std::unique_ptr<LockWatchdog>    lockWatchdog;
    std::unique_ptr<DistributedLock> distributedLock;
    std::unique_ptr<StateMachine>    stateMachine;

    repo::UserRepo        userRepo;       // 无状态，栈上持有
    repo::ResourceRepo    resourceRepo;
    repo::AuditRepo       auditRepo;
    repo::ReservationRepo reservationRepo;
    repo::SettlementRepo  settlementRepo;
    repo::WaitlistRepo    waitlistRepo;
    repo::RuleRepo        ruleRepo;       // P3
    repo::CreditRepo      creditRepo;     // P3

    // P3：缓存/引擎/信用要早于 Service 构造；析构逆序时 Service 先死，安全
    std::unique_ptr<RuleCache>     ruleCache;
    std::unique_ptr<RuleEngine>    ruleEngine;
    std::unique_ptr<CreditService> creditService;

    std::unique_ptr<AuthService>        authService;
    std::unique_ptr<ResourceService>    resourceService;
    std::unique_ptr<SettlementService>  settlementService;
    std::unique_ptr<WaitlistService>    waitlistService;
    std::unique_ptr<ReservationService> reservationService;
    std::unique_ptr<Scheduler>          scheduler;
    std::unique_ptr<MaintenanceJob>     maintenanceJob;
    std::unique_ptr<AuthController>     authCtrl;
    std::unique_ptr<ResourceController> resourceCtrl;
    std::unique_ptr<ReservationController> reservationCtrl;
    std::unique_ptr<WaitlistController>  waitlistCtrl;
    std::unique_ptr<MaintenanceController> maintenanceCtrl;
    std::unique_ptr<RuleController>      ruleCtrl;
    std::unique_ptr<CreditController>    creditCtrl;
    std::unique_ptr<SystemController>   systemCtrl;
    std::unique_ptr<AuthMiddleware>     middleware;

    httplib::Server svr;

    // 三个静态入口的挂载结果（P5：启动横幅据此只打印真实可用的入口）
    bool debugMounted = false;
    bool adminMounted = false;
    bool h5Mounted    = false;
};

Application::Application() : impl_(std::make_unique<Impl>()) {}

Application::~Application() {
    stop();
    if (impl_) {
        if (impl_->redis) {
            impl_->redis->shutdown();
        }
        if (impl_->pool) {
            impl_->pool->shutdown();
        }
    }
}

bool Application::init(const std::string& configPath, std::string& err) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->configPath = configPath;

    // ---- 1) 配置
    if (!Config::load(configPath, impl_->cfg, err)) {
        return false;
    }
    const Config& cfg = impl_->cfg;

    // ---- 2) 日志（控制台彩色 + 滚动文件）
    initLogger(cfg.log.level, cfg.log.file, cfg.log.rotateSizeMB, cfg.log.rotateCount);
    LOG_INFO("ReserveFlow 启动（v0.5 · P5）：配置={} 监听={}:{} 线程池={}", configPath, cfg.server.host,
             cfg.server.port, cfg.server.threadPool);
    LOG_INFO("存储：MySQL {}:{}/{}（pool={}），Redis {}:{}（pool={}）", cfg.mysql.host,
             cfg.mysql.port, cfg.mysql.database, cfg.mysql.poolSize, cfg.redis.host, cfg.redis.port,
             cfg.redis.poolSize);

    // ---- 3) DbPool（失败不阻断启动：业务接口将返回 1009）
    impl_->pool = std::make_unique<DbPool>(cfg.mysql);
    std::string dbErr;
    if (!impl_->pool->init(dbErr)) {
        LOG_ERROR("MySQL 初始化失败，业务接口将返回 1009（进程继续运行）：{}", dbErr);
    }

    // ---- 4) Redis（失败不阻断启动：鉴权失效名单 fail-open，探针报 down）
    impl_->redis = std::make_unique<RedisClient>(cfg.redis);
    std::string rdErr;
    if (!impl_->redis->init(rdErr)) {
        LOG_WARN("Redis 初始化失败，失效名单检查将 fail-open（进程继续运行）：{}", rdErr);
    }

    // ---- 5) JWT
    impl_->jwt = std::make_unique<JwtUtil>(cfg.jwt);

    // ---- 5b) P1 内核：状态机（phase 门控）+ 看门狗 + 分布式锁
    impl_->stateMachine = std::make_unique<StateMachine>(cfg.feature.phase);
    impl_->lockWatchdog = std::make_unique<LockWatchdog>(*impl_->redis, cfg.lock.renewIntervalMs,
                                                         cfg.lock.ttlMs);
    impl_->lockWatchdog->start();
    impl_->distributedLock = std::make_unique<DistributedLock>(*impl_->redis, *impl_->lockWatchdog,
                                                               cfg.lock.retryIntervalMs);

    // ---- 5c) P3 规则引擎：仓储 → 缓存 → 引擎（引擎在 Service 之前就绪）
    impl_->ruleCache = std::make_unique<RuleCache>(*impl_->pool, impl_->ruleRepo,
                                                   cfg.rule.cacheTtlSec);
    {
        std::string ruleErr;
        if (!impl_->ruleCache->init(ruleErr)) {
            // 规则表不可读不阻断启动：引擎会 degraded，各业务走 P2 兜底
            LOG_WARN("规则缓存初始化失败，引擎将以 degraded 模式运行（业务走 P2 兜底）：{}", ruleErr);
        } else {
            LOG_INFO("规则引擎{}：已加载 {} 条规则（cacheTtl={}s）",
                     cfg.feature.ruleEngineEnabled ? "启用" : "（总开关关闭，仅加载不生效）",
                     impl_->ruleCache->size(), cfg.rule.cacheTtlSec);
        }
    }
    impl_->ruleEngine = std::make_unique<RuleEngine>(*impl_->ruleCache, cfg.feature.ruleEngineEnabled);
    impl_->creditService =
        std::make_unique<CreditService>(*impl_->pool, impl_->creditRepo, cfg.credit);

    // ---- 6) Service（P2：结算与候补先于预约服务，预约服务持有它们的引用）
    impl_->authService = std::make_unique<AuthService>(*impl_->pool, *impl_->redis, *impl_->jwt);
    impl_->resourceService =
        std::make_unique<ResourceService>(*impl_->pool, impl_->resourceRepo, impl_->auditRepo);
    impl_->settlementService = std::make_unique<SettlementService>(
        *impl_->pool, impl_->settlementRepo, *impl_->creditService, cfg.deposit);
    impl_->waitlistService = std::make_unique<WaitlistService>(
        *impl_->pool, *impl_->redis, impl_->waitlistRepo, impl_->reservationRepo,
        impl_->resourceRepo, impl_->userRepo, impl_->auditRepo, *impl_->stateMachine,
        *impl_->distributedLock, *impl_->settlementService, cfg.waitlist, cfg.lock.waitMs,
        cfg.lock.ttlMs, cfg.feature.minCreditScore);
    impl_->reservationService = std::make_unique<ReservationService>(
        *impl_->pool, *impl_->redis, impl_->reservationRepo, impl_->resourceRepo, impl_->userRepo,
        impl_->auditRepo, *impl_->stateMachine, *impl_->distributedLock, *impl_->settlementService,
        *impl_->waitlistService, *impl_->ruleEngine, cfg.pickup, cfg.maintenance, cfg.lock.waitMs,
        cfg.lock.ttlMs, cfg.feature.minCreditScore);

    // ---- 6b) 定时维护（P2）：Scheduler 只负责按时触发，MaintenanceJob 负责扫什么
    impl_->maintenanceJob = std::make_unique<MaintenanceJob>(
        *impl_->pool, *impl_->distributedLock, *impl_->stateMachine, impl_->reservationRepo,
        impl_->resourceRepo, impl_->auditRepo, impl_->waitlistRepo, *impl_->settlementService,
        *impl_->waitlistService, *impl_->ruleEngine, cfg.maintenance, cfg.pickup);
    impl_->scheduler = std::make_unique<Scheduler>(
        "maintenance", cfg.maintenance.intervalSec,
        [job = impl_->maintenanceJob.get()](const std::string& tickId) { job->runOnce(tickId); });
    if (cfg.maintenance.enabled) {
        impl_->scheduler->start();
    } else {
        LOG_WARN("maintenance.enabled=false：调度线程未启动（仍可用 POST /admin/jobs/maintenance 手动触发）");
    }

    // ---- 7) Controller
    impl_->authCtrl     = std::make_unique<AuthController>(*impl_->authService);
    impl_->resourceCtrl = std::make_unique<ResourceController>(*impl_->resourceService);
    impl_->reservationCtrl = std::make_unique<ReservationController>(*impl_->reservationService);
    impl_->waitlistCtrl = std::make_unique<WaitlistController>(*impl_->waitlistService);
    impl_->maintenanceCtrl =
        std::make_unique<MaintenanceController>(*impl_->scheduler, *impl_->maintenanceJob);
    impl_->ruleCtrl   = std::make_unique<RuleController>(*impl_->ruleCache, impl_->ruleRepo,
                                                          *impl_->pool);
    impl_->creditCtrl = std::make_unique<CreditController>(*impl_->creditService, *impl_->pool);
    impl_->systemCtrl =
        std::make_unique<SystemController>(*impl_->pool, *impl_->redis, impl_->userRepo);

    // ---- 8) 中间件
    impl_->middleware = std::make_unique<AuthMiddleware>(*impl_->jwt, *impl_->redis);

    // ---- 9) 路由
    registerRoutes();
    return true;
}

void Application::registerRoutes() {
    httplib::Server& svr = impl_->svr;
    const ServerConfig& sc = impl_->cfg.server;

    // ---- 全局：请求体上限 1MiB、线程池、CORS 默认头（§1.4.8 / §8.9）
    svr.set_payload_max_length(1024U * 1024U);
    const int threads = sc.threadPool > 0 ? sc.threadPool : 8;
    svr.new_task_queue = [threads]() { return new httplib::ThreadPool(threads); };
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS"},
        {"Access-Control-Max-Age", "86400"},
    });

    // ---- CORS 预检：204 且无响应体
    svr.Options(R"(/.*)", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.status = 204;
    });

    AuthMiddleware*       mw = impl_->middleware.get();
    AuthController*       ac = impl_->authCtrl.get();
    ResourceController*   rc = impl_->resourceCtrl.get();
    SystemController*     sc2 = impl_->systemCtrl.get();
    ReservationController* rsvc = impl_->reservationCtrl.get();
    WaitlistController*    wctrl = impl_->waitlistCtrl.get();
    MaintenanceController* mctrl = impl_->maintenanceCtrl.get();
    SystemController*      sctrl   = impl_->systemCtrl.get();
    RuleController*        rulectl = impl_->ruleCtrl.get();
    CreditController*      creditctl = impl_->creditCtrl.get();

    // ---- 公开路由（#1 登录、#5 探针）
    svr.Post("/api/v1/auth/login",
             mw->open([ac](const httplib::Request& req) { return ac->login(req); }));
    // P6：学生自助注册（公开，不过 protect —— 注册时还没有 token）
    svr.Post("/api/v1/auth/register",
             mw->open([ac](const httplib::Request& req) { return ac->registerUser(req); }));
    svr.Get("/api/v1/health/deps",
            mw->open([sc2](const httplib::Request& req) { return sc2->healthDeps(req); }));

    // ---- 登录态路由（#2 登出、#3 当前用户、#4 改密）
    svr.Post("/api/v1/auth/logout", mw->protect([ac](const httplib::Request& req,
                                                     const AuthContext& ctx) {
        return ac->logout(req, ctx);
    }));
    svr.Get("/api/v1/auth/me", mw->protect([ac](const httplib::Request& req,
                                                const AuthContext& ctx) {
        return ac->me(req, ctx);
    }));
    svr.Put("/api/v1/auth/password", mw->protect([ac](const httplib::Request& req,
                                                     const AuthContext& ctx) {
        return ac->changePassword(req, ctx);
    }));

    // ---- 资源只读（#6 meta / #7 列表 / #8 详情）
    // 注意顺序：/resources/meta 必须在 /resources/(\d+) 之前，否则被正则吞掉
    //           /resources/(\d+)/slots 也注册在 /resources/(\d+) 之前（具体优先，ARCH-P1 §3.11）
    svr.Get("/api/v1/resources/meta", mw->protect([rc](const httplib::Request& req,
                                                       const AuthContext& ctx) {
        return rc->meta(req, ctx);
    }));
    svr.Get(R"(/api/v1/resources/(\d+)/slots)", mw->protect([rsvc](const httplib::Request& req,
                                                                  const AuthContext& ctx) {
        return rsvc->occupiedSlots(req, ctx);
    }));
    svr.Get("/api/v1/resources", mw->protect([rc](const httplib::Request& req,
                                                  const AuthContext& ctx) {
        return rc->list(req, ctx);
    }));
    svr.Get(R"(/api/v1/resources/(\d+))", mw->protect([rc](const httplib::Request& req,
                                                           const AuthContext& ctx) {
        return rc->detail(req, ctx);
    }));

    // ---- 资源写操作（#9 新增 / #10 修改 / #11 删除 / #12 改状态）：仅管理员
    svr.Post("/api/v1/resources", mw->protect([rc](const httplib::Request& req,
                                                   const AuthContext& ctx) {
        return rc->create(req, ctx);
    }, model::Role::ADMIN));
    svr.Put(R"(/api/v1/resources/(\d+))", mw->protect([rc](const httplib::Request& req,
                                                           const AuthContext& ctx) {
        return rc->update(req, ctx);
    }, model::Role::ADMIN));
    svr.Delete(R"(/api/v1/resources/(\d+))", mw->protect([rc](const httplib::Request& req,
                                                              const AuthContext& ctx) {
        return rc->remove(req, ctx);
    }, model::Role::ADMIN));
    svr.Patch(R"(/api/v1/resources/(\d+)/status)", mw->protect([rc](const httplib::Request& req,
                                                                    const AuthContext& ctx) {
        return rc->changeStatus(req, ctx);
    }, model::Role::ADMIN));

    // ---- 预约（P1，7 条）：下单/列表/详情/取消为登录态；审核通过/驳回仅管理员
    svr.Get("/api/v1/reservations", mw->protect([rsvc](const httplib::Request& req,
                                                       const AuthContext& ctx) {
        return rsvc->list(req, ctx);
    }));
    svr.Post("/api/v1/reservations", mw->protect([rsvc](const httplib::Request& req,
                                                        const AuthContext& ctx) {
        return rsvc->create(req, ctx);
    }));
    svr.Get(R"(/api/v1/reservations/(\d+))", mw->protect([rsvc](const httplib::Request& req,
                                                                const AuthContext& ctx) {
        return rsvc->detail(req, ctx);
    }));
    svr.Post(R"(/api/v1/reservations/(\d+)/cancel)", mw->protect([rsvc](const httplib::Request& req,
                                                                       const AuthContext& ctx) {
        return rsvc->cancel(req, ctx);
    }));
    svr.Post(R"(/api/v1/reservations/(\d+)/approve)", mw->protect([rsvc](const httplib::Request& req,
                                                                        const AuthContext& ctx) {
        return rsvc->approve(req, ctx);
    }, model::Role::ADMIN));
    svr.Post(R"(/api/v1/reservations/(\d+)/reject)", mw->protect([rsvc](const httplib::Request& req,
                                                                       const AuthContext& ctx) {
        return rsvc->reject(req, ctx);
    }, model::Role::ADMIN));

    // ---- P2 预约闭环（5 条）：取用/归还为登录态，验收/损坏仅管理员
    svr.Post(R"(/api/v1/reservations/(\d+)/pickup)", mw->protect([rsvc](const httplib::Request& req,
                                                                        const AuthContext& ctx) {
        return rsvc->pickup(req, ctx);
    }));
    svr.Post(R"(/api/v1/reservations/(\d+)/return)", mw->protect([rsvc](const httplib::Request& req,
                                                                        const AuthContext& ctx) {
        return rsvc->returnBack(req, ctx);
    }));
    svr.Post(R"(/api/v1/reservations/(\d+)/accept)", mw->protect([rsvc](const httplib::Request& req,
                                                                        const AuthContext& ctx) {
        return rsvc->accept(req, ctx);
    }, model::Role::ADMIN));
    svr.Post(R"(/api/v1/reservations/(\d+)/damage)", mw->protect([rsvc](const httplib::Request& req,
                                                                        const AuthContext& ctx) {
        return rsvc->damage(req, ctx);
    }, model::Role::ADMIN));
    svr.Get(R"(/api/v1/reservations/(\d+)/deposits)", mw->protect([rsvc](const httplib::Request& req,
                                                                         const AuthContext& ctx) {
        return rsvc->deposits(req, ctx);
    }));
    // P4 T00（只读）：状态时间线，本人或管理员
    svr.Get(R"(/api/v1/reservations/(\d+)/timeline)", mw->protect([rsvc](const httplib::Request& req,
                                                                         const AuthContext& ctx) {
        return rsvc->timeline(req, ctx);
    }));

    // ---- P2 候补（5 条）：入队挂在资源下；dispatch 是 ADMIN 调试入口
    svr.Post(R"(/api/v1/resources/(\d+)/waitlist)", mw->protect([wctrl](const httplib::Request& req,
                                                                        const AuthContext& ctx) {
        return wctrl->join(req, ctx);
    }));
    svr.Get("/api/v1/waitlist", mw->protect([wctrl](const httplib::Request& req,
                                                    const AuthContext& ctx) {
        return wctrl->list(req, ctx);
    }));
    svr.Post("/api/v1/waitlist/dispatch", mw->protect([wctrl](const httplib::Request& req,
                                                              const AuthContext& ctx) {
        return wctrl->dispatchDebug(req, ctx);
    }, model::Role::ADMIN));
    svr.Delete(R"(/api/v1/waitlist/(\d+))", mw->protect([wctrl](const httplib::Request& req,
                                                                 const AuthContext& ctx) {
        return wctrl->leave(req, ctx);
    }));
    svr.Post(R"(/api/v1/waitlist/(\d+)/confirm)", mw->protect([wctrl](const httplib::Request& req,
                                                                      const AuthContext& ctx) {
        return wctrl->confirm(req, ctx);
    }));

    // ---- P4 T00（只读）：用户列表（ADMIN），管理后台 /users 页与概览「黑名单人数」用
    svr.Get("/api/v1/admin/users", mw->protect([sctrl](const httplib::Request& req,
                                                       const AuthContext& ctx) {
        return sctrl->users(req, ctx);
    }, model::Role::ADMIN));

    // ---- P6：账号审核（ADMIN）。动作式路由而非 PATCH /status：
    //      合法动作天然只有两个，管理员无法把 PENDING 直接改成 BLACKLIST
    svr.Post(R"(/api/v1/admin/users/(\d+)/approve)", mw->protect([sctrl](const httplib::Request& req,
                                                                         const AuthContext& ctx) {
        return sctrl->reviewUser(req, ctx, /*approve=*/true);
    }, model::Role::ADMIN));
    svr.Post(R"(/api/v1/admin/users/(\d+)/reject)", mw->protect([sctrl](const httplib::Request& req,
                                                                        const AuthContext& ctx) {
        return sctrl->reviewUser(req, ctx, /*approve=*/false);
    }, model::Role::ADMIN));

    // ---- P2 运维：手动跑一轮维护（ADMIN，验收/排障用）
    svr.Post("/api/v1/admin/jobs/maintenance", mw->protect([mctrl](const httplib::Request& req,
                                                                    const AuthContext& ctx) {
        return mctrl->runOnce(req, ctx);
    }, model::Role::ADMIN));

    // ---- P3 信用（2 条登录态 + 2 条 ADMIN）
    svr.Get("/api/v1/credit/me", mw->protect([creditctl](const httplib::Request& req,
                                                         const AuthContext& ctx) {
        return creditctl->me(req, ctx);
    }));
    svr.Get("/api/v1/credit/me/logs", mw->protect([creditctl](const httplib::Request& req,
                                                              const AuthContext& ctx) {
        return creditctl->myLogs(req, ctx);
    }));
    svr.Get(R"(/api/v1/admin/credit/(\d+))", mw->protect([creditctl](const httplib::Request& req,
                                                                    const AuthContext& ctx) {
        return creditctl->userAccount(req, ctx);
    }, model::Role::ADMIN));
    svr.Post(R"(/api/v1/admin/credit/(\d+)/adjust)", mw->protect([creditctl](const httplib::Request& req,
                                                                           const AuthContext& ctx) {
        return creditctl->adjust(req, ctx);
    }, model::Role::ADMIN));

    // ---- P3 规则管理（8 条，全部 ADMIN）。注意 /admin/rules/validate 先于 /admin/rules/{id}
    svr.Get("/api/v1/admin/rules", mw->protect([rulectl](const httplib::Request& req,
                                                        const AuthContext& ctx) {
        return rulectl->list(req, ctx);
    }, model::Role::ADMIN));
    svr.Post("/api/v1/admin/rules/validate", mw->protect([rulectl](const httplib::Request& req,
                                                                   const AuthContext& ctx) {
        return rulectl->validate(req, ctx);
    }, model::Role::ADMIN));
    svr.Post("/api/v1/admin/rules", mw->protect([rulectl](const httplib::Request& req,
                                                         const AuthContext& ctx) {
        return rulectl->create(req, ctx);
    }, model::Role::ADMIN));
    svr.Get(R"(/api/v1/admin/rules/(\d+))", mw->protect([rulectl](const httplib::Request& req,
                                                                  const AuthContext& ctx) {
        return rulectl->detail(req, ctx);
    }, model::Role::ADMIN));
    svr.Put(R"(/api/v1/admin/rules/(\d+))", mw->protect([rulectl](const httplib::Request& req,
                                                                  const AuthContext& ctx) {
        return rulectl->update(req, ctx);
    }, model::Role::ADMIN));
    svr.Patch(R"(/api/v1/admin/rules/(\d+)/enabled)", mw->protect([rulectl](const httplib::Request& req,
                                                                          const AuthContext& ctx) {
        return rulectl->setEnabled(req, ctx);
    }, model::Role::ADMIN));
    svr.Patch(R"(/api/v1/admin/rules/(\d+)/priority)", mw->protect([rulectl](const httplib::Request& req,
                                                                           const AuthContext& ctx) {
        return rulectl->setPriority(req, ctx);
    }, model::Role::ADMIN));
    svr.Delete(R"(/api/v1/admin/rules/(\d+))", mw->protect([rulectl](const httplib::Request& req,
                                                                     const AuthContext& ctx) {
        return rulectl->remove(req, ctx);
    }, model::Role::ADMIN));

    // ---- 静态自测台 /debug
    const std::string debugRoot = resolveDebugRoot(sc);
    if (!debugRoot.empty()) {
        if (svr.set_mount_point("/debug", debugRoot)) {
            LOG_INFO("自测台已挂载：/debug → {}", debugRoot);
            impl_->debugMounted = true;
        } else {
            LOG_WARN("自测台挂载失败：{}", debugRoot);
        }
    } else {
        LOG_WARN("未找到 /debug 目录，自测台不可用（预期路径 {}）", sc.debugRoot);
    }
    // ---- 静态管理后台 /admin（P4 T12）
    const std::string adminRoot = resolveAdminRoot(sc);
    if (!adminRoot.empty()) {
        if (svr.set_mount_point("/admin", adminRoot)) {
            LOG_INFO("管理后台已挂载：/admin → {}", adminRoot);
            impl_->adminMounted = true;
        } else {
            LOG_WARN("管理后台挂载失败：{}", adminRoot);
        }
    } else {
        LOG_WARN("未找到管理后台产物（预期 {} 或 ./front/admin/dist），/admin 不可用；先执行 npm run build", sc.adminRoot);
    }
    if (impl_->adminMounted) {
        // 无尾斜杠也能进（否则 mount point 会按目录处理而 404）
        svr.Get("/admin", [](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_redirect("/admin/index.html");
        });
    }

    // ---- 静态用户端 H5 /h5（P5 T12）
    const std::string h5Root = resolveH5Root(sc);
    if (!h5Root.empty()) {
        if (svr.set_mount_point("/h5", h5Root)) {
            LOG_INFO("用户端 H5 已挂载：/h5 → {}", h5Root);
            impl_->h5Mounted = true;
        } else {
            LOG_WARN("用户端 H5 挂载失败：{}", h5Root);
        }
    } else {
        LOG_WARN("未找到用户端 H5 产物（预期 {} 或 ./front/h5/dist），/h5 不可用；先执行 npm run build",
                 sc.h5Root);
    }
    if (impl_->h5Mounted) {
        svr.Get("/h5", [](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_redirect("/h5/index.html");
        });
    }

    // 根路由：优先指向管理后台；产物缺失时退回自测台，避免演示时首屏 404
    svr.Get("/", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_redirect(impl_->adminMounted ? "/admin/" : "/debug/index.html");
    });

    // ---- 兜底：路由不存在 404/1006、方法不允许 405/1007
    // 用 HandlerWithResponse 形式：httplib 对所有 status>=400 的响应都会调用本处理器，
    // 若中间件已经写好 JSON 信封（res.body 非空）则返回 Unhandled，保持原响应体不被覆盖。
    svr.set_error_handler([](const httplib::Request& /*req*/,
                             httplib::Response& res) -> httplib::Server::HandlerResponse {
        if (!res.body.empty() || res.content_provider_) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        if (res.status == 405) {
            writeErrorBody(res, kMethodNotAllowed, "请求方法不允许");
        } else if (res.status == 404) {
            writeErrorBody(res, kRouteNotFound, "接口不存在");
        } else {
            writeErrorBody(res, kInternalError, "服务内部错误");
        }
        return httplib::Server::HandlerResponse::Handled;
    });

    // ---- 兜底：handler 抛出的异常 → 500 + 1008，进程存活
    svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res,
                                 std::exception_ptr ep) {
        std::string what = "未知异常";
        try {
            if (ep) {
                std::rethrow_exception(ep);
            }
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
            what = "非 std::exception 异常";
        }
        LOG_ERROR("rid={} action=UNCAUGHT_EXCEPTION path={} err={}", requestId(), req.path, what);
        res.status = 500;
        writeErrorBody(res, kInternalError, "服务内部错误");
    });
}

int Application::run() {
    if (!impl_) {
        return static_cast<int>(kInternalError);
    }
    const ServerConfig& sc = impl_->cfg.server;

    g_server           = &impl_->svr;
    g_stoppedBySignal  = 0;
    std::signal(SIGINT, onTerminateSignal);
    std::signal(SIGTERM, onTerminateSignal);

    LOG_INFO("HTTP 服务开始监听 http://{}:{}/api/v1（Ctrl+C 停止）", sc.host, sc.port);
    // 启动横幅：用 printf 而不是 LOG_INFO —— 保证在 CLion / 终端控制台必定可见
    // （LOG 只写文件）。**只打印实际挂载成功的入口**，未挂载的给出补救命令，不打印假地址。
    std::printf("========================================\n");
    std::printf(" ReserveFlow 已启动   端口 %d\n", sc.port);
    std::printf("========================================\n");
    if (impl_->h5Mounted) {
        std::printf("  学生端 H5     http://127.0.0.1:%d/h5/\n", sc.port);
    } else {
        std::printf("  学生端 H5     （未挂载：先 cd front/h5 && npm run build）\n");
    }
    if (impl_->adminMounted) {
        std::printf("  管理后台      http://127.0.0.1:%d/admin/\n", sc.port);
    } else {
        std::printf("  管理后台      （未挂载：先 cd front/admin && npm run build）\n");
    }
    if (impl_->debugMounted) {
        std::printf("  接口自测台    http://127.0.0.1:%d/debug/\n", sc.port);
    } else {
        std::printf("  接口自测台    （未挂载：缺少 web/debug/index.html）\n");
    }
    std::printf("  API 根路径    http://127.0.0.1:%d/api/v1\n", sc.port);
    std::printf("========================================\n");
    std::printf("  管理员   admin / Admin@123456\n");
    std::printf("  学生     student01 / User@123456\n");
    std::printf("  黑名单   student02 / Black@123456\n");
    std::printf("========================================\n");
    std::fflush(stdout);

    const bool ok = impl_->svr.listen(sc.host, sc.port);

    g_server = nullptr;
    if (!ok && g_stoppedBySignal == 0) {
        LOG_ERROR("HTTP 服务监听失败：{}:{}（端口被占用或无权限）", sc.host, sc.port);
        return static_cast<int>(kInternalError);
    }
    LOG_INFO("HTTP 服务已停止，进程正常退出");
    return 0;
}

void Application::stop() {
    if (impl_) {
        impl_->svr.stop();
        // P2：先停调度线程（维护任务还在借 Redis / DB），再停看门狗，最后析构 redis/pool。
        // 顺序不能反：任务线程还在跑时关闭 Redis 连接会踩已释放的连接池（ARCH-P2 §5.1）。
        if (impl_->scheduler) {
            impl_->scheduler->stop();
        }
        if (impl_->lockWatchdog) {
            impl_->lockWatchdog->stop();
        }
    }
}

} // namespace rf
