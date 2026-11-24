// =============================================================================
// src/Scheduler.cpp  —  周期调度线程实现（ARCH-P2 §3.1 / §5.1）
//
// 线程体只有一个阻塞点：cv_.wait_for(intervalSec_)。
// 因此 stop() 的 notify + join 最多等一轮「正在执行的 tick」，不会卡住退出。
// =============================================================================
#include "Scheduler.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

#include "Base.h"

namespace rf {

Scheduler::Scheduler(std::string name, int intervalSec, Job job)
    : name_(std::move(name)),
      intervalSec_(intervalSec > 0 ? intervalSec : 60),
      job_(std::move(job)) {}

Scheduler::~Scheduler() {
    stop();
}

std::string Scheduler::makeTickId(const std::string& name, uint64_t seq) {
    const std::time_t now = TimeUtil::nowEpoch();
    std::string stamp     = TimeUtil::format(now);   // yyyy-MM-ddTHH:mm:ss
    for (char& c : stamp) {
        if (c == '-' || c == ':' || c == 'T') {
            c = '\0';   // 先占位，后面统一过滤
        }
    }
    std::string compact;
    compact.reserve(stamp.size());
    for (const char c : stamp) {
        if (c != '\0') {
            compact.push_back(c);
        }
    }
    return name + "-" + compact + "-" + std::to_string(seq);
}

void Scheduler::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;   // 已在运行
    }
    if (thread_.joinable()) {
        thread_.join();   // 上一轮已 stop 但还没 join 的残留
    }
    thread_ = std::thread([this]() { loop(); });
    LOG_INFO("调度器已启动：name={} interval={}s", name_, intervalSec_);
}

void Scheduler::stop() {
    running_.store(false);
    cv_.notify_all();
    // 防御：从调度线程自身调用 stop() 时 join 自己会永久阻塞
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

bool Scheduler::runOnce(const std::string& tickId) {
    if (!job_) {
        return false;
    }
    // 非阻塞 try_lock：已有 tick 在执行 → 立刻返回 false（→ 3022 维护任务正在执行）
    std::unique_lock<std::mutex> run(runMu_, std::try_to_lock);
    if (!run.owns_lock()) {
        LOG_WARN("调度器 {} 手动触发被拒绝：上一轮仍在执行（tickId={}）", name_, tickId);
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    try {
        job_(tickId);
    } catch (const std::exception& e) {
        LOG_ERROR("调度器 {} tick {} 抛异常（已吞掉，线程继续）：{}", name_, tickId, e.what());
    } catch (...) {
        LOG_ERROR("调度器 {} tick {} 抛未知异常（已吞掉，线程继续）", name_, tickId);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    LOG_INFO("调度器 {} tick {} 完成，耗时 {}ms", name_, tickId, static_cast<long long>(ms));
    return true;
}

void Scheduler::loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (running_.load()) {
        cv_.wait_for(lk, std::chrono::seconds(intervalSec_));
        if (!running_.load()) {
            break;
        }
        lk.unlock();
        const uint64_t seq = tickCount_.fetch_add(1) + 1;
        (void)runOnce(makeTickId(name_, seq));   // 与手动触发互斥；忙则跳过本轮
        lk.lock();
    }
}

} // namespace rf
