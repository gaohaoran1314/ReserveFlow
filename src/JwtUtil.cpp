// =============================================================================
// src/security/JwtUtil.cpp  —  HS256 JWT 实现细节
// 对应 ARCH-P0 §3.7
// =============================================================================
#include "JwtUtil.h"

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "json.hpp"

#include "Base.h"
#include "CryptoUtil.h"

namespace rf {

namespace {

/** 常量时间字符串比较：先比长度（长度不敏感），再逐字节 XOR 累加 */
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff |
               (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

/** 取 JSON 数值（整数或浮点都接受），缺失返回 fallback */
int64_t jsonInt(const nlohmann::json& j, const char* key, int64_t fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_number()) {
        return fallback;
    }
    return static_cast<int64_t>(it->get<double>());
}

/** 取 JSON 字符串，缺失或类型不符返回空串 */
std::string jsonStr(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

}  // namespace

std::string JwtUtil::sign(const JwtPayload& p) const {
    const int64_t iat = (p.iat > 0) ? p.iat : static_cast<int64_t>(std::time(nullptr));
    const int64_t exp = (p.exp > 0) ? p.exp : (iat + static_cast<int64_t>(cfg_.ttlSeconds));

    // header 固定：只声明 HS256 + JWT，避免 alg=none 类攻击面
    nlohmann::json header = nlohmann::json::object();
    header["alg"] = "HS256";
    header["typ"] = "JWT";

    nlohmann::json payload = nlohmann::json::object();
    payload["iss"]   = cfg_.issuer;
    payload["sub"]   = p.uname;
    payload["uid"]   = p.uid;
    payload["uname"] = p.uname;
    payload["role"]  = model::toStr(p.role);
    payload["iat"]   = iat;
    payload["exp"]   = exp;
    if (!p.jti.empty()) {
        payload["jti"] = p.jti;
    }

    const std::string headerB64  = crypto::base64UrlEncode(header.dump());
    const std::string payloadB64 = crypto::base64UrlEncode(payload.dump());
    const std::string signingInput = headerB64 + "." + payloadB64;

    const std::string sig = crypto::hmacSha256(cfg_.secret, signingInput);
    if (sig.size() != 32) {
        LOG_ERROR("JWT 签发失败：HMAC 计算错误 {}", crypto::lastError());
        return std::string();
    }
    return signingInput + "." + crypto::base64UrlEncode(sig);
}

JwtUtil::VerifyStatus JwtUtil::verify(const std::string& token, JwtPayload& out,
                                      std::string* err) const {
    auto failWith = [err](VerifyStatus st, const char* reason) {
        if (err != nullptr) {
            *err = reason;
        }
        return st;
    };

    if (token.empty()) {
        return failWith(VerifyStatus::MALFORMED, "令牌为空");
    }
    if (token.size() > 4096) {
        return failWith(VerifyStatus::MALFORMED, "令牌长度非法");
    }

    // ---- 1) 结构：必须恰好三段 ----
    const size_t dot1 = token.find('.');
    if (dot1 == std::string::npos) {
        return failWith(VerifyStatus::MALFORMED, "令牌缺少分隔符");
    }
    const size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) {
        return failWith(VerifyStatus::MALFORMED, "令牌段数不足");
    }
    if (token.find('.', dot2 + 1) != std::string::npos) {
        return failWith(VerifyStatus::MALFORMED, "令牌段数过多");
    }
    const std::string headerB64  = token.substr(0, dot1);
    const std::string payloadB64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    const std::string sigB64     = token.substr(dot2 + 1);
    if (headerB64.empty() || payloadB64.empty() || sigB64.empty()) {
        return failWith(VerifyStatus::MALFORMED, "令牌存在空段");
    }

    // ---- 2) 签名：先验签名，再解析内容，避免用未验证的数据做判断 ----
    const std::string signingInput = headerB64 + "." + payloadB64;
    const std::string expectSig = crypto::hmacSha256(cfg_.secret, signingInput);
    if (expectSig.size() != 32) {
        return failWith(VerifyStatus::MALFORMED, "服务端签名密钥不可用");
    }
    std::string gotSig;
    if (!crypto::base64UrlDecode(sigB64, gotSig)) {
        return failWith(VerifyStatus::MALFORMED, "签名段不是合法 base64url");
    }
    if (!constantTimeEquals(expectSig, gotSig)) {
        return failWith(VerifyStatus::BAD_SIGNATURE, "签名不匹配");
    }

    // ---- 3) header：只接受 HS256 ----
    std::string headerTxt;
    if (!crypto::base64UrlDecode(headerB64, headerTxt)) {
        return failWith(VerifyStatus::MALFORMED, "header 段不是合法 base64url");
    }
    nlohmann::json header;
    try {
        header = nlohmann::json::parse(headerTxt);
    } catch (const std::exception&) {
        return failWith(VerifyStatus::MALFORMED, "header 不是合法 JSON");
    }
    if (!header.is_object() || jsonStr(header, "alg") != "HS256") {
        return failWith(VerifyStatus::MALFORMED, "header.alg 必须为 HS256");
    }

    // ---- 4) payload ----
    std::string payloadTxt;
    if (!crypto::base64UrlDecode(payloadB64, payloadTxt)) {
        return failWith(VerifyStatus::MALFORMED, "payload 段不是合法 base64url");
    }
    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(payloadTxt);
    } catch (const std::exception&) {
        return failWith(VerifyStatus::MALFORMED, "payload 不是合法 JSON");
    }
    if (!payload.is_object()) {
        return failWith(VerifyStatus::MALFORMED, "payload 不是 JSON 对象");
    }

    const std::string iss = jsonStr(payload, "iss");
    if (!iss.empty() && iss != cfg_.issuer) {
        return failWith(VerifyStatus::MALFORMED, "签发者不匹配");
    }

    JwtPayload p;
    p.uid   = jsonInt(payload, "uid", 0);
    p.uname = jsonStr(payload, "uname");
    if (p.uname.empty()) {
        p.uname = jsonStr(payload, "sub");   // 兼容只填 sub 的令牌
    }
    p.iat = jsonInt(payload, "iat", 0);
    p.exp = jsonInt(payload, "exp", 0);
    p.jti = jsonStr(payload, "jti");
    const auto roleOpt = model::roleFrom(jsonStr(payload, "role"));
    p.role = roleOpt.value_or(model::Role::USER);

    if (p.uid <= 0) {
        return failWith(VerifyStatus::MALFORMED, "payload.uid 缺失或非法");
    }
    if (p.exp <= 0) {
        return failWith(VerifyStatus::MALFORMED, "payload.exp 缺失或非法");
    }

    // ---- 5) 有效期（签名已验证，此处只信自己的时钟）----
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (now >= p.exp) {
        return failWith(VerifyStatus::EXPIRED, "令牌已过期");
    }
    if (p.iat > now + 300) {   // 容忍 5 分钟时钟漂移
        return failWith(VerifyStatus::MALFORMED, "payload.iat 晚于当前时间");
    }

    out = p;
    if (err != nullptr) {
        err->clear();
    }
    return VerifyStatus::OK;
}

std::string JwtUtil::sha256Hex(const std::string& s) {
    return crypto::sha256Hex(s);
}

std::string JwtUtil::blacklistKey(const std::string& token) {
    return std::string("rf:jwt:blacklist:") + crypto::sha256Hex(token);
}

} // namespace rf
