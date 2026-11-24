// =============================================================================
// src/Scheduler.h  —  通用周期调度线程（ARCH-P2 §3.1 / §5.1 / D-P2-14）
//
// 只负责「按时触发 + 不重入 + 干净退出」，不关心任务内容。
// 三条保证：
//   1) 不重入：runMu_ 让「后台 tick」与「手动 runOnce」互斥，慢 tick 不会被叠加；
//   2) tick 抛异常不杀线程（catch(...) 记 error 后继续下一轮）；
//   3) stop() 用 cv 唤醒 + join，最长等待 1 个 tickMs，不会卡住进程退出。
//
// 生命周期约束（与 LockWatchdog 同款）：必须在 RedisClient **之前**析构，
// 因为任务会借 Redis 连接。Application::Impl 成员声明顺序：
//   redis → lockWatchdog → distributedLock → scheduler → maintenanceJob
// =============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rf {

class Scheduler {
public:
    /** 任务签名：tickId 仅用于日志串联（形如 "maintenance-20260210120000-7"） */
    using Job = std::function<void(const std::string& tickId)>;

    /**
     * @param name        任务名（日志用，如 "maintenance"）
     * @param intervalSec 周期秒数（<=0 时取 60）
     * @param job         到点执行的回调（内部已加 runMu_，无需自行防重入）
     */
    Scheduler(std::string name, int intervalSec, Job job);
    ~Scheduler();
    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    /** 置停止标志 → 唤醒 → join。Application::stop() 必须先于 watchdog.stop() 调用 */
    void stop();
    bool running() const { return running_.load(); }

    /**
     * 手动执行一次（供 ADMIN 调试接口 / 单测）。
     * 与后台 tick 互斥；已有 tick 在执行时 **非阻塞返回 false**（→ 3022）。
     * @return true 表示本次真的执行了
     */
    bool runOnce(const std::string& tickId);

    int                intervalSec() const { return intervalSec_; }
    uint64_t           tickCount() const { return tickCount_.load(); }
    const std::string& name() const { return name_; }

private:
    void               loop();
    static std::string makeTickId(const std::string& name, uint64_t seq);

    std::string            name_;
    int                    intervalSec_ = 60;
    Job                    job_;
    std::thread            thread_;
    mutable std::mutex     mu_;
    std::condition_variable cv_;
    std::atomic<bool>      running_{false};
    std::atomic<uint64_t>  tickCount_{0};
    std::mutex             runMu_;   ///< 串行化 tick 与手动触发（try_lock 语义）
};

} // namespace rf
