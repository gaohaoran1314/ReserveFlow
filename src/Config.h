// =============================================================================
// src/config/Config.h  —  运行期配置（header-only）
// 对应 ARCH-P0 §3.3 / §8.9：工程常量只在 config.json 与本文件默认值一处定义
// =============================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "json.hpp"

namespace rf {

/** HTTP 服务段 */
struct ServerConfig {
    std::string host       = "0.0.0.0";
    int         port       = 8080;
    int         threadPool = 8;
    std::string debugRoot  = "./web/debug";
    /// 管理后台静态产物目录（P4 T12）；缺失时只 WARN，不影响服务启动
    std::string adminRoot  = "./front/admin/dist";
    /// 用户端 H5 静态产物目录（P5 T12）；缺失时只 WARN，不影响服务启动
    std::string h5Root     = "./front/h5/dist";
};

/** MySQL 段 */
struct MysqlConfig {
    std::string host              = "127.0.0.1";
    int         port              = 3306;
    std::string user;
    std::string password;
    std::string database;
    int         poolSize          = 8;
    std::string charset           = "utf8mb4";
    int         connectTimeoutSec = 5;
};

/**
 * Redis 段。
 * @note poolSize 的取值依据：**必须 >= 2 × server.threadPool**。
 *       一个请求可能连续用多次 Redis（鉴权黑名单校验 → 抢资源锁 → 单号 INCR），
 *       池子小于线程数时，压测下租借会排队到 timeoutMs 超时，命令直接返回
 *       1010「缓存访问失败」——用户看到的就不是「资源正忙」了。
 *       8 线程 → 16 条连接（实测 config.json 的 16 可完全吸收 100/200 并发）。
 */
struct RedisConfig {
    std::string host      = "127.0.0.1";
    int         port      = 6379;
    std::string password;
    int         timeoutMs = 1000;
    int         poolSize  = 16;   ///< >= 2 × server.threadPool，见上方说明
};

/** JWT 段（D-1：单令牌 8 小时，无 refresh token） */
struct JwtConfig {
    std::string secret;
    int         ttlSeconds = 28800;
    std::string issuer     = "reserveflow";
};

/** 日志段 */
struct LogConfig {
    std::string level        = "info";
    std::string file         = "./logs/reserveflow.log";
    int         rotateSizeMB = 10;
    int         rotateCount  = 5;
};

/**
 * 分布式锁段（ARCH-P1 §3.10 / D-P1-4）。
 * 硬约束：renewIntervalMs * 2 < ttlMs —— 否则看门狗还没续期锁就已过期。
 */
struct LockConfig {
    int ttlMs           = 30000;   // 锁 TTL
    int renewIntervalMs = 10000;   // 看门狗续期间隔（必须 < ttlMs/2）
    int waitMs          = 3000;    // 抢锁总等待上限
    int retryIntervalMs = 30;      // 重试间隔
};

/**
 * 业务特性开关段（ARCH-P1 §3.10 / D-P1-1 / §9-1）。
 * phase 是唯一需要改动的「批次门控」：P2 只需把它改成 2，不改一行代码。
 */
struct FeatureConfig {
    int phase          = 1;    // 1 = P1 启用范围；2 = 放开 IN_USE 及之后
    int minCreditScore = 60;   // 低于此信用分禁止预约（3011）；设 0 表示不拦
    /**
     * 规则引擎总开关（ARCH-P3 D-P3-6 安全阀）。
     * false → 引擎完全不介入，全部走 P2 硬编码路径，**行为与 P2 逐字节一致**。
     * 演示前发现引擎有问题，改这一个布尔值即可退回 P2。
     */
    bool ruleEngineEnabled = true;
};

/**
 * 信用体系段（ARCH-P3 §6 / D-P3-7）。
 * 迟滞区 [enter, exit)：进黑名单要 `score < enter`（**严格小于**），
 * 解除要 `score >= exit`；区间内保持原状态，防止临界反复横跳。
 */
struct CreditConfig {
    int blacklistEnterScore = 60;   ///< 严格小于该值 → 进黑名单（`<` 不是 `<=`！）
    int blacklistExitScore  = 80;   ///< 大于等于该值 → 解除黑名单
};

/** 规则缓存段（对应 config.json 的 "rule"；命名带 Cache 以免与 Rule 混淆） */
struct RuleCacheConfig {
    int cacheTtlSec = 5;   ///< 指纹校验周期（D-P3-10）
};

/** 取用时间窗（ARCH-P2 §3.11） */
struct PickupConfig {
    int earlyMin = 15;   ///< 可提前多少分钟取用（早于 → 3015）
    int lateMin  = 30;   ///< 超过开始时间多少分钟内仍可取用（晚于 → 3016）
};

/** 定时维护段（ARCH-P2 §3.11 / §5） */
struct MaintenanceConfig {
    bool enabled               = true; ///< 总开关：false 则不启动调度线程（仅保留手动触发接口）
    int intervalSec            = 60;   ///< 调度周期（秒），<=0 取 60
    int batchSize              = 50;   ///< 每类每轮最多处理单数
    int lockWaitMs             = 500;  ///< 定时任务抢锁等待上限（抢不到就跳过，不阻塞用户）
    int pendingReviewTtlMin    = 120;  ///< 待审核超时（分钟）
    int noShowDeductScore      = 10;   ///< 爽约扣分
    int overdueDeductScore     = 5;    ///< 逾期每周期扣分
    int overdueDeductPeriodMin = 60;   ///< 逾期扣分周期（分钟）
    int overdueDeductMaxTimes  = 5;    ///< 逾期最多扣几次（超过只停扣分，不迁移状态）
    int damageDeductScore      = 20;   ///< 损坏认定扣分（附录 B 示例值为 20）
    /** RETURN_CHECK 长时间无人验收的告警阈值（分钟）；只告警不改状态（D-P2-7 口径） */
    int returnCheckWarnMin     = 1440;
};

/** 押金规则段（ARCH-P2 §6） */
struct DepositConfig {
    double damageDeductRatio = 0.5;    ///< 损坏默认扣减比例（占冻结额）
    double damageDeductMax   = 200.0;  ///< 单次损坏扣减上限（元）
};

/** 候补段（ARCH-P2 §7） */
struct WaitlistConfig {
    int offerTtlMin = 15;   ///< offer 确认时限（分钟）
    int maxPerUser  = 3;    ///< 单用户同时有效候补上限（WAITING + OFFERED）
};

/** 全局配置聚合 */
struct Config {
    ServerConfig server;
    MysqlConfig  mysql;
    RedisConfig  redis;
    JwtConfig    jwt;
    LogConfig    log;
    LockConfig   lock;      // P1 增量（ARCH-P1 §3.10）
    FeatureConfig feature;  // P1 增量（ARCH-P1 §3.10 / D-P1-1）
    PickupConfig      pickup;       // P2 增量（ARCH-P2 §3.11）
    MaintenanceConfig maintenance;  // P2 增量
    DepositConfig     deposit;      // P2 增量
    WaitlistConfig    waitlist;     // P2 增量
    CreditConfig      credit;       // P3 增量（ARCH-P3 §7.6）
    RuleCacheConfig   rule;         // P3 增量

    /**
     * 加载并校验配置文件。
     * @param path 配置文件路径
     * @param out  成功时写入解析结果
     * @param err  失败时写入可读原因
     * @return 缺 jwt.secret 或 mysql 段（或 JSON 非法）返回 false
     */
    static bool load(const std::string& path, Config& out, std::string& err) {
        err.clear();
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) {
            err = "无法打开配置文件：" + path;
            return false;
        }
        nlohmann::json j;
        try {
            // ignore_comments=true：允许 config.json 写 // 与 /* */ 注释
            //（配置项取值依据写在使用点旁边，比只写在代码里更容易被运维看到）
            j = nlohmann::json::parse(ifs, nullptr, /*allow_exceptions=*/true,
                                      /*ignore_comments=*/true);
        } catch (const std::exception& e) {
            err = std::string("配置文件不是合法 JSON：") + e.what();
            return false;
        }
        if (!j.is_object()) {
            err = "配置文件根节点必须是 JSON 对象";
            return false;
        }

        // ---- server ----
        if (j.contains("server") && j["server"].is_object()) {
            const auto& s = j["server"];
            out.server.host       = strOf(s, "host", out.server.host);
            out.server.port       = intOf(s, "port", out.server.port);
            out.server.threadPool = std::max(1, intOf(s, "threadPool", out.server.threadPool));
            out.server.debugRoot  = strOf(s, "debugRoot", out.server.debugRoot);
            out.server.adminRoot  = strOf(s, "adminRoot", out.server.adminRoot);
            out.server.h5Root     = strOf(s, "h5Root", out.server.h5Root);
        }
        if (out.server.port < 1 || out.server.port > 65535) {
            err = "server.port 必须在 1..65535 之间";
            return false;
        }

        // ---- mysql（必需段）----
        if (!j.contains("mysql") || !j["mysql"].is_object()) {
            err = "配置缺少 mysql 段";
            return false;
        }
        {
            const auto& m = j["mysql"];
            out.mysql.host              = strOf(m, "host", out.mysql.host);
            out.mysql.port              = intOf(m, "port", out.mysql.port);
            out.mysql.user              = strOf(m, "user", out.mysql.user);
            out.mysql.password          = strOf(m, "password", out.mysql.password);
            out.mysql.database          = strOf(m, "database", out.mysql.database);
            out.mysql.poolSize          = std::max(1, intOf(m, "poolSize", out.mysql.poolSize));
            out.mysql.charset           = strOf(m, "charset", out.mysql.charset);
            out.mysql.connectTimeoutSec = std::max(1, intOf(m, "connectTimeoutSec", out.mysql.connectTimeoutSec));
            if (out.mysql.user.empty()) {
                err = "mysql.user 不能为空";
                return false;
            }
            if (out.mysql.database.empty()) {
                err = "mysql.database 不能为空";
                return false;
            }
        }

        // ---- redis ----
        if (j.contains("redis") && j["redis"].is_object()) {
            const auto& r = j["redis"];
            out.redis.host      = strOf(r, "host", out.redis.host);
            out.redis.port      = intOf(r, "port", out.redis.port);
            out.redis.password  = strOf(r, "password", out.redis.password);
            out.redis.timeoutMs = std::max(1, intOf(r, "timeoutMs", out.redis.timeoutMs));
            out.redis.poolSize  = std::max(1, intOf(r, "poolSize", out.redis.poolSize));
        }
        if (out.redis.poolSize > 64) {
            err = "redis.poolSize 过大（当前 " + std::to_string(out.redis.poolSize) +
                  "，上限 64）：应取 >= 2 × server.threadPool 且不要超过 Redis maxclients";
            return false;
        }

        // ---- jwt（secret 必需）----
        if (j.contains("jwt") && j["jwt"].is_object()) {
            const auto& t = j["jwt"];
            out.jwt.secret     = strOf(t, "secret", out.jwt.secret);
            out.jwt.ttlSeconds = intOf(t, "ttlSeconds", out.jwt.ttlSeconds);
            out.jwt.issuer     = strOf(t, "issuer", out.jwt.issuer);
        }
        if (out.jwt.secret.empty()) {
            err = "配置缺少 jwt.secret（HS256 签名密钥，不能为空）";
            return false;
        }
        if (out.jwt.ttlSeconds <= 0) {
            err = "jwt.ttlSeconds 必须为正数";
            return false;
        }

        // ---- log ----
        if (j.contains("log") && j["log"].is_object()) {
            const auto& l = j["log"];
            out.log.level        = strOf(l, "level", out.log.level);
            out.log.file         = strOf(l, "file", out.log.file);
            out.log.rotateSizeMB = std::max(1, intOf(l, "rotateSizeMB", out.log.rotateSizeMB));
            out.log.rotateCount  = std::max(1, intOf(l, "rotateCount", out.log.rotateCount));
        }

        // ---- lock（P1 增量；段缺失时沿用默认值，老 config.json 照样能跑）----
        if (j.contains("lock") && j["lock"].is_object()) {
            const auto& k = j["lock"];
            out.lock.ttlMs           = intOf(k, "ttlMs", out.lock.ttlMs);
            out.lock.renewIntervalMs = intOf(k, "renewIntervalMs", out.lock.renewIntervalMs);
            out.lock.waitMs          = intOf(k, "waitMs", out.lock.waitMs);
            out.lock.retryIntervalMs = intOf(k, "retryIntervalMs", out.lock.retryIntervalMs);
        }
        if (out.lock.ttlMs < 1000) {
            err = "lock.ttlMs 必须 >= 1000（当前 " + std::to_string(out.lock.ttlMs) + "）";
            return false;
        }
        if (out.lock.renewIntervalMs <= 0) {
            err = "lock.renewIntervalMs 必须为正数（当前 " +
                  std::to_string(out.lock.renewIntervalMs) + "）";
            return false;
        }
        if (out.lock.renewIntervalMs * 2 >= out.lock.ttlMs) {
            err = "lock.renewIntervalMs(" + std::to_string(out.lock.renewIntervalMs) +
                  ") 必须小于 lock.ttlMs(" + std::to_string(out.lock.ttlMs) +
                  ") 的一半，否则看门狗还没续期锁就已过期";
            return false;
        }
        if (out.lock.waitMs < 0) {
            err = "lock.waitMs 不能为负数（当前 " + std::to_string(out.lock.waitMs) + "）";
            return false;
        }
        if (out.lock.retryIntervalMs < 1) {
            err = "lock.retryIntervalMs 必须 >= 1（当前 " +
                  std::to_string(out.lock.retryIntervalMs) + "）";
            return false;
        }

        // ---- feature（P1 增量；phase 只能是 1 或 2，D-P1-1）----
        if (j.contains("feature") && j["feature"].is_object()) {
            const auto& f = j["feature"];
            out.feature.phase          = intOf(f, "phase", out.feature.phase);
            out.feature.minCreditScore = intOf(f, "minCreditScore", out.feature.minCreditScore);
            out.feature.ruleEngineEnabled =
                boolOf(f, "ruleEngineEnabled", out.feature.ruleEngineEnabled);
        }
        if (out.feature.phase != 1 && out.feature.phase != 2) {
            err = "feature.phase 只能是 1 或 2（当前 " + std::to_string(out.feature.phase) + "）";
            return false;
        }
        if (out.feature.minCreditScore < 0 || out.feature.minCreditScore > 100) {
            err = "feature.minCreditScore 必须在 0..100 之间（当前 " +
                  std::to_string(out.feature.minCreditScore) + "）";
            return false;
        }

        // ---- pickup（P2 增量；段缺失时沿用默认值）----
        if (j.contains("pickup") && j["pickup"].is_object()) {
            const auto& p = j["pickup"];
            out.pickup.earlyMin = intOf(p, "earlyMin", out.pickup.earlyMin);
            out.pickup.lateMin  = intOf(p, "lateMin", out.pickup.lateMin);
        }
        if (out.pickup.earlyMin < 0) {
            err = "pickup.earlyMin 不能为负数（当前 " + std::to_string(out.pickup.earlyMin) + "）";
            return false;
        }
        if (out.pickup.lateMin < 0) {
            err = "pickup.lateMin 不能为负数（当前 " + std::to_string(out.pickup.lateMin) + "）";
            return false;
        }

        // ---- maintenance（P2 增量）----
        if (j.contains("maintenance") && j["maintenance"].is_object()) {
            const auto& m = j["maintenance"];
            out.maintenance.enabled                = boolOf(m, "enabled", out.maintenance.enabled);
            out.maintenance.intervalSec            = intOf(m, "intervalSec", out.maintenance.intervalSec);
            out.maintenance.batchSize              = intOf(m, "batchSize", out.maintenance.batchSize);
            out.maintenance.lockWaitMs             = intOf(m, "lockWaitMs", out.maintenance.lockWaitMs);
            out.maintenance.pendingReviewTtlMin    = intOf(m, "pendingReviewTtlMin", out.maintenance.pendingReviewTtlMin);
            out.maintenance.noShowDeductScore      = intOf(m, "noShowDeductScore", out.maintenance.noShowDeductScore);
            out.maintenance.overdueDeductScore     = intOf(m, "overdueDeductScore", out.maintenance.overdueDeductScore);
            out.maintenance.overdueDeductPeriodMin = intOf(m, "overdueDeductPeriodMin", out.maintenance.overdueDeductPeriodMin);
            out.maintenance.overdueDeductMaxTimes  = intOf(m, "overdueDeductMaxTimes", out.maintenance.overdueDeductMaxTimes);
            out.maintenance.damageDeductScore      = intOf(m, "damageDeductScore", out.maintenance.damageDeductScore);
            out.maintenance.returnCheckWarnMin     = intOf(m, "returnCheckWarnMin", out.maintenance.returnCheckWarnMin);
        }
        if (out.maintenance.intervalSec < 5) {
            err = "maintenance.intervalSec 必须 >= 5 秒（当前 " +
                  std::to_string(out.maintenance.intervalSec) + "）";
            return false;
        }
        if (out.maintenance.batchSize < 1 || out.maintenance.batchSize > 500) {
            err = "maintenance.batchSize 必须在 1..500 之间（当前 " +
                  std::to_string(out.maintenance.batchSize) + "）";
            return false;
        }
        if (out.maintenance.lockWaitMs < 1) {
            err = "maintenance.lockWaitMs 必须 >= 1（当前 " +
                  std::to_string(out.maintenance.lockWaitMs) + "）";
            return false;
        }
        if (out.maintenance.pendingReviewTtlMin < 1) {
            err = "maintenance.pendingReviewTtlMin 必须 >= 1（当前 " +
                  std::to_string(out.maintenance.pendingReviewTtlMin) + "）";
            return false;
        }
        if (out.maintenance.overdueDeductPeriodMin < 1) {
            err = "maintenance.overdueDeductPeriodMin 必须 >= 1（当前 " +
                  std::to_string(out.maintenance.overdueDeductPeriodMin) + "）";
            return false;
        }
        if (out.maintenance.noShowDeductScore < 0 || out.maintenance.overdueDeductScore < 0 ||
            out.maintenance.damageDeductScore < 0) {
            err = "maintenance.noShowDeductScore / overdueDeductScore / damageDeductScore 不能为负数";
            return false;
        }
        if (out.maintenance.overdueDeductMaxTimes < 0) {
            err = "maintenance.overdueDeductMaxTimes 不能为负数（当前 " +
                  std::to_string(out.maintenance.overdueDeductMaxTimes) + "）";
            return false;
        }
        if (out.maintenance.returnCheckWarnMin < 1) {
            err = "maintenance.returnCheckWarnMin 必须 >= 1（当前 " +
                  std::to_string(out.maintenance.returnCheckWarnMin) + "）";
            return false;
        }

        // ---- deposit（P2 增量）----
        if (j.contains("deposit") && j["deposit"].is_object()) {
            const auto& d = j["deposit"];
            out.deposit.damageDeductRatio = dblOf(d, "damageDeductRatio", out.deposit.damageDeductRatio);
            out.deposit.damageDeductMax   = dblOf(d, "damageDeductMax", out.deposit.damageDeductMax);
        }
        if (out.deposit.damageDeductRatio < 0.0 || out.deposit.damageDeductRatio > 1.0) {
            err = "deposit.damageDeductRatio 必须在 0..1 之间（当前 " +
                  std::to_string(out.deposit.damageDeductRatio) + "）";
            return false;
        }
        if (out.deposit.damageDeductMax < 0.0) {
            err = "deposit.damageDeductMax 不能为负数（当前 " +
                  std::to_string(out.deposit.damageDeductMax) + "）";
            return false;
        }

        // ---- waitlist（P2 增量）----
        if (j.contains("waitlist") && j["waitlist"].is_object()) {
            const auto& w = j["waitlist"];
            out.waitlist.offerTtlMin = intOf(w, "offerTtlMin", out.waitlist.offerTtlMin);
            out.waitlist.maxPerUser  = intOf(w, "maxPerUser", out.waitlist.maxPerUser);
        }
        if (out.waitlist.offerTtlMin < 1) {
            err = "waitlist.offerTtlMin 必须 >= 1（当前 " +
                  std::to_string(out.waitlist.offerTtlMin) + "）";
            return false;
        }
        if (out.waitlist.maxPerUser < 1) {
            err = "waitlist.maxPerUser 必须 >= 1（当前 " +
                  std::to_string(out.waitlist.maxPerUser) + "）";
            return false;
        }

        // ---- credit（P3 增量：黑名单迟滞阈值，D-P3-7）----
        if (j.contains("credit") && j["credit"].is_object()) {
            const auto& c = j["credit"];
            out.credit.blacklistEnterScore =
                intOf(c, "blacklistEnterScore", out.credit.blacklistEnterScore);
            out.credit.blacklistExitScore =
                intOf(c, "blacklistExitScore", out.credit.blacklistExitScore);
        }
        if (out.credit.blacklistEnterScore < 0 || out.credit.blacklistExitScore > 100) {
            err = "credit.blacklistEnterScore / blacklistExitScore 必须在 0..100 之间";
            return false;
        }
        if (out.credit.blacklistEnterScore >= out.credit.blacklistExitScore) {
            // 迟滞区为负 → 会在阈值附近反复横跳，属于配置错误而非运行期可容忍
            err = "credit.blacklistEnterScore(" +
                  std::to_string(out.credit.blacklistEnterScore) +
                  ") 必须小于 credit.blacklistExitScore(" +
                  std::to_string(out.credit.blacklistExitScore) + ")，否则迟滞区为负会反复横跳";
            return false;
        }

        // ---- rule（P3 增量：规则缓存 TTL，D-P3-10）----
        if (j.contains("rule") && j["rule"].is_object()) {
            const auto& r = j["rule"];
            out.rule.cacheTtlSec = intOf(r, "cacheTtlSec", out.rule.cacheTtlSec);
        }
        if (out.rule.cacheTtlSec < 1) {
            err = "rule.cacheTtlSec 必须 >= 1（当前 " + std::to_string(out.rule.cacheTtlSec) + "）";
            return false;
        }

        return true;
    }

    /**
     * 依次尝试若干候选路径，返回第一个存在的 config.json；
     * 全部不存在时返回第一个候选（让 load 报出明确路径）。
     * @param argv0 main 的第一个参数，可为 nullptr
     */
    static std::string resolvePath(const char* argv0) {
        std::vector<std::string> candidates;
        const std::string dir = exeDir();
        if (!dir.empty()) {
            candidates.push_back(dir + "/config/config.json");
            candidates.push_back(dir + "/../config/config.json");
            candidates.push_back(dir + "/../../config/config.json");
        }
        candidates.push_back("./config/config.json");
        candidates.push_back("../config/config.json");
        candidates.push_back("../../config/config.json");
        if (argv0 != nullptr && argv0[0] != '\0') {
            candidates.push_back(std::string(argv0) + ".config/config.json");
        }

        for (const auto& c : candidates) {
            std::ifstream probe(c, std::ios::binary);
            if (probe.is_open()) {
                return c;
            }
        }
        return candidates.empty() ? std::string("./config/config.json") : candidates.front();
    }

private:
    /** 可执行文件所在目录（末尾不带分隔符）；失败返回空串 */
    static std::string exeDir() {
#ifdef _WIN32
        // _pgmptr 由 MSVC CRT 在启动时填好，值为可执行文件全路径，免去 windows.h 依赖
        const char* p = _pgmptr;
        if (p == nullptr) {
            return std::string();
        }
        std::string full(p);
        const size_t slash = full.find_last_of("/\\");
        if (slash == std::string::npos) {
            return std::string();
        }
        return full.substr(0, slash);
#else
        return std::string();
#endif
    }

    /** 取字符串字段，缺失或类型不符时用默认值 */
    static std::string strOf(const nlohmann::json& j, const char* key, const std::string& def) {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null() || !it->is_string()) {
            return def;
        }
        return it->get<std::string>();
    }

    /** 取整数字段，缺失或类型不符时用默认值 */
    static int intOf(const nlohmann::json& j, const char* key, int def) {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null() || !it->is_number()) {
            return def;
        }
        return static_cast<int>(it->get<double>());
    }

    /**
     * 取布尔字段。
     * **必须同时接受 JSON bool 与数字 0/1**：config.json 里写的是 `"enabled": true`，
     * 若只认 is_number()，布尔会被当成「类型不符」而静默回落到默认值——
     * 这正是 P3 安全阀踩过的坑（`ruleEngineEnabled: false` 被忽略，引擎一直是开着的）。
     */
    static bool boolOf(const nlohmann::json& j, const char* key, bool def) {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) {
            return def;
        }
        if (it->is_boolean()) {
            return it->get<bool>();
        }
        if (it->is_number()) {
            return it->get<double>() != 0.0;
        }
        return def;
    }

    /** 取浮点字段（P2 增量：押金比例/上限），缺失或类型不符时用默认值 */
    static double dblOf(const nlohmann::json& j, const char* key, double def) {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null() || !it->is_number()) {
            return def;
        }
        return it->get<double>();
    }
};

} // namespace rf
