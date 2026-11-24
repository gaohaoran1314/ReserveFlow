// =============================================================================
// src/security/CryptoUtil.h  —  CNG（bcrypt.dll）密码学原语（header-only）
// 对应 ARCH-P0 §3.6 / §8.9
// 实现要点（§1.4.6）：
//   HMAC  → BCryptOpenAlgorithmProvider(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)
//   PBKDF2→ BCryptDeriveKeyPBKDF2
//   随机  → BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)
// 无状态、纯函数：每次调用新建 handle，可被多线程并发调用。
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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

namespace rf::crypto {

// ------------------------------------------------------------------ 工程常量（§8.9）
inline constexpr uint32_t kPbkdf2Iters = 120000;   ///< PBKDF2 迭代轮数
inline constexpr size_t   kSaltBytes   = 16;       ///< 盐长度（字节）
inline constexpr size_t   kDkBytes     = 32;       ///< 派生密钥长度（字节）
inline constexpr const char* kHashPrefix = "pbkdf2_sha256";

namespace detail {

/// 最近一次 CNG 错误文本（每线程一份，避免加锁）
inline thread_local std::string g_lastError;

inline void setErr(const std::string& what, long status = 0) {
    char buf[64] = {0};
    if (status != 0) {
        std::snprintf(buf, sizeof(buf), " (ntstatus=0x%08lX)", static_cast<unsigned long>(status));
    }
    g_lastError = what + buf;
}

#ifdef _WIN32
/// BCRYPT 算法句柄 RAII
class AlgHandle {
public:
    AlgHandle() = default;
    ~AlgHandle() {
        if (h_ != nullptr) {
            ::BCryptCloseAlgorithmProvider(h_, 0);
        }
    }
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;

    bool open(LPCWSTR algId, ULONG flags) {
        const NTSTATUS st = ::BCryptOpenAlgorithmProvider(&h_, algId, nullptr, flags);
        if (!BCRYPT_SUCCESS(st)) {
            detail::setErr("BCryptOpenAlgorithmProvider 失败", st);
            h_ = nullptr;
            return false;
        }
        return true;
    }
    BCRYPT_ALG_HANDLE get() const { return h_; }

private:
    BCRYPT_ALG_HANDLE h_ = nullptr;
};

/// 哈希句柄 RAII
class HashHandle {
public:
    HashHandle() = default;
    ~HashHandle() {
        if (h_ != nullptr) {
            ::BCryptDestroyHash(h_);
        }
    }
    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
    BCRYPT_HASH_HANDLE* addr() { return &h_; }
    BCRYPT_HASH_HANDLE get() const { return h_; }

private:
    BCRYPT_HASH_HANDLE h_ = nullptr;
};

/// 查算法属性（OBJECT_LENGTH / HASH_LENGTH）
inline bool getPropU32(BCRYPT_ALG_HANDLE hAlg, LPCWSTR prop, DWORD& out) {
    DWORD cb = 0;
    const NTSTATUS st = ::BCryptGetProperty(hAlg, prop, reinterpret_cast<PUCHAR>(&out),
                                            sizeof(out), &cb, 0);
    if (!BCRYPT_SUCCESS(st)) {
        detail::setErr("BCryptGetProperty 失败", st);
        return false;
    }
    return true;
}
#endif  // _WIN32

// ---- base64 编解码（标准字母表，带 padding；密码哈希用） ----
inline const char* stdAlphabet() { return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; }
inline const char* urlAlphabet() { return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"; }

inline std::string base64EncodeImpl(std::string_view bin, const char* alphabet, bool pad) {
    std::string out;
    const size_t n = bin.size();
    out.reserve((n + 2) / 3 * 4 + 4);
    size_t i = 0;
    while (i + 3 <= n) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(bin[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(bin[i + 1])) << 8) |
                           static_cast<uint32_t>(static_cast<unsigned char>(bin[i + 2]));
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >> 6) & 0x3F]);
        out.push_back(alphabet[v & 0x3F]);
        i += 3;
    }
    const size_t rest = n - i;
    if (rest == 1) {
        const uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(bin[i])) << 16;
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        if (pad) {
            out.push_back('=');
            out.push_back('=');
        }
    } else if (rest == 2) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(bin[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(bin[i + 1])) << 8);
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >> 6) & 0x3F]);
        if (pad) {
            out.push_back('=');
        }
    }
    return out;
}

/// 宽容解码：同时接受标准与 url 字母表，忽略空白与 padding
inline bool base64DecodeImpl(std::string_view txt, std::string& out) {
    int8_t table[256];
    for (int i = 0; i < 256; ++i) {
        table[i] = -1;
    }
    const char* std_a = stdAlphabet();
    const char* url_a = urlAlphabet();
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(std_a[i])] = static_cast<int8_t>(i);
        table[static_cast<unsigned char>(url_a[i])] = static_cast<int8_t>(i);
    }

    out.clear();
    out.reserve(txt.size() / 4 * 3 + 3);
    uint32_t buf = 0;
    int bits = 0;
    for (const char ch : txt) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (ch == '=' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            continue;
        }
        const int8_t v = table[uc];
        if (v < 0) {
            detail::setErr("base64 解码遇到非法字符");
            return false;
        }
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

}  // namespace detail

// ------------------------------------------------------------------ 摘要 / 随机
/**
 * SHA-256 摘要。
 * @param data 任意字节串
 * @return 32 字节二进制；失败返回空串（原因见 lastError()）
 */
inline std::string sha256(std::string_view data) {
#ifdef _WIN32
    detail::AlgHandle alg;
    if (!alg.open(BCRYPT_SHA256_ALGORITHM, 0)) {
        return std::string();
    }
    DWORD objLen = 0;
    DWORD hashLen = 0;
    if (!detail::getPropU32(alg.get(), BCRYPT_OBJECT_LENGTH, objLen) ||
        !detail::getPropU32(alg.get(), BCRYPT_HASH_LENGTH, hashLen)) {
        return std::string();
    }
    std::vector<UCHAR> obj(objLen == 0 ? 1 : objLen, 0);
    detail::HashHandle hHash;
    NTSTATUS st = ::BCryptCreateHash(alg.get(), hHash.addr(), obj.data(), objLen, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(st)) {
        detail::setErr("BCryptCreateHash 失败", st);
        return std::string();
    }
    if (!data.empty()) {
        st = ::BCryptHashData(hHash.get(), reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                              static_cast<ULONG>(data.size()), 0);
        if (!BCRYPT_SUCCESS(st)) {
            detail::setErr("BCryptHashData 失败", st);
            return std::string();
        }
    }
    std::string out(hashLen, '\0');
    st = ::BCryptFinishHash(hHash.get(), reinterpret_cast<PUCHAR>(out.data()), hashLen, 0);
    if (!BCRYPT_SUCCESS(st)) {
        detail::setErr("BCryptFinishHash 失败", st);
        return std::string();
    }
    return out;
#else
    (void)data;
    detail::setErr("当前平台未实现 CNG");
    return std::string();
#endif
}

/**
 * HMAC-SHA256。
 * @param key  密钥
 * @param data 消息
 * @return 32 字节二进制；失败返回空串
 */
inline std::string hmacSha256(std::string_view key, std::string_view data) {
#ifdef _WIN32
    detail::AlgHandle alg;
    if (!alg.open(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)) {
        return std::string();
    }
    DWORD objLen = 0;
    DWORD hashLen = 0;
    if (!detail::getPropU32(alg.get(), BCRYPT_OBJECT_LENGTH, objLen) ||
        !detail::getPropU32(alg.get(), BCRYPT_HASH_LENGTH, hashLen)) {
        return std::string();
    }
    std::vector<UCHAR> obj(objLen == 0 ? 1 : objLen, 0);
    detail::HashHandle hHash;
    NTSTATUS st = ::BCryptCreateHash(alg.get(), hHash.addr(), obj.data(), objLen,
                                     reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                     static_cast<ULONG>(key.size()), 0);
    if (!BCRYPT_SUCCESS(st)) {
        detail::setErr("BCryptCreateHash(HMAC) 失败", st);
        return std::string();
    }
    if (!data.empty()) {
        st = ::BCryptHashData(hHash.get(), reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                              static_cast<ULONG>(data.size()), 0);
        if (!BCRYPT_SUCCESS(st)) {
            detail::setErr("BCryptHashData(HMAC) 失败", st);
            return std::string();
        }
    }
    std::string out(hashLen, '\0');
    st = ::BCryptFinishHash(hHash.get(), reinterpret_cast<PUCHAR>(out.data()), hashLen, 0);
    if (!BCRYPT_SUCCESS(st)) {
        detail::setErr("BCryptFinishHash(HMAC) 失败", st);
        return std::string();
    }
    return out;
#else
    (void)key;
    (void)data;
    detail::setErr("当前平台未实现 CNG");
    return std::string();
#endif
}

/** SHA-256 → 64 位小写 hex（Redis 失效名单 key 用，§8.9） */
inline std::string sha256Hex(std::string_view data) {
    const std::string bin = sha256(data);
    if (bin.empty()) {
        return std::string();
    }
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.resize(bin.size() * 2);
    for (size_t i = 0; i < bin.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(bin[i]);
        out[i * 2]     = kDigits[(b >> 4) & 0x0F];
        out[i * 2 + 1] = kDigits[b & 0x0F];
    }
    return out;
}

/**
 * 加密安全随机字节。
 * @param n 字节数
 * @return n 字节随机数据；失败返回空 vector
 */
inline std::vector<uint8_t> randomBytes(size_t n) {
    std::vector<uint8_t> buf(n, 0);
    if (n == 0) {
        return buf;
    }
#ifdef _WIN32
    const NTSTATUS st = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buf.data()),
                                          static_cast<ULONG>(buf.size()),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) {
        detail::setErr("BCryptGenRandom 失败", st);
        return std::vector<uint8_t>();
    }
    return buf;
#else
    detail::setErr("当前平台未实现 CNG");
    return std::vector<uint8_t>();
#endif
}

/**
 * PBKDF2-HMAC-SHA256。
 * @param pwd        明文口令
 * @param salt       盐
 * @param iterations 迭代轮数
 * @param dkLen      派生密钥长度
 * @return dkLen 字节派生密钥；失败返回空 vector
 */
inline std::vector<uint8_t> pbkdf2Sha256(std::string_view pwd,
                                        const std::vector<uint8_t>& salt,
                                        uint32_t iterations, size_t dkLen) {
    if (iterations == 0 || dkLen == 0) {
        detail::setErr("PBKDF2 参数非法（iterations/dkLen 不能为 0）");
        return std::vector<uint8_t>();
    }
#ifdef _WIN32
    detail::AlgHandle alg;
    if (!alg.open(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)) {
        return std::vector<uint8_t>();
    }
    std::vector<uint8_t> dk(dkLen, 0);
    const NTSTATUS st = ::BCryptDeriveKeyPBKDF2(
        alg.get(),
        reinterpret_cast<PUCHAR>(const_cast<char*>(pwd.data())),
        static_cast<ULONG>(pwd.size()),
        salt.empty() ? nullptr : const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()),
        static_cast<ULONGLONG>(iterations),
        reinterpret_cast<PUCHAR>(dk.data()),
        static_cast<ULONG>(dk.size()),
        0);
    if (!BCRYPT_SUCCESS(st)) {
        detail::setErr("BCryptDeriveKeyPBKDF2 失败", st);
        return std::vector<uint8_t>();
    }
    return dk;
#else
    (void)pwd;
    (void)salt;
    detail::setErr("当前平台未实现 CNG");
    return std::vector<uint8_t>();
#endif
}

// ------------------------------------------------------------------ base64 包装
/** 标准 base64（带 padding）。密码哈希串使用此编码，保证与 sql/seed.sql 预置哈希互通 */
inline std::string base64Encode(std::string_view bin) {
    return detail::base64EncodeImpl(bin, detail::stdAlphabet(), true);
}

/** 标准 base64 解码（宽容：也接受 url 字母表与缺失 padding） */
inline bool base64Decode(std::string_view txt, std::string& out) {
    return detail::base64DecodeImpl(txt, out);
}

/** base64url（无 padding），JWT 三段使用 */
inline std::string base64UrlEncode(std::string_view bin) {
    return detail::base64EncodeImpl(bin, detail::urlAlphabet(), false);
}

/** base64url 解码（宽容：也接受标准字母表与带 padding 的输入） */
inline bool base64UrlDecode(std::string_view txt, std::string& out) {
    return detail::base64DecodeImpl(txt, out);
}

// ------------------------------------------------------------------ 口令哈希
/**
 * 生成口令哈希串：pbkdf2_sha256$<iter>$<b64(salt)>$<b64(dk)>
 * 盐每次随机（16B），可直接替换 sql/seed.sql 中的预置值。
 * @param plain 明文口令
 * @return 哈希串；失败返回空串
 */
inline std::string hashPassword(std::string_view plain) {
    const std::vector<uint8_t> salt = randomBytes(kSaltBytes);
    if (salt.size() != kSaltBytes) {
        return std::string();
    }
    const std::vector<uint8_t> dk = pbkdf2Sha256(plain, salt, kPbkdf2Iters, kDkBytes);
    if (dk.size() != kDkBytes) {
        return std::string();
    }
    const std::string b64Salt = base64Encode(std::string_view(reinterpret_cast<const char*>(salt.data()), salt.size()));
    const std::string b64Hash = base64Encode(std::string_view(reinterpret_cast<const char*>(dk.data()), dk.size()));
    return std::string(kHashPrefix) + "$" + std::to_string(kPbkdf2Iters) + "$" + b64Salt + "$" + b64Hash;
}

/**
 * 校验口令。解析失败、格式不符、迭代轮数非法一律返回 false（不抛异常）。
 * @param plain  待校验明文
 * @param stored 已存哈希串（形如 pbkdf2_sha256$120000$b64salt$b64hash）
 */
inline bool verifyPassword(std::string_view plain, const std::string& stored) {
    // 期望 4 段
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t pos = stored.find('$', start);
        if (pos == std::string::npos) {
            parts.push_back(stored.substr(start));
            break;
        }
        parts.push_back(stored.substr(start, pos - start));
        start = pos + 1;
    }
    if (parts.size() != 4) {
        return false;
    }
    if (parts[0] != kHashPrefix) {
        return false;
    }
    uint32_t iters = 0;
    try {
        const long long v = std::stoll(parts[1]);
        if (v <= 0 || v > 10000000LL) {
            return false;
        }
        iters = static_cast<uint32_t>(v);
    } catch (const std::exception&) {
        return false;
    }

    std::string saltBin;
    std::string dkBin;
    if (!detail::base64DecodeImpl(parts[2], saltBin) || saltBin.empty()) {
        return false;
    }
    if (!detail::base64DecodeImpl(parts[3], dkBin) || dkBin.empty()) {
        return false;
    }

    std::vector<uint8_t> salt(saltBin.begin(), saltBin.end());
    const std::vector<uint8_t> calc = pbkdf2Sha256(plain, salt, iters, dkBin.size());
    if (calc.size() != dkBin.size()) {
        return false;
    }
    // 常量时间比较，避免比较耗时泄露哈希前缀
    unsigned char diff = 0;
    for (size_t i = 0; i < calc.size(); ++i) {
        diff = static_cast<unsigned char>(diff | (calc[i] ^ static_cast<uint8_t>(dkBin[i])));
    }
    return diff == 0;
}

/** 最近一次 CNG / 编解码错误文本（日志用，每线程独立） */
inline const char* lastError() {
    return detail::g_lastError.c_str();
}

}  // namespace rf::crypto
