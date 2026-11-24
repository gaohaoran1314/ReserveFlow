// =============================================================================
// src/cache/RedisClient.cpp  —  hiredis 封装实现
// 对应 ARCH-P0 §3.5 / §8.7
// 关键点：
//   1) hiredis 1.2.0 的 net.c 在 Windows 下**不**调用 WSAStartup，必须自行初始化；
//   2) 连接池容量用尽时最多等待 timeoutMs，超时返回 kCacheError（绝不死等）；
//   3) 建连失败后进入 1 秒冷却（degraded_），避免 Redis 长时间不可用时每次请求都白等
//      一个 TCP 超时 —— 鉴权 fail-open 路径依赖这一点保证接口延迟。
// =============================================================================
#include "RedisClient.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#endif

#include "Base.h"

namespace rf {

namespace {

/** 进程内只做一次 Winsock 初始化（hiredis 1.2.0 不代劳） */
void ensureWinsock() {
    static std::once_flag once;
    std::call_once(once, []() {
#ifdef _WIN32
        WSADATA wsa{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            LOG_ERROR("WSAStartup 失败，rc={}", rc);
        }
#endif
    });
}

/** 毫秒 → timeval */
struct timeval msToTimeval(int ms) {
    struct timeval tv;
    const int safe = ms > 0 ? ms : 1000;
    tv.tv_sec  = safe / 1000;
    tv.tv_usec = (safe % 1000) * 1000;
    return tv;
}

/** 建连失败后的冷却窗口（毫秒）：期间不再尝试 TCP 建连，快速失败 */
constexpr int64_t kDegradeCooldownMs = 1000;

/** 单调时钟毫秒 */
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** reply 摘要（错误信息用，避免把整包数据打进日志） */
std::string replySummary(const redisReply* r) {
    if (r == nullptr) {
        return "空响应";
    }
    switch (r->type) {
        case REDIS_REPLY_ERROR:
            return r->str != nullptr ? std::string(r->str) : std::string("命令返回错误");
        case REDIS_REPLY_STATUS:
            return r->str != nullptr ? std::string(r->str) : std::string("状态响应");
        case REDIS_REPLY_NIL:
            return "nil";
        default:
            return "非预期响应类型 " + std::to_string(static_cast<int>(r->type));
    }
}

}  // namespace

// ============================================================================
// Conn / Lease
// ============================================================================

RedisClient::Conn::~Conn() {
    if (ctx != nullptr) {
        ::redisFree(ctx);
        ctx = nullptr;
    }
}

/** 连接租借句柄：析构时归还（坏连接直接销毁） */
struct RedisClient::Lease {
    Lease() = default;
    Lease(RedisClient* owner, std::unique_ptr<Conn> c)
        : owner_(owner), conn_(std::move(c)) {}
    Lease(Lease&& other) noexcept
        : owner_(other.owner_), conn_(std::move(other.conn_)), healthy_(other.healthy_) {
        other.owner_ = nullptr;
    }
    Lease(const Lease&)            = delete;
    Lease& operator=(const Lease&) = delete;
    Lease& operator=(Lease&&)      = delete;
    ~Lease();

    Conn* operator->() const { return conn_.get(); }
    Conn& operator*() const { return *conn_; }
    explicit operator bool() const { return conn_ != nullptr; }
    /** 标记为坏连接：析构时不回收 */
    void markBroken() { healthy_ = false; }

    RedisClient*          owner_   = nullptr;
    std::unique_ptr<Conn> conn_;
    bool                  healthy_ = true;
};

RedisClient::Lease::~Lease() {
    if (owner_ != nullptr && conn_) {
        owner_->recycle(std::move(conn_), healthy_);
    }
}

// ============================================================================
// RedisClient
// ============================================================================

RedisClient::RedisClient(const RedisConfig& cfg) : cfg_(cfg) {}

RedisClient::~RedisClient() {
    shutdown();
}

std::unique_ptr<RedisClient::Conn> RedisClient::makeConn(std::string& err) {
    ensureWinsock();

    auto conn = std::make_unique<Conn>();
    const struct timeval tv = msToTimeval(cfg_.timeoutMs);
    redisContext* ctx = ::redisConnectWithTimeout(cfg_.host.c_str(), cfg_.port, tv);
    if (ctx == nullptr) {
        err = "redisConnect 分配失败（内存不足）";
        return nullptr;
    }
    if (ctx->err != 0) {
        err = "连接 Redis 失败（" + cfg_.host + ":" + std::to_string(cfg_.port) + "）：" +
              (ctx->errstr[0] != '\0' ? std::string(ctx->errstr) : std::string("未知错误"));
        ::redisFree(ctx);
        return nullptr;
    }

    // 命令级超时：避免 Redis 卡死时同步阻塞线程池
    ::redisSetTimeout(ctx, tv);

    if (!cfg_.password.empty()) {
        auto* reply = static_cast<redisReply*>(
            ::redisCommand(ctx, "AUTH %s", cfg_.password.c_str()));
        const bool ok = (reply != nullptr && reply->type == REDIS_REPLY_STATUS);
        if (!ok) {
            err = "Redis AUTH 失败：" + replySummary(reply);
            if (reply != nullptr) {
                ::freeReplyObject(reply);
            }
            ::redisFree(ctx);
            return nullptr;
        }
        ::freeReplyObject(reply);
    }

    conn->ctx = ctx;
    return conn;
}

bool RedisClient::init(std::string& err) {
    err.clear();
    ensureWinsock();

    std::vector<std::unique_ptr<Conn>> built;
    const int want = cfg_.poolSize > 0 ? cfg_.poolSize : 4;
    for (int i = 0; i < want; ++i) {
        std::string openErr;
        auto c = makeConn(openErr);
        if (!c) {
            err = "第 " + std::to_string(i + 1) + " 条连接建立失败：" + openErr;
            built.clear();
            {
                std::lock_guard<std::mutex> lk(mu_);
                degraded_     = true;
                lastFailTick_ = nowMs();
            }
            return false;
        }
        built.push_back(std::move(c));
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& c : built) {
            idle_.push_back(std::move(c));
        }
        total_    = idle_.size();
        degraded_ = false;
    }
    LOG_INFO("RedisClient 初始化完成：{} 条连接 → {}:{}", total_, cfg_.host, cfg_.port);
    return true;
}

void RedisClient::shutdown() {
    std::vector<std::unique_ptr<Conn>> released;
    bool alreadyStopped = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        alreadyStopped = stopped_;
        stopped_ = true;
        released.swap(idle_);
        total_ = 0;
    }
    cv_.notify_all();
    released.clear();   // 锁外释放 socket
    if (!alreadyStopped) {
        LOG_INFO("RedisClient 已关闭");   // 析构里会再调一次 shutdown，避免重复日志
    }
}

void RedisClient::recycle(std::unique_ptr<Conn> c, bool healthy) {
    if (!c) {
        return;
    }
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_ || !healthy) {
            total_ = total_ > 0 ? total_ - 1 : 0;
            dropped = true;
        } else {
            idle_.push_back(std::move(c));
        }
    }
    if (dropped) {
        c.reset();          // 锁外销毁 socket
    }
    cv_.notify_one();
}

RedisClient::Lease RedisClient::lease(int timeoutMs) {
    ensureWinsock();
    std::unique_lock<std::mutex> lk(mu_);

    if (stopped_) {
        return Lease();
    }
    if (!idle_.empty()) {
        auto c = std::move(idle_.back());
        idle_.pop_back();
        return Lease(this, std::move(c));
    }
    if (total_ < static_cast<size_t>(cfg_.poolSize > 0 ? cfg_.poolSize : 4)) {
        // 冷却期内不重试建连：Redis 明确不可用时快速失败，避免拖慢接口
        if (degraded_ && (nowMs() - lastFailTick_) < kDegradeCooldownMs) {
            return Lease();
        }
        ++total_;
        lk.unlock();
        std::string err;
        auto c = makeConn(err);
        if (!c) {
            {
                std::lock_guard<std::mutex> g(mu_);
                total_        = total_ > 0 ? total_ - 1 : 0;
                degraded_     = true;
                lastFailTick_ = nowMs();
            }
            cv_.notify_one();
            return Lease();
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            degraded_ = false;
        }
        return Lease(this, std::move(c));
    }

    // 池已满且无空闲：有限等待
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
    if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
        if (idle_.empty()) {
            return Lease();
        }
    }
    if (idle_.empty() || stopped_) {
        return Lease();
    }
    auto c = std::move(idle_.back());
    idle_.pop_back();
    return Lease(this, std::move(c));
}

Result<redisReply*> RedisClient::command(const std::vector<std::string>& args, Lease& l) {
    if (!l || l->ctx == nullptr) {
        return Result<redisReply*>::fail(kCacheError, "缓存访问失败：连接不可用");
    }
    if (args.empty()) {
        return Result<redisReply*>::fail(kCacheError, "缓存访问失败：命令为空");
    }

    std::vector<const char*> argv(args.size(), nullptr);
    std::vector<size_t>      argvlen(args.size(), 0);
    for (size_t i = 0; i < args.size(); ++i) {
        argv[i]    = args[i].data();
        argvlen[i] = args[i].size();
    }

    auto* reply = static_cast<redisReply*>(
        ::redisCommandArgv(l->ctx, static_cast<int>(args.size()), argv.data(), argvlen.data()));
    if (reply == nullptr) {
        const std::string why =
            (l->ctx->errstr[0] != '\0') ? std::string(l->ctx->errstr) : std::string("未知错误");
        l.markBroken();   // socket 已不可用，归还时销毁
        return Result<redisReply*>::fail(kCacheError, "缓存访问失败：" + why);
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        const std::string why = replySummary(reply);
        ::freeReplyObject(reply);
        return Result<redisReply*>::fail(kCacheError, "缓存访问失败：" + why);
    }
    return Result<redisReply*>::make(reply);
}

bool RedisClient::ping(int* latencyMs, std::string& err) {
    err.clear();
    if (latencyMs != nullptr) {
        *latencyMs = -1;
    }
    const auto t0 = std::chrono::steady_clock::now();

    Lease leaseGuard = lease(cfg_.timeoutMs);
    if (!leaseGuard) {
        err = "无法连接 Redis（" + cfg_.host + ":" + std::to_string(cfg_.port) + "）";
        return false;
    }
    auto r = command({"PING"}, leaseGuard);
    if (!r.ok()) {
        err = r.msg;
        return false;
    }
    redisReply* reply = r.data;
    const bool pong = (reply->type == REDIS_REPLY_STATUS && reply->str != nullptr &&
                       std::strcmp(reply->str, "PONG") == 0);
    ::freeReplyObject(reply);
    if (!pong) {
        err = "Redis PING 响应非预期";
        return false;
    }

    const auto t1 = std::chrono::steady_clock::now();
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    if (latencyMs != nullptr) {
        *latencyMs = ms;
    }
    return true;
}

Result<std::optional<std::string>> RedisClient::get(const std::string& key) {
    Lease l = lease(cfg_.timeoutMs);
    auto r = command({"GET", key}, l);
    if (!r.ok()) {
        return Result<std::optional<std::string>>::fail(r.code, r.msg);
    }
    redisReply* reply = r.data;
    Result<std::optional<std::string>> out = [&]() {
        if (reply->type == REDIS_REPLY_NIL) {
            return Result<std::optional<std::string>>::make(std::optional<std::string>{});
        }
        if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS) {
            const std::string v = (reply->str != nullptr)
                                      ? std::string(reply->str, reply->len)
                                      : std::string();
            return Result<std::optional<std::string>>::make(std::optional<std::string>(v));
        }
        return Result<std::optional<std::string>>::fail(
            kCacheError, "缓存访问失败：" + replySummary(reply));
    }();
    ::freeReplyObject(reply);
    return out;
}

Result<Empty> RedisClient::set(const std::string& key, const std::string& val) {
    Lease l = lease(cfg_.timeoutMs);
    auto r = command({"SET", key, val}, l);
    if (!r.ok()) {
        return Result<Empty>::fail(r.code, r.msg);
    }
    redisReply* reply = r.data;
    const bool ok = (reply->type == REDIS_REPLY_STATUS);
    const std::string why = replySummary(reply);
    ::freeReplyObject(reply);
    if (!ok) {
        return Result<Empty>::fail(kCacheError, "缓存访问失败：" + why);
    }
    return Result<Empty>::make(Empty{});
}

Result<Empty> RedisClient::setEx(const std::string& key, const std::string& val, int ttlSeconds) {
    const int ttl = ttlSeconds > 0 ? ttlSeconds : 1;
    Lease l = lease(cfg_.timeoutMs);
    auto r = command({"SETEX", key, std::to_string(ttl), val}, l);
    if (!r.ok()) {
        return Result<Empty>::fail(r.code, r.msg);
    }
    redisReply* reply = r.data;
    const bool ok = (reply->type == REDIS_REPLY_STATUS);
    const std::string why = replySummary(reply);
    ::freeReplyObject(reply);
    if (!ok) {
        return Result<Empty>::fail(kCacheError, "缓存访问失败：" + why);
    }
    return Result<Empty>::make(Empty{});
}

Result<bool> RedisClient::exists(const std::string& key) {
    Lease l = lease(cfg_.timeoutMs);
    auto r = command({"EXISTS", key}, l);
    if (!r.ok()) {
        return Result<bool>::fail(r.code, r.msg);
    }
    redisReply* reply = r.data;
    const bool hit = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    const bool valid = (reply->type == REDIS_REPLY_INTEGER);
    const std::string why = replySummary(reply);
    ::freeReplyObject(reply);
    if (!valid) {
        return Result<bool>::fail(kCacheError, "缓存访问失败：" + why);
    }
    return Result<bool>::make(hit);
}

Result<int64_t> RedisClient::del(const std::string& key) {
    Lease l = lease(cfg_.timeoutMs);
    auto r = command({"DEL", key}, l);
    if (!r.ok()) {
        return Result<int64_t>::fail(r.code, r.msg);
    }
    redisReply* reply = r.data;
    const bool valid = (reply->type == REDIS_REPLY_INTEGER);
    const int64_t n = valid ? static_cast<int64_t>(reply->integer) : 0;
    const std::string why = replySummary(reply);
    ::freeReplyObject(reply);
    if (!valid) {
        return Result<int64_t>::fail(kCacheError, "缓存访问失败：" + why);
    }
    return Result<int64_t>::make(n);
}

Result<Empty> RedisClient::expire(const std::string& key, int ttlSeconds) {
    const int ttl = ttlSeconds > 0 ? ttlSeconds : 1;
    Lease l = lease(cfg_.timeoutMs);
    auto r = command({"EXPIRE", key, std::to_string(ttl)}, l);
    if (!r.ok()) {
        return Result<Empty>::fail(r.code, r.msg);
    }
    redisReply* reply = r.data;
    const bool valid = (reply->type == REDIS_REPLY_INTEGER);
    const std::string why = replySummary(reply);
    ::freeReplyObject(reply);
    if (!valid) {
        return Result<Empty>::fail(kCacheError, "缓存访问失败：" + why);
    }
    return Result<Empty>::make(Empty{});
}

Result<bool> RedisClient::setNxPx(const std::string& key, const std::string& val, int ttlMs) {
    const int ttl = ttlMs > 0 ? ttlMs : 1;
    Lease l = lease(cfg_.timeoutMs);
    auto r = command({"SET", key, val, "NX", "PX", std::to_string(ttl)}, l);
    if (!r.ok()) {
        return Result<bool>::fail(r.code, r.msg);
    }
    redisReply* reply = r.data;
    bool acquired = false;
    bool valid = true;
    if (reply->type == REDIS_REPLY_NIL) {
        acquired = false;   // 已被别人持有
    } else if (reply->type == REDIS_REPLY_STATUS && reply->str != nullptr &&
               std::strcmp(reply->str, "OK") == 0) {
        acquired = true;
    } else {
        valid = false;
    }
    const std::string why = replySummary(reply);
    ::freeReplyObject(reply);
    if (!valid) {
        return Result<bool>::fail(kCacheError, "缓存访问失败：" + why);
    }
    return Result<bool>::make(acquired);
}

Result<int64_t> RedisClient::eval(const std::string& script,
                                  const std::vector<std::string>& keys,
                                  const std::vector<std::string>& args) {
    std::vector<std::string> cmd;
    cmd.reserve(3 + keys.size() + args.size());
    cmd.emplace_back("EVAL");
    cmd.push_back(script);
    cmd.push_back(std::to_string(keys.size()));
    for (const auto& k : keys) {
        cmd.push_back(k);
    }
    for (const auto& a : args) {
        cmd.push_back(a);
    }

    Lease l = lease(cfg_.timeoutMs);
    auto r = command(cmd, l);
    if (!r.ok()) {
        return Result<int64_t>::fail(r.code, r.msg);
    }
    redisReply* reply = r.data;
    int64_t value = 0;
    bool valid = true;
    if (reply->type == REDIS_REPLY_NIL) {
        value = 0;
    } else if (reply->type == REDIS_REPLY_INTEGER) {
        value = static_cast<int64_t>(reply->integer);
    } else if (reply->type == REDIS_REPLY_STRING && reply->str != nullptr) {
        value = static_cast<int64_t>(std::strtoll(reply->str, nullptr, 10));
    } else {
        valid = false;
    }
    const std::string why = replySummary(reply);
    ::freeReplyObject(reply);
    if (!valid) {
        return Result<int64_t>::fail(kCacheError, "缓存访问失败：" + why);
    }
    return Result<int64_t>::make(value);
}

} // namespace rf
