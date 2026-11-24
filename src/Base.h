// =============================================================================
// src/common/Base.h  —  时间 / 字符串 / JSON / 日志 工具集合（header-only）
// 对应 ARCH-P0 §3.2 / §8.6 / D-7
// 分层约束：本文件不得依赖项目内其他层（只依赖标准库 + 第三方）
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace rf {

/** 时间工具（D-7 / §8.3）：统一 yyyy-MM-ddTHH:mm:ss，本地时间，不带时区后缀与毫秒 */
struct TimeUtil {
    static constexpr const char* kIsoFmt = "%Y-%m-%dT%H:%M:%S";

    /** 当前本地时间字符串 "2026-01-01T10:00:00" */
    static std::string nowString() { return format(nowEpoch()); }

    /** time_t → "2026-01-01T10:00:00"（本地时区） */
    static std::string format(std::time_t t) {
        std::tm tmBuf{};
#ifdef _WIN32
        if (localtime_s(&tmBuf, &t) != 0) {
            return std::string();
        }
#else
        if (localtime_r(&t, &tmBuf) == nullptr) {
            return std::string();
        }
#endif
        char buf[32] = {0};
        if (std::strftime(buf, sizeof(buf), kIsoFmt, &tmBuf) == 0) {
            return std::string();
        }
        return std::string(buf);
    }

    /** 毫秒数 → "3.21"（日志耗时展示用） */
    static std::string formatMs(double ms) {
        char buf[32] = {0};
        std::snprintf(buf, sizeof(buf), "%.2f", ms);
        return std::string(buf);
    }

    /** 当前 epoch 秒 */
    static std::time_t nowEpoch() { return std::time(nullptr); }

    /**
     * 解析 "yyyy-MM-ddTHH:mm:ss"（同时容忍空格分隔与末尾 'Z'）。
     * @param s   输入字符串
     * @param out 成功时写入 epoch 秒
     * @return 解析失败返回 false
     */
    static bool parseIso(const std::string& s, std::time_t& out) {
        if (s.size() < 19) {
            return false;
        }
        // 允许日期与时间之间是 'T' 或空格：把下标 10 归一成空格后统一 sscanf
        // （MSVC 无 strptime，这里只用标准 C 函数，保证两平台行为一致）
        std::string buf = s.substr(0, 19);
        if (buf[10] == 'T') {
            buf[10] = ' ';
        }
        int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
        if (std::sscanf(buf.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
                        &year, &mon, &day, &hour, &min, &sec) != 6) {
            return false;
        }
        if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
            hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
            return false;
        }
        std::tm t{};
        t.tm_year  = year - 1900;
        t.tm_mon   = mon - 1;
        t.tm_mday  = day;
        t.tm_hour  = hour;
        t.tm_min   = min;
        t.tm_sec   = sec;
        t.tm_isdst = -1;   // 交给 mktime 按本地时区判定
        const std::time_t v = std::mktime(&t);
        if (v == static_cast<std::time_t>(-1)) {
            return false;
        }
        out = v;
        return true;
    }
};

/** 字符串工具 */
struct Str {
    /** 去掉首尾空白（空格 / \\t / \\r / \\n） */
    static std::string trim(std::string_view s) {
        size_t b = 0;
        size_t e = s.size();
        while (b < e && isSpace(static_cast<unsigned char>(s[b]))) {
            ++b;
        }
        while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) {
            --e;
        }
        return std::string(s.substr(b, e - b));
    }

    /** 按单字符分隔切分，保留空段（便于校验 "a,,b"） */
    static std::vector<std::string> split(std::string_view s, char sep) {
        std::vector<std::string> out;
        size_t start = 0;
        while (true) {
            const size_t pos = s.find(sep, start);
            if (pos == std::string_view::npos) {
                out.emplace_back(s.substr(start));
                break;
            }
            out.emplace_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return out;
    }

    /** 前缀判断 */
    static bool startsWith(std::string_view s, std::string_view p) {
        return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    }

    /** 转小写（仅 ASCII） */
    static std::string toLower(std::string_view s) {
        std::string out(s);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }

    /** 转大写（仅 ASCII） */
    static std::string toUpper(std::string_view s) {
        std::string out(s);
        for (char& c : out) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        return out;
    }

    /** 二进制 → 小写 hex */
    static std::string hexLower(const unsigned char* p, size_t n) {
        static const char* kDigits = "0123456789abcdef";
        std::string out;
        out.resize(n * 2);
        for (size_t i = 0; i < n; ++i) {
            out[i * 2]     = kDigits[(p[i] >> 4) & 0x0F];
            out[i * 2 + 1] = kDigits[p[i] & 0x0F];
        }
        return out;
    }

    /** 加密安全随机字节 → 小写 hex，用于 rid / jti（不走 crypto 层以保持依赖方向） */
    static std::string randomHex(size_t bytes) {
        std::vector<unsigned char> buf(bytes == 0 ? 1 : bytes, 0);
#ifdef _WIN32
        if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return fallbackRandomHex(bytes);
        }
#else
        return fallbackRandomHex(bytes);
#endif
        return hexLower(buf.data(), buf.size());
    }

private:
    static bool isSpace(unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    }

    /** CNG 不可用时的兜底：std::random_device / mt19937（仅用于非安全场景） */
    static std::string fallbackRandomHex(size_t bytes) {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::vector<unsigned char> buf(bytes == 0 ? 1 : bytes, 0);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = static_cast<unsigned char>(rng() & 0xFF);
        }
        return hexLower(buf.data(), buf.size());
    }
};

// ---------------------------------------------------------------- 请求关联 ID
/**
 * 每请求 rid（16 位 hex）的线程内存储。
 * 背景：httplib 一个请求独占一个工作线程，因此 thread_local 足以把中间件生成的 rid
 *       传给同一请求内的服务层，无需把 rid 塞进每一个业务方法签名（ARCH §8.6 要求
 *       每条日志与 AUDIT 行都带 rid）。
 */
inline std::string& requestIdStorage() {
    static thread_local std::string rid;
    return rid;
}

/** 由鉴权中间件在进入 handler 时写入 */
inline void setRequestId(const std::string& rid) { requestIdStorage() = rid; }

/** 服务层写日志时读取；未设置时返回空串 */
inline const std::string& requestId() { return requestIdStorage(); }

/**
 * nlohmann::json 安全取值：缺失或类型不符一律 nullopt，由 Controller 转 1000。
 * @note 命名说明：ARCH §3.2 原写 `struct Json`，但 §3.11 又要求
 *       `controller/Controllers.h` 内出现 `using Json = nlohmann::json;`，
 *       两者同名会在包含顺序不同的编译单元里冲突（重定义）。
 *       因此此处改名为 `JsonUtil`，语义不变；`Json` 名字留给 Controller 层的类型别名。
 */
struct JsonUtil {
    static std::optional<std::string> getStr(const nlohmann::json& j, const char* key) {
        if (!j.is_object()) {
            return std::nullopt;
        }
        const auto it = j.find(key);
        if (it == j.end() || it->is_null() || !it->is_string()) {
            return std::nullopt;
        }
        return it->get<std::string>();
    }

    static std::optional<int64_t> getI64(const nlohmann::json& j, const char* key) {
        if (!j.is_object()) {
            return std::nullopt;
        }
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) {
            return std::nullopt;
        }
        if (it->is_number_integer()) {
            return it->get<int64_t>();
        }
        if (it->is_number_unsigned()) {
            return static_cast<int64_t>(it->get<uint64_t>());
        }
        if (it->is_number_float()) {
            return static_cast<int64_t>(it->get<double>());
        }
        return std::nullopt;
    }

    static std::optional<double> getDbl(const nlohmann::json& j, const char* key) {
        if (!j.is_object()) {
            return std::nullopt;
        }
        const auto it = j.find(key);
        if (it == j.end() || it->is_null() || !it->is_number()) {
            return std::nullopt;
        }
        return it->get<double>();
    }

    static bool has(const nlohmann::json& j, const char* key) {
        return j.is_object() && j.find(key) != j.end() && !j.find(key)->is_null();
    }
};

/**
 * 日志级别字符串 → spdlog 级别，未知值回落 info（避免 from_str 返回 off 导致静默）。
 * @param level 级别名（大小写不敏感）
 */
inline spdlog::level::level_enum levelOf(const std::string& level) {
    const std::string v = Str::toLower(Str::trim(level));
    if (v == "trace")    return spdlog::level::trace;
    if (v == "debug")    return spdlog::level::debug;
    if (v == "warn" || v == "warning") return spdlog::level::warn;
    if (v == "error")    return spdlog::level::err;
    if (v == "critical") return spdlog::level::critical;
    if (v == "off")      return spdlog::level::off;
    return spdlog::level::info;
}

/**
 * 初始化全局日志（控制台彩色 + 滚动文件）。
 * @param level   trace/debug/info/warn/error/critical，未知值回落 info
 * @param file    日志文件路径，父目录不存在时自动创建
 * @param sizeMB  单文件上限（MB），<=0 时取 10
 * @param count   滚动文件数量，<=0 时取 5
 */
inline void initLogger(const std::string& level, const std::string& file, int sizeMB, int count) {
    constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e][%-5l][t:%t] %v";

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern(kPattern);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(consoleSink);

    std::string fileSinkError;   // 文件 sink 失败不阻塞启动，待 logger 建好后再告警
    if (!file.empty()) {
        try {
            const size_t slash = file.find_last_of("/\\");
            if (slash != std::string::npos) {
                const std::string dir = file.substr(0, slash);
                if (!dir.empty()) {
#ifdef _WIN32
                    ::CreateDirectoryA(dir.c_str(), nullptr);   // 已存在则返回失败，忽略
#else
                    std::string cmd = "mkdir -p '" + dir + "'";
                    (void)std::system(cmd.c_str());
#endif
                }
            }
            const size_t maxBytes = static_cast<size_t>(sizeMB > 0 ? sizeMB : 10) * 1024U * 1024U;
            const size_t maxFiles = static_cast<size_t>(count > 0 ? count : 5);
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file, maxBytes, maxFiles);
            fileSink->set_pattern(kPattern);
            sinks.push_back(fileSink);
        } catch (const std::exception& e) {
            fileSinkError = std::string("文件日志初始化失败，降级为仅控制台：") + e.what();
        }
    }

    auto logger = std::make_shared<spdlog::logger>("reserveflow", sinks.begin(), sinks.end());
    const auto lvl = levelOf(level);
    logger->set_level(lvl);
    logger->flush_on(spdlog::level::info);   // P0：及时落盘，保证 grep 可见
    spdlog::set_default_logger(logger);
    spdlog::set_level(lvl);
    if (!fileSinkError.empty()) {
        logger->warn("{}", fileSinkError);
    }
}

} // namespace rf

// ---- 日志宏（§8.6）：只在业务代码里用，避免各处手写 spdlog:: 前缀
#define LOG_TRACE(...) ::spdlog::trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::spdlog::debug(__VA_ARGS__)
#define LOG_INFO(...)  ::spdlog::info(__VA_ARGS__)
#define LOG_WARN(...)  ::spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) ::spdlog::error(__VA_ARGS__)

// 审计行：固定前缀 AUDIT，字段序固定 rid op role action code target，便于 grep
// 说明：code 显式转 long long —— fmt v10 的 consteval 检查不接受枚举类型实参，
//       统一在宏内收敛，避免每个调用点都手写 static_cast。
#define LOG_AUDIT(rid, op, role, action, code, target)                                  \
    ::spdlog::info("AUDIT rid={} op={} role={} action={} code={} target={}",            \
                   (rid), (op), (role), (action), static_cast<long long>(code), (target))
