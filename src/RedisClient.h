// =============================================================================
// src/cache/RedisClient.h  —  hiredis 连接池化客户端
// 对应 ARCH-P0 §3.5 / §1.3 / §8.7
// 并发模型：redisContext 非线程安全 → 池化 + mutex/condition_variable
// 降级约定（§8.7）：Redis 不可用时**所有**命令返回 kCacheError(1010)，进程不退出、
//                  不抛异常；恢复后下一次调用自动重连回可用状态。
// =============================================================================
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// hiredis 的 sds.h 使用「结构体末尾零长数组」这一 MSVC 非标准扩展，
// 在 /W3 下会刷 C4200；此处局部屏蔽，不改动第三方源码
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200)
#endif
#include "hiredis.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "Result.h"
#include "Config.h"

namespace rf {

/** 连接池化 Redis 客户端 */
class RedisClient {
public:
    explicit RedisClient(const RedisConfig& cfg);
    ~RedisClient();
    RedisClient(const RedisClient&)            = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    /**
     * 预建 poolSize 条连接。
     * @param err 失败原因（Redis 未启动时也会返回 false，但对象仍可用：后续命令返回 1010）
     * @return 至少有一条连接可用时返回 true
     */
    bool init(std::string& err);
    void shutdown();

    /**
     * 探针：PING → +PONG。
     * @param latencyMs 成功时写入耗时（毫秒），失败写 -1
     * @param err       失败时写入可读原因
     */
    bool ping(int* latencyMs, std::string& err);

    // ---- 基础命令（均为 Result，不可用时返回 kCacheError）----
    Result<std::optional<std::string>> get(const std::string& key);
    Result<Empty>   set(const std::string& key, const std::string& val);
    Result<Empty>   setEx(const std::string& key, const std::string& val, int ttlSeconds);
    Result<bool>    exists(const std::string& key);
    Result<int64_t> del(const std::string& key);
    Result<Empty>   expire(const std::string& key, int ttlSeconds);

    // ---- P1 分布式锁预留（P0 实现，P0 不调用）----
    /** SET key val NX PX ttlMs：抢锁成功返回 true */
    Result<bool> setNxPx(const std::string& key, const std::string& val, int ttlMs);

    /** EVAL script numkeys key... arg... */
    Result<int64_t> eval(const std::string& script,
                         const std::vector<std::string>& keys,
                         const std::vector<std::string>& args);

    /** 比较并删除的原子释放锁脚本 */
    static constexpr const char* kUnlockScript =
        "if redis.call('get',KEYS[1])==ARGV[1] then return redis.call('del',KEYS[1]) else return 0 end";

private:
    /** 单条连接（redisContext 包装，析构即释放） */
    class Conn {
    public:
        Conn() = default;
        ~Conn();
        Conn(const Conn&)            = delete;
        Conn& operator=(const Conn&) = delete;
        redisContext* ctx = nullptr;
    };

    struct Lease;   // 从池借出 → 析构归还（定义在 .cpp）

    Lease lease(int timeoutMs = 1000);
    /** 归还连接；healthy=false 时直接销毁（坏连接不回收） */
    void recycle(std::unique_ptr<Conn> c, bool healthy);
    /** 新建并校验一条连接；失败返回 nullptr 并写 err */
    std::unique_ptr<Conn> makeConn(std::string& err);
    /** 执行一条命令；失败返回 kCacheError。成功时调用方负责 freeReplyObject */
    Result<redisReply*> command(const std::vector<std::string>& args, Lease& l);
    /** 统一错误构造 */
    static Result<Empty> cacheFail(const std::string& detail);

    RedisConfig                          cfg_;
    std::mutex                           mu_;
    std::condition_variable              cv_;
    std::vector<std::unique_ptr<Conn>>   idle_;
    size_t                               total_        = 0;
    bool                                 stopped_      = false;
    bool                                 degraded_     = false;   // 最近一次建连失败
    int64_t                              lastFailTick_ = 0;       // 上次建连失败时刻（ms）
};

} // namespace rf
