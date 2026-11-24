// =============================================================================
// src/DistributedLock.h  —  自研分布式锁（SET NX PX）+ 看门狗续期
// 对应 ARCH-P1 §3.5 / §5.2 / §5.3 / §5.4 / D-P1-3 / D-P1-4
//
// 三个角色：
//   DistributedLock  抢锁器：SET NX PX + 固定间隔重试，总等待 <= waitMs
//   LockWatchdog     后台线程：每 tickMs 扫注册表，对到期的锁执行续期 Lua
//   LockHandle       RAII 句柄：析构顺序严格是 detach(看门狗) → Lua 释放
//
// 两条硬约束（违反会出生产事故）：
//   1) D-P1-3：锁**不是**正确性保证，唯一索引才是。提交前必须查 lock.lost()，
//      为真则回滚并返回 3014；
//   2) §5.4：释放顺序必须是「先 detach，再 unlock」。反过来会让看门狗把别人
//      刚拿到的锁续成 30s，形成死锁。
//
// 线程安全：DistributedLock 可在多线程并发调用；LockHandle 是「单线程持有、
// 看门狗跨线程置位 lost」的模型，lost_ 用 shared_ptr<atomic<bool>> 共享。
// =============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "RedisClient.h"
#include "Result.h"

namespace rf {

class LockWatchdog;   // 前向

/**
 * 锁句柄：RAII。析构依次做 detach(看门狗) → Lua 释放。
 * move-only；`lost()` 由看门狗跨线程置位。
 */
class LockHandle {
public:
    LockHandle() = default;
    ~LockHandle();
    LockHandle(LockHandle&&) noexcept;
    LockHandle& operator=(LockHandle&&) noexcept;
    LockHandle(const LockHandle&)            = delete;
    LockHandle& operator=(const LockHandle&) = delete;

    bool               valid() const;      ///< 仍持有且未丢失
    bool               lost() const;       ///< 看门狗判定已丢失（D-P1-3 提交前必须查）
    const std::string& key() const;
    const std::string& token() const;      ///< owner uuid
    void               unlock();           ///< 提前释放；幂等

private:
    friend class DistributedLock;
    friend class LockWatchdog;

    RedisClient*                      redis_ = nullptr;
    LockWatchdog*                     wd_    = nullptr;
    std::string                       key_;
    std::string                       token_;
    std::shared_ptr<std::atomic<bool>> lost_;
    uint64_t                          handleId_ = 0;   ///< 看门狗注册表 id，0 表示未注册
    bool                              released_ = false;
};

/**
 * 看门狗：后台线程每 tickMs(1s) 扫描注册表，对「距上次续期 >= renewIntervalMs」的锁
 * 执行续期 Lua；返回非 1 说明锁已不属于自己 → 置 *lost = true。
 *
 * 生命周期约束：必须在 RedisClient 之前析构（Application::Impl 的成员声明顺序
 * 为 redis → watchdog → lock，析构反序执行，已在 ARCH-P1 §1.4 标注）。
 */
class LockWatchdog {
public:
    LockWatchdog(RedisClient& redis, int renewIntervalMs = 10000, int ttlMs = 30000);
    ~LockWatchdog();
    LockWatchdog(const LockWatchdog&)            = delete;
    LockWatchdog& operator=(const LockWatchdog&) = delete;

    void start();
    /** 置停止标志 → 唤醒线程 → join。Application::stop() 必须先调它 */
    void stop();

    /** 注册一把锁，返回注册 id */
    uint64_t attach(const std::string& key, const std::string& token, int ttlMs,
                    std::shared_ptr<std::atomic<bool>> lostFlag);
    void     detach(uint64_t id);

    /** 立即扫一遍（单测用，避免等 1s） */
    void tickOnce();

    size_t activeCount() const;

private:
    struct Entry {
        std::string                        key;
        std::string                        token;
        int                                ttlMs = 30000;
        int64_t                            lastRenewMs = 0;
        std::shared_ptr<std::atomic<bool>> lost;
    };
    void          loop();
    /** 收集到期的注册项，出锁后再逐个续期（Redis 调用期间不持锁） */
    void          tick();
    static int64_t nowMs();

    RedisClient&                          redis_;
    int                                   tickMs_            = 1000;
    int                                   renewIntervalMs_   = 10000;
    int                                   ttlMs_             = 30000;
    mutable std::mutex                    mu_;
    std::condition_variable               cv_;
    std::unordered_map<uint64_t, Entry>   entries_;
    uint64_t                              nextId_ = 1;
    std::atomic<bool>                     running_{false};
    std::thread                           thread_;
};

/** 抢锁器 */
class DistributedLock {
public:
    /**
     * @param redis            已 init 的 Redis 客户端（锁操作与业务共用连接池）
     * @param wd               已 start 的看门狗（未 start 时锁不会续期，仅靠 TTL 兜底）
     * @param retryIntervalMs  抢锁失败的重试间隔；<=0 时取 30（来自 config.lock.retryIntervalMs）
     */
    DistributedLock(RedisClient& redis, LockWatchdog& wd, int retryIntervalMs = 30);

    /**
     * 抢某资源的锁：key = "rf:lock:resource:{resourceId}"。
     * SET NX PX ttlMs；失败按 retryIntervalMs 间隔重试，总等待 <= waitMs。
     *
     * 重要（D-P1-4）：本方法内**不持有**任何 DB 连接，调用方必须在 acquire 成功之后
     * 才去 DbPool::acquire —— 否则 8 个线程会各自占着连接等锁，形成全局死锁。
     *
     * 重试语义（P2 修订）：抢锁失败**不区分**「锁被别人占」与「Redis 瞬时错误」，
     * 两者都在 waitMs 窗口内按 retryIntervalMs 重试。窗口耗尽后按证据分流：
     *   · 期间至少收到过一次「已被别人持有」→ kResourceBusy(3004)（正常的资源竞争）
     *   · 全程只有 Redis 错误             → kCacheError(1010)（真故障，如实上报，不掩盖）
     * 这样连接池拥塞会被重试吸收，用户看到的是「资源正忙」而不是「缓存访问失败」。
     *
     * @param resourceId      资源 id（必须 > 0）
     * @param waitMs          总等待上限，默认 3000
     * @param ttlMs           锁 TTL，默认 30000
     * @return 成功给 LockHandle；竞争超时 → 3004；Redis 真故障 → 1010
     */
    Result<LockHandle> acquireResource(int64_t resourceId, int waitMs = 3000, int ttlMs = 30000);

    static std::string resourceKey(int64_t resourceId);   // "rf:lock:resource:4"

    int retryIntervalMs() const { return retryIntervalMs_; }

private:
    RedisClient&  redis_;
    LockWatchdog& wd_;
    int           retryIntervalMs_ = 30;
};

} // namespace rf
