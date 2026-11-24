// =============================================================================
// src/main.cpp  —  程序入口
// 对应 ARCH-P0 §3.13 / T01 / T03 / T04 / T06 / T07 / T08
// 子命令：
//   reserveflow                      装配全部依赖并启动 HTTP 服务（8080）
//   reserveflow --version            版本信息
//   reserveflow --gen-hash <明文>     生成 pbkdf2_sha256$iter$b64salt$b64hash（可直接替换 02_seed.sql）
//   reserveflow --selftest           自测：安全层 / JWT / DbPool / Redis / 仓储层 / 服务层
//   reserveflow --config <路径>       指定配置文件
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "Application.h"
#include "Base.h"
#include "Result.h"
#include "Config.h"
#include "CryptoUtil.h"
#include "JwtUtil.h"
#include "DbPool.h"
#include "RedisClient.h"
#include "Model.h"
#include "AuditRepo.h"
#include "ResourceRepo.h"
#include "UserRepo.h"
#include "AuthService.h"
#include "ResourceService.h"

namespace {

/// 版本号（与启动横幅的 v0.5 · P5 保持一致）
constexpr const char* kVersion = "0.5.0";

/// 让控制台正确显示中文（UTF-8 输出 + 开启 ANSI 颜色）
void setupConsole() {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (::GetConsoleMode(h, &mode) != 0) {
            ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
}

void printVersion() {
    std::printf("reserveflow %s\n", kVersion);
    std::printf("  校园资源预约系统 P5（状态机 / 分布式锁 / 押金 / 规则引擎 / 管理后台 / 用户端 H5）\n");
    std::printf("  C++20 / MSVC _MSC_VER=%d\n", static_cast<int>(_MSC_VER));
    std::printf("  依赖：cpp-httplib / nlohmann-json / spdlog / hiredis / libmysql / CNG\n");
}

void printUsage() {
    std::printf("用法：\n");
    std::printf("  reserveflow [--config <path>]            启动 HTTP 服务（默认 8080）\n");
    std::printf("  reserveflow --version                    显示版本\n");
    std::printf("  reserveflow --gen-hash <明文>             生成口令哈希串\n");
    std::printf("  reserveflow --selftest [--config <path>] 自测（安全层/JWT/MySQL/Redis/仓储/服务）\n");
}

// ------------------------------------------------------------------ --gen-hash
int cmdGenHash(const std::string& plain) {
    const std::string h1 = rf::crypto::hashPassword(plain);
    const std::string h2 = rf::crypto::hashPassword(plain);
    if (h1.empty() || h2.empty()) {
        std::printf("[FAIL] 生成失败：%s\n", rf::crypto::lastError());
        return 1;
    }
    std::printf("%s\n", h1.c_str());
    std::printf("校验(自身)   ：%s\n", rf::crypto::verifyPassword(plain, h1) ? "通过" : "失败");
    std::printf("校验(第二串) ：%s\n", rf::crypto::verifyPassword(plain, h2) ? "通过" : "失败");
    std::printf("校验(错密码) ：%s\n",
                rf::crypto::verifyPassword(plain + "_x", h1) ? "意外通过" : "正确拒绝");
    std::printf("盐是否随机会 ：%s\n", (h1 != h2) ? "是（两次盐不同）" : "否（异常！）");
    return (rf::crypto::verifyPassword(plain, h1) && rf::crypto::verifyPassword(plain, h2) &&
            h1 != h2)
               ? 0
               : 1;
}

// ------------------------------------------------------------------ --selftest
struct Checker {
    int passed = 0;
    int failed = 0;

    void check(const char* name, bool ok, const std::string& detail = std::string()) {
        if (ok) {
            ++passed;
            std::printf("  [PASS] %s%s%s\n", name, detail.empty() ? "" : " → ", detail.c_str());
        } else {
            ++failed;
            std::printf("  [FAIL] %s%s%s\n", name, detail.empty() ? "" : " → ", detail.c_str());
        }
    }
};

/** 按 '$' 切分（测盐是否随机 / 解析哈希串） */
std::vector<std::string> splitDollar(const std::string& s) {
    return rf::Str::split(s, '$');
}

void testCrypto(Checker& c) {
    std::printf("\n-- 1. 安全层（CNG：PBKDF2-HMAC-SHA256）--\n");

    const std::string plain = "Admin@123456";
    const std::string h1 = rf::crypto::hashPassword(plain);
    const std::string h2 = rf::crypto::hashPassword(plain);

    c.check("hashPassword 前缀格式", rf::Str::startsWith(h1, "pbkdf2_sha256$120000$"), h1.substr(0, 30) + "...");
    c.check("同明文两次盐不同", h1 != h2);

    const auto p1 = splitDollar(h1);
    const auto p2 = splitDollar(h2);
    c.check("哈希串 4 段", p1.size() == 4 && p2.size() == 4);
    if (p1.size() == 4 && p2.size() == 4) {
        std::string s1, s2;
        rf::crypto::base64Decode(p1[2], s1);
        rf::crypto::base64Decode(p2[2], s2);
        c.check("盐长度 16B", s1.size() == 16 && s2.size() == 16,
                std::to_string(s1.size()) + "B / " + std::to_string(s2.size()) + "B");
        std::string d1;
        rf::crypto::base64Decode(p1[3], d1);
        c.check("派生密钥 32B", d1.size() == 32, std::to_string(d1.size()) + "B");
    }

    c.check("verifyPassword 第 1 串", rf::crypto::verifyPassword(plain, h1));
    c.check("verifyPassword 第 2 串", rf::crypto::verifyPassword(plain, h2));
    c.check("verifyPassword 拒绝错密码", !rf::crypto::verifyPassword("Admin@123457", h1));

    // 与 sql/seed.sql 中预置的管理员哈希互通（标准 base64 编码）
    const std::string seedAdmin =
        "pbkdf2_sha256$120000$5GSkhu8Mu1pXdEsbxHRUWA==$hgPTCoY38Pw/PHwT+TzQaNVTvBbmjD8LE9D4f0P8tmU=";
    c.check("校验 seed.sql 预置管理员哈希", rf::crypto::verifyPassword("Admin@123456", seedAdmin));

    c.check("sha256Hex 长度 64", rf::crypto::sha256Hex("abc").size() == 64,
            rf::crypto::sha256Hex("abc"));
    c.check("sha256(\"abc\") 已知向量",
            rf::crypto::sha256Hex("abc") ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // base64url round-trip
    const std::string bin(32, '\x7F');
    std::string decoded;
    const std::string enc = rf::crypto::base64UrlEncode(bin);
    c.check("base64url 往返", rf::crypto::base64UrlDecode(enc, decoded) && decoded == bin,
            "len=" + std::to_string(enc.size()));
}

void testJwt(Checker& c, const rf::JwtConfig& jwtCfg) {
    std::printf("\n-- 2. JWT（HS256）--\n");
    rf::JwtUtil jwt(jwtCfg);

    rf::JwtPayload p;
    p.uid   = 1;
    p.uname = "admin";
    p.role  = rf::model::Role::ADMIN;
    p.iat   = static_cast<int64_t>(std::time(nullptr));
    p.exp   = p.iat + jwt.ttlSeconds();
    p.jti   = rf::Str::randomHex(8);

    const std::string token = jwt.sign(p);
    c.check("sign 产出三段式", std::count(token.begin(), token.end(), '.') == 2,
            "len=" + std::to_string(token.size()));

    rf::JwtPayload out;
    std::string err;
    const auto st1 = jwt.verify(token, out, &err);
    c.check("verify 往返 OK", st1 == rf::JwtUtil::VerifyStatus::OK,
            std::string("status=") + std::to_string(static_cast<int>(st1)) + " err=" + err);
    // 打印示例令牌，供 jwt.io / 独立实现交叉验证（本地开发密钥，非生产）
    std::printf("  自测令牌：%s\n", token.c_str());
    c.check("载荷回读一致", out.uid == 1 && out.uname == "admin" &&
                                out.role == rf::model::Role::ADMIN && out.exp == p.exp,
            "uid=" + std::to_string(out.uid) + " uname=" + out.uname +
                " role=" + rf::model::toStr(out.role));

    // 篡改 1 个字节 → 签名错误（改 payload 段第一个字符；签名校验先于内容解析）
    std::string tampered = token;
    const size_t payloadPos = tampered.find('.') + 1;
    tampered[payloadPos] = (tampered[payloadPos] == 'A') ? 'B' : 'A';
    rf::JwtPayload out2;
    std::string err2;
    const auto st2 = jwt.verify(tampered, out2, &err2);
    c.check("篡改 1 字节 → BAD_SIGNATURE", st2 == rf::JwtUtil::VerifyStatus::BAD_SIGNATURE,
            "err=" + err2);

    // 结构非法
    rf::JwtPayload out3;
    const auto st3 = jwt.verify("not-a-jwt", out3);
    c.check("结构非法 → MALFORMED", st3 == rf::JwtUtil::VerifyStatus::MALFORMED);

    // 伪造过期时间（用同一密钥重签，但 exp 在 100 秒前）
    rf::JwtPayload expired = p;
    expired.iat = p.iat - 200;
    expired.exp = p.iat - 100;
    const std::string expiredToken = jwt.sign(expired);
    rf::JwtPayload out4;
    std::string err4;
    const auto st4 = jwt.verify(expiredToken, out4, &err4);
    c.check("exp 过期 → EXPIRED", st4 == rf::JwtUtil::VerifyStatus::EXPIRED, "err=" + err4);

    // 换密钥签发 → 签名不匹配
    rf::JwtConfig otherCfg = jwtCfg;
    otherCfg.secret = jwtCfg.secret + "-other";
    rf::JwtUtil otherJwt(otherCfg);
    rf::JwtPayload out5;
    const auto st5 = otherJwt.verify(token, out5);
    c.check("异密钥签发 → BAD_SIGNATURE", st5 == rf::JwtUtil::VerifyStatus::BAD_SIGNATURE);

    c.check("blacklistKey 前缀", rf::Str::startsWith(rf::JwtUtil::blacklistKey(token), "rf:jwt:blacklist:"),
            rf::JwtUtil::blacklistKey(token).substr(0, 40) + "...");
}

void testDb(Checker& c, const rf::MysqlConfig& myCfg) {
    std::printf("\n-- 3. DbPool（MySQL C API 连接池 + 预处理语句）--\n");
    rf::DbPool pool(myCfg);
    std::string err;
    const bool inited = pool.init(err);
    c.check("初始化连接池", inited, inited ? ("total=" + std::to_string(pool.totalCount()) +
                                            " idle=" + std::to_string(pool.idleCount()))
                                         : err);
    if (!inited) {
        return;
    }
    c.check("连接数 == poolSize", pool.totalCount() == static_cast<size_t>(myCfg.poolSize),
            std::to_string(pool.totalCount()) + " / " + std::to_string(myCfg.poolSize));

    int latency = -1;
    std::string pingErr;
    const bool pingOk = pool.ping(&latency, pingErr);
    c.check("ping 成功并返回耗时", pingOk, pingOk ? (std::to_string(latency) + "ms") : pingErr);

    {
        rf::DbPool::Guard guard = pool.acquire(3000);
        c.check("acquire 取到连接", static_cast<bool>(guard));
        if (guard) {
            auto rs = guard->rawQuery("SELECT 1");
            bool ok = rs.ok() && rs.data.size() == 1 && rs.data[0].size() == 1 &&
                      rs.data[0][0].has_value() && *rs.data[0][0] == "1";
            c.check("SELECT 1 可查", ok, ok ? "值=" + *rs.data[0][0] : rs.msg);

            auto ver = guard->rawQuery("SELECT VERSION()");
            c.check("读取 MySQL 版本", ver.ok() && !ver.data.empty() &&
                                          ver.data[0][0].has_value(),
                    (ver.ok() && !ver.data.empty() && ver.data[0][0].has_value())
                        ? *ver.data[0][0] : ver.msg);

            // ---- 预处理语句 + 中文长文本不截断 ----
            std::string cn;
            for (int i = 0; i < 40; ++i) {
                cn += "中文描述测试-电子显微镜需培训后使用;";
            }
            while (cn.size() < 1200) {
                cn += "补";
            }
            {
                rf::Stmt st(*guard, "SELECT ? AS v");
                st.bindStr(cn);
                auto r = st.query();
                const bool ok2 = r.ok() && r.data.size() == 1 && r.data[0].size() == 1 &&
                                 r.data[0][0].has_value();
                if (ok2) {
                    const std::string got = *r.data[0][0];
                    c.check("中文长文本不截断（>256B）", got == cn,
                            "输入 " + std::to_string(cn.size()) + "B / 回读 " +
                                std::to_string(got.size()) + "B");
                } else {
                    c.check("中文长文本不截断（>256B）", false, r.msg);
                }
            }

            // ---- 数字 / NULL 列 ----
            {
                rf::Stmt st(*guard, "SELECT ? AS d, NULL AS n, 42 AS i");
                st.bindDouble(3.25);
                auto r = st.query();
                const bool ok3 = r.ok() && r.data.size() == 1 && r.data[0].size() == 3;
                if (ok3) {
                    const auto& row = r.data[0];
                    c.check("DECIMAL/DOUBLE 回读", row[0].has_value() && *row[0] == "3.25",
                            row[0].has_value() ? *row[0] : "null");
                    c.check("NULL 列 → nullopt", !row[1].has_value());
                    c.check("INT 列回读", row[2].has_value() && *row[2] == "42",
                            row[2].has_value() ? *row[2] : "null");
                } else {
                    c.check("DECIMAL/DOUBLE 回读", false, r.msg);
                }
            }

            // ---- 真实业务表（若已建）----
            {
                rf::Stmt st(*guard,
                            "SELECT id, code, name, description FROM resource ORDER BY id LIMIT 1");
                auto r = st.query();
                if (!r.ok()) {
                    std::printf("  [SKIP] resource 表尚不可用（%s）\n", r.msg.c_str());
                } else if (r.data.empty()) {
                    std::printf("  [SKIP] resource 表为空\n");
                } else {
                    const auto& row = r.data[0];
                    const std::string name = row.size() > 2 && row[2].has_value() ? *row[2] : "";
                    const std::string desc = row.size() > 3 && row[3].has_value() ? *row[3] : "";
                    c.check("resource 表中文回读", !name.empty(),
                            "code=" + (row[1].has_value() ? *row[1] : "") + " name=" + name +
                                " desc=" + std::to_string(desc.size()) + "B");
                }
            }
        }
    }

    pool.shutdown();
    c.check("shutdown 后 idle 归零", pool.idleCount() == 0,
            std::to_string(pool.idleCount()));
}

void testRedis(Checker& c, const rf::RedisConfig& rdCfg) {
    std::printf("\n-- 4. RedisClient（hiredis 连接池）--\n");
    rf::RedisClient redis(rdCfg);
    std::string err;
    const bool inited = redis.init(err);
    c.check("初始化连接池", inited, inited ? ("total=" + std::to_string(rdCfg.poolSize)) : err);

    int latency = -1;
    std::string pingErr;
    const bool pingOk = redis.ping(&latency, pingErr);
    c.check("PING → PONG", pingOk, pingOk ? (std::to_string(latency) + "ms") : pingErr);

    const std::string key = "rf:selftest:key";
    const std::string val = "中文值-with-ascii-123";

    auto rSetEx = redis.setEx(key, val, 60);
    c.check("setEx", rSetEx.ok(), rSetEx.ok() ? "ok" : (rSetEx.msg + " [code=" + std::to_string(rSetEx.code) + "]"));

    auto rGet = redis.get(key);
    const bool getOk = rGet.ok() && rGet.data.has_value() && *rGet.data == val;
    c.check("get", getOk,
            rGet.ok() ? (rGet.data.has_value() ? *rGet.data : std::string("<nil>"))
                      : (rGet.msg + " [code=" + std::to_string(rGet.code) + "]"));

    auto rExists1 = redis.exists(key);
    c.check("exists（存在）", rExists1.ok() && rExists1.data,
            rExists1.ok() ? "true" : (rExists1.msg + " [code=" + std::to_string(rExists1.code) + "]"));

    auto rDel = redis.del(key);
    c.check("del", rDel.ok() && rDel.data == 1,
            rDel.ok() ? ("删除 " + std::to_string(rDel.data) + " 个 key")
                      : (rDel.msg + " [code=" + std::to_string(rDel.code) + "]"));

    auto rExists2 = redis.exists(key);
    c.check("exists（已删）", rExists2.ok() && !rExists2.data,
            rExists2.ok() ? "false" : (rExists2.msg + " [code=" + std::to_string(rExists2.code) + "]"));

    const std::string lockKey = "rf:selftest:lock";
    (void)redis.del(lockKey);
    auto rNx1 = redis.setNxPx(lockKey, "owner-1", 5000);
    c.check("setNxPx 首次抢锁", rNx1.ok() && rNx1.data,
            rNx1.ok() ? "true" : (rNx1.msg + " [code=" + std::to_string(rNx1.code) + "]"));
    auto rNx2 = redis.setNxPx(lockKey, "owner-2", 5000);
    c.check("setNxPx 二次抢锁失败", rNx2.ok() && !rNx2.data,
            rNx2.ok() ? "false（符合预期）" : (rNx2.msg + " [code=" + std::to_string(rNx2.code) + "]"));

    auto rEval = redis.eval(rf::RedisClient::kUnlockScript, {lockKey}, {"owner-1"});
    c.check("eval 释放锁脚本", rEval.ok() && rEval.data == 1,
            rEval.ok() ? ("返回值 " + std::to_string(rEval.data))
                       : (rEval.msg + " [code=" + std::to_string(rEval.code) + "]"));

    auto rEval2 = redis.eval(rf::RedisClient::kUnlockScript, {lockKey}, {"owner-1"});
    c.check("eval 重复释放返回 0", rEval2.ok() && rEval2.data == 0,
            rEval2.ok() ? ("返回值 " + std::to_string(rEval2.data))
                        : (rEval2.msg + " [code=" + std::to_string(rEval2.code) + "]"));

    redis.shutdown();
}

// ------------------------------------------------- 5/6. 仓储层 + 服务层（T06/T07）
void testRepoAndService(Checker& c, const rf::Config& cfg) {
    std::printf("\n-- 5. 仓储层（ResourceRepo / UserRepo / AuditRepo）--\n");
    rf::DbPool pool(cfg.mysql);
    std::string err;
    if (!pool.init(err)) {
        c.check("仓储层自测：初始化连接池", false, err);
        return;
    }

    rf::repo::UserRepo     userRepo;
    rf::repo::ResourceRepo rr;
    rf::repo::AuditRepo    ar;

    {
        rf::DbPool::Guard g = pool.acquire(3000);
        c.check("仓储层自测：取连接", static_cast<bool>(g));
        if (!g) {
            pool.shutdown();
            return;
        }

        // ---- UserRepo
        auto admin = userRepo.findByUsername(*g, "admin");
        c.check("UserRepo::findByUsername(admin)",
                admin.ok() && admin.data.has_value() &&
                    admin.data->role == rf::model::Role::ADMIN && !admin.data->passwordHash.empty(),
                admin.ok() ? ("role=" + rf::model::toStr(admin.data->role))
                           : ("code=" + std::to_string(admin.code) + " " + admin.msg));
        auto ghost = userRepo.findByUsername(*g, "__no_such_user__");
        c.check("UserRepo::findByUsername 不存在 → nullopt", ghost.ok() && !ghost.data.has_value());

        // ---- search：无条件
        rf::model::ResourceQuery qAll;
        qAll.page = 1;
        qAll.size = 100;
        auto all = rr.search(*g, qAll);
        c.check("search 无条件：total == list.size()",
                all.ok() && all.data.total == static_cast<int64_t>(all.data.list.size()),
                all.ok() ? ("total=" + std::to_string(all.data.total) + " list=" +
                            std::to_string(all.data.list.size()))
                         : all.msg);

        // ---- search：type 过滤
        rf::model::ResourceQuery qType;
        qType.page = 1;
        qType.size = 100;
        qType.type = rf::model::ResourceType::MEETING_ROOM;
        auto byType = rr.search(*g, qType);
        bool typeOk = byType.ok();
        for (const auto& r : byType.data.list) {
            if (r.type != rf::model::ResourceType::MEETING_ROOM) {
                typeOk = false;
            }
        }
        c.check("search type=MEETING_ROOM：全部命中且 total==size",
                typeOk && byType.data.total == static_cast<int64_t>(byType.data.list.size()),
                "total=" + std::to_string(byType.data.total));

        // ---- search：keyword 过滤（name 或 description 命中）
        rf::model::ResourceQuery qKw;
        qKw.page    = 1;
        qKw.size    = 100;
        qKw.keyword = "图书馆";
        auto byKw   = rr.search(*g, qKw);
        bool kwOk   = byKw.ok();
        for (const auto& r : byKw.data.list) {
            const bool hit = r.name.find("图书馆") != std::string::npos ||
                             r.description.find("图书馆") != std::string::npos ||
                             r.location.find("图书馆") != std::string::npos;
            if (!hit) {
                kwOk = false;
            }
        }
        c.check("search keyword=图书馆：全部命中", kwOk && byKw.data.total >= 1,
                "total=" + std::to_string(byKw.data.total));

        // ---- search：location 过滤
        rf::model::ResourceQuery qLoc;
        qLoc.page     = 1;
        qLoc.size     = 100;
        qLoc.location = "图书馆3层";
        auto byLoc    = rr.search(*g, qLoc);
        bool locOk    = byLoc.ok();
        for (const auto& r : byLoc.data.list) {
            if (r.location != "图书馆3层") {
                locOk = false;
            }
        }
        c.check("search location=图书馆3层：全部命中", locOk && byLoc.data.total == 1,
                "total=" + std::to_string(byLoc.data.total));

        // ---- search：组合过滤（type + status）
        rf::model::ResourceQuery qCombo;
        qCombo.page   = 1;
        qCombo.size   = 100;
        qCombo.type   = rf::model::ResourceType::MEETING_ROOM;
        qCombo.status = rf::model::ResourceState::FREE;
        auto byCombo  = rr.search(*g, qCombo);
        bool comboOk  = byCombo.ok();
        for (const auto& r : byCombo.data.list) {
            if (r.type != rf::model::ResourceType::MEETING_ROOM ||
                r.status != rf::model::ResourceState::FREE) {
                comboOk = false;
            }
        }
        c.check("search type+status 组合过滤", comboOk,
                "total=" + std::to_string(byCombo.data.total));

        // ---- 分页：第 1 页与第 2 页不重复，total 不变
        rf::model::ResourceQuery qp1;
        qp1.page = 1;
        qp1.size = 2;
        rf::model::ResourceQuery qp2;
        qp2.page = 2;
        qp2.size = 2;
        auto page1 = rr.search(*g, qp1);
        auto page2 = rr.search(*g, qp2);
        bool noOverlap = true;
        for (const auto& a : page1.data.list) {
            for (const auto& b : page2.data.list) {
                if (a.id == b.id) {
                    noOverlap = false;
                }
            }
        }
        c.check("分页 page1/page2 数据不重复且 total 相同",
                page1.ok() && page2.ok() && noOverlap && page1.data.total == page2.data.total &&
                    page1.data.list.size() == 2,
                "p1=" + std::to_string(page1.data.list.size()) + " p2=" +
                    std::to_string(page2.data.list.size()) + " total=" +
                    std::to_string(page1.data.total));

        // ---- countActiveReservations：P0 预约表为空 → 0
        auto active = rr.countActiveReservations(*g, 1);
        c.check("countActiveReservations 空表返回 0", active.ok() && active.data == 0,
                active.ok() ? ("count=" + std::to_string(active.data)) : active.msg);

        // ---- listLocations / existsNotDeleted
        auto locs = rr.listLocations(*g);
        c.check("listLocations 非空且去重", locs.ok() && !locs.data.empty(),
                "地点数=" + std::to_string(locs.data.size()));
        auto exist1 = rr.existsNotDeleted(*g, 1);
        auto existNo = rr.existsNotDeleted(*g, 999999);
        c.check("existsNotDeleted 语义正确", exist1.ok() && exist1.data && existNo.ok() &&
                                                 !existNo.data);
    }

    // ---- 服务层
    std::printf("\n-- 6. 服务层（AuthService / ResourceService）--\n");
    rf::RedisClient redis(cfg.redis);
    std::string     rdErr;
    const bool      redisOk = redis.init(rdErr);
    if (!redisOk) {
        std::printf("  [WARN] Redis 不可用（%s）：登录仍可测，登出/失效名单相关项跳过\n",
                    rdErr.c_str());
    }

    rf::JwtUtil        jwt(cfg.jwt);
    rf::AuthService    auth(pool, redis, jwt);
    rf::ResourceService resSvc(pool, rr, ar);

    rf::AuthContext adminCtx;
    adminCtx.userId   = 1;
    adminCtx.username = "admin";
    adminCtx.role     = rf::model::Role::ADMIN;
    adminCtx.expEpoch = static_cast<int64_t>(std::time(nullptr)) + cfg.jwt.ttlSeconds;

    // ---- 登录
    auto okLogin = auth.login("admin", "Admin@123456", "selftest");
    c.check("登录 admin 成功并签发令牌",
            okLogin.ok() && !okLogin.data.token.empty() && okLogin.data.user.role ==
                                                                rf::model::Role::ADMIN,
            okLogin.ok() ? ("token 长度=" + std::to_string(okLogin.data.token.size()) +
                            " expiresAt=" + okLogin.data.expiresAt)
                         : (std::to_string(okLogin.code) + " " + okLogin.msg));

    auto badPwd = auth.login("admin", "definitely-wrong-pwd", "selftest");
    c.check("错误密码 → 2000（账号或密码错误）", !badPwd.ok() && badPwd.code == 2000,
            std::to_string(badPwd.code) + " " + badPwd.msg);

    auto noUser = auth.login("__no_such_user__", "whatever", "selftest");
    c.check("账号不存在 → 2000（不泄露账号是否存在）",
            !noUser.ok() && noUser.code == 2000 && noUser.msg == badPwd.msg,
            std::to_string(noUser.code) + " " + noUser.msg);

    auto blackLogin = auth.login("student02", "Black@123456", "selftest");
    c.check("黑名单账号可登录且 creditScore=40",
            blackLogin.ok() && blackLogin.data.user.creditScore == 40 &&
                blackLogin.data.user.status == rf::model::UserStatus::BLACKLIST,
            blackLogin.ok() ? ("creditScore=" + std::to_string(blackLogin.data.user.creditScore) +
                               " status=" + rf::model::toStr(blackLogin.data.user.status))
                            : blackLogin.msg);

    // ---- 新增 / 重复编号
    const std::string tempCode = "SELFTEST-" + rf::Str::randomHex(4);
    rf::model::ResourceCreateReq createReq;
    createReq.code          = tempCode;
    createReq.name          = "自测会议室A";
    createReq.type          = rf::model::ResourceType::MEETING_ROOM;
    createReq.location      = "自测楼-101";
    createReq.capacity      = 6;
    createReq.depositAmount = 12.5;
    createReq.description   = "自测用资源，脚本结束会自动清理";

    auto created = resSvc.create(createReq, adminCtx);
    const int64_t tempId = created.ok() ? created.data : 0;
    c.check("create 成功返回新 id", created.ok() && tempId > 0,
            created.ok() ? ("id=" + std::to_string(tempId) + " code=" + tempCode) : created.msg);

    auto dupCreate = resSvc.create(createReq, adminCtx);
    c.check("重复编号 create → 3001", !dupCreate.ok() && dupCreate.code == 3001,
            std::to_string(dupCreate.code) + " " + dupCreate.msg);

    if (tempId > 0) {
        // ---- 半更新：只改 name
        rf::model::ResourceUpdateReq updReq;
        updReq.name = "自测会议室A-改名";
        auto updated = resSvc.update(tempId, updReq, adminCtx);
        const bool halfOk =
            updated.ok() && updated.data.name == "自测会议室A-改名" &&
            updated.data.type == rf::model::ResourceType::MEETING_ROOM &&
            updated.data.location == "自测楼-101" && updated.data.capacity == 6 &&
            std::fabs(updated.data.depositAmount - 12.5) < 0.001 &&
            updated.data.description == createReq.description;
        c.check("update 只传 name：其他字段保持不变", halfOk,
                updated.ok() ? ("name=" + updated.data.name + " type=" +
                                rf::model::toStr(updated.data.type) + " location=" +
                                updated.data.location + " capacity=" +
                                std::to_string(updated.data.capacity) + " deposit=" +
                                std::to_string(updated.data.depositAmount))
                             : updated.msg);

        // ---- 状态：非法字面量 / P0 不可写 / 正常
        auto badLiteral = resSvc.changeStatus(tempId, "ABC", adminCtx);
        c.check("PATCH 非法字面量 ABC → 1000",
                !badLiteral.ok() && badLiteral.code == 1000,
                std::to_string(badLiteral.code) + " " + badLiteral.msg);

        auto deniedState = resSvc.changeStatus(tempId, "IN_USE", adminCtx);
        c.check("PATCH 合法枚举但 P0 不可写 IN_USE → 3002",
                !deniedState.ok() && deniedState.code == 3002,
                std::to_string(deniedState.code) + " " + deniedState.msg);

        auto toMaint = resSvc.changeStatus(tempId, "MAINTENANCE", adminCtx);
        c.check("PATCH MAINTENANCE 成功",
                toMaint.ok() && toMaint.data.status == rf::model::ResourceState::MAINTENANCE,
                toMaint.ok() ? rf::model::toStr(toMaint.data.status) : toMaint.msg);

        // ---- status_log：''→FREE、FREE→MAINTENANCE 均落库，rule_code 为 NULL
        {
            rf::DbPool::Guard g = pool.acquire(3000);
            if (g) {
                auto logs = ar.listByBusiness(*g, "RESOURCE", tempId, 20);
                bool hasCreate = false;
                bool hasMaint  = false;
                bool allRuleNull = true;
                for (const auto& e : logs.data) {
                    if (e.fromStatus.empty() && e.toStatus == "FREE") {
                        hasCreate = true;
                    }
                    if (e.toStatus == "MAINTENANCE") {
                        hasMaint = true;
                    }
                    if (e.ruleCode.has_value()) {
                        allRuleNull = false;
                    }
                }
                c.check("status_log 落 ''→FREE 与 →MAINTENANCE 两条",
                        logs.ok() && hasCreate && hasMaint,
                        "条数=" + std::to_string(logs.data.size()));
                c.check("status_log.rule_code 全部为 NULL（D-12）", logs.ok() && allRuleNull);
            }
        }

        // ---- 删除：成功提示 + 幂等 + 详情 3000
        auto del1 = resSvc.remove(tempId, adminCtx);
        c.check("remove 成功且 msg 含「永久保留」",
                del1.ok() && del1.msg.find("永久保留") != std::string::npos, del1.msg);

        auto del2 = resSvc.remove(tempId, adminCtx);
        c.check("重复 remove 幂等成功", del2.ok(), del2.msg);

        auto afterDel = resSvc.detail(tempId);
        c.check("删除后 detail → 3000", !afterDel.ok() && afterDel.code == 3000,
                std::to_string(afterDel.code) + " " + afterDel.msg);

        // ---- 清理自测数据（硬删，避免污染开发库）
        {
            rf::DbPool::Guard g = pool.acquire(3000);
            if (g) {
                rf::Stmt delLog(*g, "DELETE FROM `status_log` WHERE business_type = 'RESOURCE' AND "
                                    "business_id = ?");
                delLog.bindI64(tempId);
                (void)delLog.exec();
                rf::Stmt delRes(*g, "DELETE FROM `resource` WHERE id = ?");
                delRes.bindI64(tempId);
                auto rc2 = delRes.exec();
                c.check("自测数据已清理", rc2.ok(), rc2.ok() ? "已删除临时资源" : rc2.msg);
            }
        }
    }

    redis.shutdown();
    pool.shutdown();
}

int cmdSelfTest(const std::string& configPath) {
    std::printf("ReserveFlow 地基自测（T01-T04）\n");
    std::printf("配置文件：%s\n", configPath.c_str());

    rf::Config cfg;
    std::string err;
    if (!rf::Config::load(configPath, cfg, err)) {
        std::printf("[FAIL] 配置加载失败：%s\n", err.c_str());
        return 1;
    }
    std::printf("配置加载：OK（server.port=%d mysql=%s:%d/%s redis=%s:%d jwt.ttl=%ds）\n",
                cfg.server.port, cfg.mysql.host.c_str(), cfg.mysql.port,
                cfg.mysql.database.c_str(), cfg.redis.host.c_str(), cfg.redis.port,
                cfg.jwt.ttlSeconds);

    rf::initLogger(cfg.log.level, cfg.log.file, cfg.log.rotateSizeMB, cfg.log.rotateCount);
    LOG_INFO("自测开始：日志中文输出验证（配置文件 {}）", configPath);
    LOG_INFO("依赖段：mysql={}:{} redis={}:{}", cfg.mysql.host, cfg.mysql.port, cfg.redis.host,
             cfg.redis.port);

    Checker c;
    try {
        testCrypto(c);
        testJwt(c, cfg.jwt);
        testDb(c, cfg.mysql);
        testRedis(c, cfg.redis);
        testRepoAndService(c, cfg);
    } catch (const std::exception& e) {
        std::printf("\n[FAIL] 自测过程中抛出异常：%s\n", e.what());
        LOG_ERROR("自测异常：{}", e.what());
        ++c.failed;
    } catch (...) {
        std::printf("\n[FAIL] 自测过程中抛出未知异常\n");
        ++c.failed;
    }

    std::printf("\n===== 自测汇总：PASS %d / FAIL %d =====\n", c.passed, c.failed);
    LOG_INFO("自测结束：PASS {} / FAIL {}", c.passed, c.failed);
    return c.failed == 0 ? 0 : 1;
}

// ------------------------------------------------------------------ 默认启动
int cmdRun(const std::string& configPath) {
    rf::Application app;
    std::string     err;
    if (!app.init(configPath, err)) {
        std::printf("[配置错误 1011] %s\n", err.c_str());
        return static_cast<int>(rf::kConfigError);
    }
    return app.run();
}

}  // namespace

int main(int argc, char** argv) {
    setupConsole();

    std::string configPath;
    std::string genHash;
    bool hasGenHash = false;
    bool selftest   = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            printVersion();
            return 0;
        }
        if (a == "--help" || a == "-h") {
            printUsage();
            return 0;
        }
        if (a == "--config" && i + 1 < argc) {
            configPath = argv[++i];
            continue;
        }
        if (a == "--gen-hash" && i + 1 < argc) {
            genHash    = argv[++i];
            hasGenHash = true;
            continue;
        }
        if (a == "--selftest") {
            selftest = true;
            continue;
        }
        std::printf("未知参数：%s\n\n", a.c_str());
        printUsage();
        return 2;
    }

    try {
        if (hasGenHash) {
            return cmdGenHash(genHash);
        }
        if (configPath.empty()) {
            configPath = rf::Config::resolvePath(argc > 0 ? argv[0] : nullptr);
        }
        if (selftest) {
            return cmdSelfTest(configPath);
        }
        return cmdRun(configPath);
    } catch (const std::exception& e) {
        std::printf("[未捕获异常 1008] %s\n", e.what());
        return static_cast<int>(rf::kInternalError);
    } catch (...) {
        std::printf("[未捕获异常 1008] 未知类型\n");
        return static_cast<int>(rf::kInternalError);
    }
}
