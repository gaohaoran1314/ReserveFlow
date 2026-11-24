// =============================================================================
// src/DistributedLock.cpp  —  SET NX PX 抢锁 / Lua 释放与续期 / 看门狗线程
// 对应 ARCH-P1 §3.5 / §5.2 / §5.3 / §5.4
//
// 全部 Redis 交互只用到 P0 已有的 setNxPx / eval（D-P1-5：不给 RedisClient 加 incr
// 之类的新命令）。三段 Lua 见 ARCH-P1 §1.6，本文件用其中的「解锁」与「续期」两段。
// =============================================================================
#include "DistributedLock.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf {

namespace {

/**
 * 续期 Lua（ARCH-P1 §1.6 第 3 段，照抄勿改）：
 * owner 匹配才 PEXPIRE；返回 1 = 续期成功，0 = 锁已过期或被他人持有。
 */
constexpr const char* kRenewScript =
    "if redis.call('get',KEYS[1])==ARGV[1] then "
    "return redis.call('pexpire',KEYS[1],ARGV[2]) else return 0 end";

}  // namespace

// ============================================================================
// LockHandle
// ============================================================================

LockHandle::~LockHandle() {
    unlock();
}

LockHandle::LockHandle(LockHandle&& other) noexcept
    : redis_(other.redis_),
      wd_(other.wd_),
      key_(std::move(other.key_)),
      token_(std::move(other.token_)),
      lost_(std::move(other.lost_)),
      handleId_(other.handleId_),
      released_(other.released_) {
    // 被移动对象必须变成「不可再释放」的空壳，否则它的析构会把新持有者的锁删掉
    other.redis_    = nullptr;
    other.wd_       = nullptr;
    other.handleId_ = 0;
    other.released_ = true;
}

LockHandle& LockHandle::operator=(LockHandle&& other) noexcept {
    if (this != &other) {
        unlock();   // 先释放自己持有的锁（幂等）
        redis_    = other.redis_;
        wd_       = other.wd_;
        key_      = std::move(other.key_);
        token_    = std::move(other.token_);
        lost_     = std::move(other.lost_);
        handleId_ = other.handleId_;
        released_ = other.released_;

        other.redis_    = nullptr;
        other.wd_       = nullptr;
        other.handleId_ = 0;
        other.released_ = true;
    }
    return *this;
}

bool LockHandle::valid() const {
    return redis_ != nullptr && !released_ && !lost();
}

bool LockHandle::lost() const {
    return lost_ != nullptr && lost_->load();
}

const std::string& LockHandle::key() const { return key_; }

const std::string& LockHandle::token() const { return token_; }

void LockHandle::unlock() {
    if (released_) {
        return;   // 幂等：析构 / 提前释放 / move 后的空壳都走这里
    }
    released_ = true;   // 先置位，防止重入

    // ① 先 detach（§5.4）：停掉续期，避免释放后被看门狗「复活」成别人的锁
    if (wd_ != nullptr && handleId_ != 0) {
        wd_->detach(handleId_);
        handleId_ = 0;
    }
    // ② 再比对 owner 后删除
    if (redis_ != nullptr && !key_.empty() && !token_.empty()) {
        auto r = redis_->eval(RedisClient::kUnlockScript, {key_}, {token_});
        if (!r.ok()) {
            // Redis 故障时锁只能等 TTL 自然过期；这里只告警，不抛异常也不阻塞退出
            LOG_WARN("释放资源锁失败（等待 TTL 自然过期）：key={} reason={}", key_, r.msg);
        } else if (r.data == 0) {
            // 返回 0 说明锁已过期或已被他人持有：绝不能删别人的锁，这正是 owner 校验的意义
            LOG_WARN("释放资源锁时 owner 不匹配（锁已过期或被他人持有）：key={}", key_);
        }
    }
    redis_ = nullptr;
}

// ============================================================================
// LockWatchdog
// ============================================================================

LockWatchdog::LockWatchdog(RedisClient& redis, int renewIntervalMs, int ttlMs)
    : redis_(redis),
      renewIntervalMs_(renewIntervalMs > 0 ? renewIntervalMs : 10000),
      ttlMs_(ttlMs > 0 ? ttlMs : 30000) {}

LockWatchdog::~LockWatchdog() {
    stop();
}

void LockWatchdog::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;   // 已在运行
    }
    if (thread_.joinable()) {
        thread_.join();   // 上一轮已 stop 但尚未 join 的残留
    }
    thread_ = std::thread([this]() { loop(); });
    LOG_INFO("锁看门狗已启动：tick={}ms renew={}ms ttl={}ms", tickMs_, renewIntervalMs_, ttlMs_);
}

void LockWatchdog::stop() {
    running_.store(false);
    cv_.notify_all();
    // 防御：若从看门狗线程自身调用 stop()，join 自己会永久阻塞，故跳过 join
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();   // 线程体内唯一的阻塞点是 wait_for，notify 后最多再跑一轮 tick
    }
}

uint64_t LockWatchdog::attach(const std::string& key, const std::string& token, int ttlMs,
                              std::shared_ptr<std::atomic<bool>> lostFlag) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t id = nextId_++;
    Entry e;
    e.key         = key;
    e.token       = token;
    e.ttlMs       = ttlMs > 0 ? ttlMs : ttlMs_;
    e.lastRenewMs = nowMs();
    e.lost        = std::move(lostFlag);
    entries_[id]  = std::move(e);
    return id;
}

void LockWatchdog::detach(uint64_t id) {
    if (id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    entries_.erase(id);
}

void LockWatchdog::tickOnce() { tick(); }

size_t LockWatchdog::activeCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

int64_t LockWatchdog::nowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void LockWatchdog::loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (running_.load()) {
        cv_.wait_for(lk, std::chrono::milliseconds(tickMs_));
        if (!running_.load()) {
            break;
        }
        lk.unlock();
        tick();          // Redis 调用期间不持注册表锁
        lk.lock();
    }
}

void LockWatchdog::tick() {
    struct Job {
        uint64_t                           id;
        std::string                        key;
        std::string                        token;
        int                                ttlMs;
        std::shared_ptr<std::atomic<bool>> lost;
    };

    std::vector<Job> jobs;
    const int64_t    now = nowMs();
    {
        std::lock_guard<std::mutex> lk(mu_);
        jobs.reserve(entries_.size());
        for (auto& kv : entries_) {
            const Entry& e = kv.second;
            if (e.lost != nullptr && e.lost->load()) {
                continue;   // 已判定丢失：只等请求侧 detach，不再重试
            }
            if (now - e.lastRenewMs < static_cast<int64_t>(renewIntervalMs_)) {
                continue;   // 还没到续期时刻
            }
            jobs.push_back(Job{kv.first, e.key, e.token, e.ttlMs, e.lost});
        }
    }

    for (const Job& j : jobs) {
        auto r = redis_.eval(kRenewScript, {j.key}, {j.token, std::to_string(j.ttlMs)});
        if (r.ok() && r.data == 1) {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = entries_.find(j.id);
            if (it != entries_.end()) {
                it->second.lastRenewMs = nowMs();
            }
            continue;
        }
        // 返回 0 → 锁已过期或被他人持有；Redis 出错 → 无法确认持有，同样按丢失处理（fail-closed）
        if (j.lost != nullptr) {
            j.lost->store(true);
        }
        const std::string why =
            r.ok() ? std::string("owner 不匹配或锁已过期") : ("Redis 访问失败：" + r.msg);
        LOG_WARN("锁续期失败，判定丢失：key={}（{}）", j.key, why);
    }
}

// ============================================================================
// DistributedLock
// ============================================================================

DistributedLock::DistributedLock(RedisClient& redis, LockWatchdog& wd, int retryIntervalMs)
    : redis_(redis), wd_(wd), retryIntervalMs_(retryIntervalMs > 0 ? retryIntervalMs : 30) {}

std::string DistributedLock::resourceKey(int64_t resourceId) {
    return "rf:lock:resource:" + std::to_string(resourceId);
}

Result<LockHandle> DistributedLock::acquireResource(int64_t resourceId, int waitMs, int ttlMs) {
    if (resourceId <= 0) {
        return Result<LockHandle>::fail(kInvalidParam, "参数校验失败：资源 id 必须为正整数");
    }
    const std::string key   = resourceKey(resourceId);
    const std::string token = Str::randomHex(16);   // owner uuid：每次抢锁唯一
    const int         ttl   = ttlMs > 0 ? ttlMs : 30000;
    const int         wait  = waitMs > 0 ? waitMs : 0;
    const int         retry = retryIntervalMs_ > 0 ? retryIntervalMs_ : 30;

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(wait);

    // 进入循环前的两个观察量，用于「窗口耗尽后」区分真故障与正常忙：
    //   sawBusy    —— 至少收到过一次「锁被别人持有」的确定答复
    //   cacheErrs  —— 期间收到过多少次 Redis 层错误（1010，典型是连接池租借超时）
    // 结论：只要见过一次 busy，窗口耗尽就是 3004；全程只有 Redis 错误才是真故障 → 1010。
    bool        sawBusy     = false;
    int         cacheErrs   = 0;   // 累计（仅用于日志）
    int         consecErrs  = 0;   // 连续次数：一旦拿到确定答复就清零
    int         attempts    = 0;
    std::string lastCacheMsg;

    // 真故障不该让用户等满 waitMs：连续这么多次都拿不到确定答复就直接判 1010。
    // 5 次 × retryIntervalMs(30ms) ≈ 150ms，既能吸收「池子瞬间打满」，又不会在
    // Redis 宕机时把每个请求都拖成 3 秒。反过来说，只要中途出现过一次 busy 答复，
    // 就说明 Redis 是活的，走窗口耗尽 → 3004。
    constexpr int kMaxConsecutiveCacheErr = 5;

    while (true) {
        ++attempts;
        auto r = redis_.setNxPx(key, token, ttl);
        if (r.ok() && r.data) {
            LockHandle h;
            h.redis_    = &redis_;
            h.wd_       = &wd_;
            h.key_      = key;
            h.token_    = token;
            h.lost_     = std::make_shared<std::atomic<bool>>(false);
            h.handleId_ = wd_.attach(key, token, ttl, h.lost_);
            if (cacheErrs > 0) {
                // 池拥塞被重试吸收：说明扩池/重试生效，但保留一条 WARN 便于发现容量不足
                LOG_WARN("资源锁获取期间遇到 {} 次缓存错误后成功（第 {} 次尝试）：key={}",
                         cacheErrs, attempts, key);
            }
            LOG_DEBUG("已获取资源锁：key={} token={} ttl={}ms", key, token, ttl);
            return Result<LockHandle>::make(std::move(h));
        }
        if (r.ok()) {
            sawBusy    = true;   // Redis 明确答复「已被别人持有」
            consecErrs = 0;      // 拿到确定答复 → 说明 Redis 活着，重置连续错误计数
        } else {
            // 瞬时错误（连接池租借超时 / 连接抖动）：**不立刻失败**，在 waitMs 窗口内重试。
            // 压测下池子瞬间打满很常见，一次失败就返回 1010 会让用户看到「缓存故障」，
            // 而实际只是并发太高。真故障（Redis 停了）会在窗口耗尽后如实返回 1010。
            ++cacheErrs;
            ++consecErrs;
            lastCacheMsg = r.msg;
            LOG_DEBUG("抢锁遇到缓存错误（累计 {} 次 / 连续 {} 次，在 {}ms 窗口内重试）：key={} err={}",
                      cacheErrs, consecErrs, wait, key, lastCacheMsg);
            if (consecErrs >= kMaxConsecutiveCacheErr) {
                // 真故障快速失败：不再等满窗口，也不伪装成「资源正忙」
                LOG_ERROR("获取资源锁失败：连续 {} 次缓存错误（共 {} 次尝试），判定 Redis 不可用：key={} err={}",
                          consecErrs, attempts, key, lastCacheMsg);
                return Result<LockHandle>::fail(
                    kCacheError, "缓存访问失败，无法获取资源锁（连续 " + std::to_string(consecErrs) +
                                     " 次尝试均失败）：" + lastCacheMsg);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const int64_t remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     deadline - now)
                                     .count();
        int sleepMs = retry;
        if (remainMs < static_cast<int64_t>(sleepMs)) {
            sleepMs = static_cast<int>(remainMs);
        }
        if (sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }

    if (sawBusy) {
        LOG_WARN("获取资源锁超时（等待 {}ms，{} 次尝试）：key={}", wait, attempts, key);
        return Result<LockHandle>::fail(kResourceBusy, "资源正忙，请稍后重试");
    }
    // 全程没拿到过任何确定答复 → 这是 Redis 真故障，绝不能伪装成「资源正忙」
    LOG_ERROR("获取资源锁失败：{}ms 内 {} 次尝试全部遇到缓存错误：key={} err={}", wait, attempts,
              key, lastCacheMsg);
    return Result<LockHandle>::fail(kCacheError,
                                    "缓存访问失败，无法获取资源锁（" + std::to_string(attempts) +
                                        " 次尝试均失败）：" + lastCacheMsg);
}

} // namespace rf
