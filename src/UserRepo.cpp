// =============================================================================
// src/repo/UserRepo.cpp  —  账号仓储实现（全部走预处理语句，零字符串拼接注入面）
// 对应 ARCH-P0 §3.9 / §8.3（读路径用 DATE_FORMAT 直接取 ISO 字符串）
// =============================================================================
#include "UserRepo.h"

#include <cstdlib>
#include <string>

#include "Base.h"

namespace rf::repo {

namespace {

/**
 * 统一列清单（读路径）。
 * DATE_FORMAT 在 SQL 侧生成 yyyy-MM-ddTHH:mm:ss，C++ 侧不做 time_t 换算（§8.3），
 * 避免本地时区换算 bug。
 */
constexpr const char* kUserSelect =
    "SELECT id, username, password_hash, real_name, role, credit_score, status, phone, "
    "DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%s') AS created_at, "
    "DATE_FORMAT(updated_at, '%Y-%m-%dT%H:%i:%s') AS updated_at "
    "FROM `user` ";

/** 取单元格字符串（NULL → 空串） */
std::string cellStr(const Row& row, size_t i) {
    return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
}

/** 取单元格整数（NULL / 非法 → 0） */
int64_t cellI64(const Row& row, size_t i) {
    const std::string v = cellStr(row, i);
    if (v.empty()) {
        return 0;
    }
    return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
}

}  // namespace

model::User UserRepo::fromRow(const Row& row) {
    model::User u;
    if (row.size() < 10) {
        return u;   // 列数与 kUserSelect 不一致时返回空实体，交给上层判空
    }
    u.id           = cellI64(row, 0);
    u.username     = cellStr(row, 1);
    u.passwordHash = cellStr(row, 2);
    u.realName     = cellStr(row, 3);
    // 枚举非法值一律回落默认值：宁可展示默认，也不抛异常打断请求
    u.role         = model::roleFrom(cellStr(row, 4)).value_or(model::Role::USER);
    u.creditScore  = static_cast<int>(cellI64(row, 5));
    // D-P6-10：未知状态字符串**不能**当成 NORMAL 放进来（那是 fail-open，会把脏数据变成可用账号）。
    // 回落到 DISABLED（最严格的可登录态），并留一条 WARN 让人能查到是哪条数据坏了。
    {
        const std::string rawStatus = cellStr(row, 6);
        const auto        parsed    = model::userStatusFrom(rawStatus);
        if (!parsed.has_value()) {
            LOG_WARN("账号 id={} 的 status 取值非法「{}」，已按 DISABLED 处理（P6 堵 fail-open）",
                     u.id, rawStatus);
            u.status = model::UserStatus::DISABLED;
        } else {
            u.status = *parsed;
        }
    }
    u.phone        = cellStr(row, 7);
    u.createdAt    = cellStr(row, 8);
    u.updatedAt    = cellStr(row, 9);
    return u;
}

Result<std::optional<model::User>> UserRepo::findById(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<std::optional<model::User>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kUserSelect) + "WHERE id = ? LIMIT 1");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::User>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::User>>::make(std::nullopt);
        }
        return Result<std::optional<model::User>>::make(
            std::optional<model::User>(fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::User>>::fail(
            kDbError, std::string("查询账号失败：") + e.what());
    }
}

Result<std::optional<model::User>> UserRepo::findByUsername(DbConn& db,
                                                           const std::string& username) {
    if (username.empty()) {
        return Result<std::optional<model::User>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kUserSelect) + "WHERE username = ? LIMIT 1");
        st.bindStr(username);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::User>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::User>>::make(std::nullopt);
        }
        return Result<std::optional<model::User>>::make(
            std::optional<model::User>(fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::User>>::fail(
            kDbError, std::string("查询账号失败：") + e.what());
    }
}

VoidResult UserRepo::updatePasswordHash(DbConn& db, int64_t id,
                                               const std::string& newHash) {
    if (id <= 0 || newHash.empty()) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：账号或口令哈希为空");
    }
    try {
        Stmt st(db, "UPDATE `user` SET password_hash = ? WHERE id = ?");
        st.bindStr(newHash).bindI64(id);
        auto r = st.exec();
        if (!r.ok()) {
            return VoidResult::fail(r.code, r.msg);
        }
        if (r.data.affectedRows == 0) {
            // 值完全相同（MySQL 认为未变更）也算成功：改密语义幂等
            LOG_DEBUG("updatePasswordHash 未影响行（id={}），视为幂等成功", id);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("更新口令失败：") + e.what());
    }
}

VoidResult UserRepo::updateCreditScore(DbConn& db, int64_t id, int newScore) {
    if (id <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：账号 id 非法");
    }
    const int score = (newScore < 0) ? 0 : (newScore > 100 ? 100 : newScore);
    try {
        Stmt st(db, "UPDATE `user` SET credit_score = ? WHERE id = ?");
        st.bindI64(static_cast<int64_t>(score)).bindI64(id);
        auto r = st.exec();
        if (!r.ok()) {
            return VoidResult::fail(r.code, r.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("更新信用分失败：") + e.what());
    }
}

Result<UserRepo::UserPage> UserRepo::list(DbConn& db, int page, int size,
                                          const std::string& status, const std::string& keyword) {
    const int p  = (page < 1) ? 1 : page;
    const int sz = (size < 1) ? 10 : (size > 100 ? 100 : size);
    const int64_t offset = static_cast<int64_t>(p - 1) * static_cast<int64_t>(sz);

    try {
        // 条件拼装（只用占位符，无字符串拼接注入面）
        std::string where = "1=1";
        if (!status.empty()) {
            where += " AND status = ?";
        }
        if (!keyword.empty()) {
            where += " AND (username LIKE ? OR real_name LIKE ?)";
        }
        auto bindFilters = [&](Stmt& st) {
            if (!status.empty()) {
                st.bindStr(status);
            }
            if (!keyword.empty()) {
                const std::string like = "%" + keyword + "%";
                st.bindStr(like).bindStr(like);
            }
        };

        int64_t total = 0;
        {
            Stmt cnt(db, "SELECT COUNT(*) AS c FROM `user` WHERE " + where);
            bindFilters(cnt);
            auto cr = cnt.query();
            if (!cr.ok()) {
                return Result<UserPage>::fail(cr.code, cr.msg);
            }
            if (!cr.data.empty()) {
                total = cellI64(cr.data.front(), 0);
            }
        }

        UserPage out;
        out.total = total;
        out.page  = p;
        out.size  = sz;
        if (total == 0) {
            return Result<UserPage>::make(std::move(out));
        }

        Stmt st(db, std::string(kUserSelect) + "WHERE " + where +
                        " ORDER BY id ASC LIMIT ? OFFSET ?");
        bindFilters(st);
        st.bindI64(static_cast<int64_t>(sz)).bindI64(offset);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<UserPage>::fail(rs.code, rs.msg);
        }
        out.list.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.list.push_back(fromRow(row));
        }
        return Result<UserPage>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<UserPage>::fail(kDbError, std::string("查询用户列表失败：") + e.what());
    }
}

// ============================================================================
// P6：注册与审核（ARCH-P6 §3.1 / §3.2）
// ============================================================================

Result<int64_t> UserRepo::create(DbConn& db, const NewUser& u) {
    if (u.username.empty() || u.passwordHash.empty()) {
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：账号或口令为空");
    }
    try {
        Stmt st(db,
                "INSERT INTO `user` (username, password_hash, real_name, role, credit_score, status) "
                "VALUES (?, ?, ?, ?, ?, ?)");
        st.bindStr(u.username)
            .bindStr(u.passwordHash)
            .bindStr(u.realName)
            .bindStr(model::toStr(u.role))
            .bindI64(static_cast<int64_t>(u.creditScore))
            .bindStr(model::toStr(u.status));
        auto r = st.exec();
        if (!r.ok()) {
            // 唯一索引冲突 → 上层要能识别成「学号已被注册」，而不是通用 DB 错误
            if (st.isDupEntry()) {
                return Result<int64_t>::fail(kUsernameTaken, "该学号已被注册");
            }
            return Result<int64_t>::fail(r.code, r.msg);
        }
        return Result<int64_t>::make(static_cast<int64_t>(r.data.insertId));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("创建账号失败：") + e.what());
    }
}

Result<bool> UserRepo::resubmit(DbConn& db, int64_t id, const std::string& realName,
                                const std::string& passwordHash) {
    if (id <= 0) {
        return Result<bool>::fail(kInvalidParam, "参数校验失败：账号 id 非法");
    }
    try {
        // CAS：**只在 REJECTED 时**才更新，避免把 NORMAL/BLACKLIST 账号覆盖成 PENDING
        Stmt st(db,
                "UPDATE `user` SET real_name = ?, password_hash = ?, status = 'PENDING', "
                "updated_at = NOW() WHERE id = ? AND status = 'REJECTED'");
        st.bindStr(realName).bindStr(passwordHash).bindI64(id);
        auto r = st.exec();
        if (!r.ok()) {
            return Result<bool>::fail(r.code, r.msg);
        }
        return Result<bool>::make(r.data.affectedRows > 0);
    } catch (const std::exception& e) {
        return Result<bool>::fail(kDbError, std::string("重新提交注册失败：") + e.what());
    }
}

Result<bool> UserRepo::approve(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<bool>::fail(kInvalidParam, "参数校验失败：账号 id 非法");
    }
    try {
        // CAS：**只在 PENDING 时**才通过，防止并发/重复审核把已生效的账号再改一次
        Stmt st(db,
                "UPDATE `user` SET status = 'NORMAL', updated_at = NOW() "
                "WHERE id = ? AND status = 'PENDING'");
        st.bindI64(id);
        auto r = st.exec();
        if (!r.ok()) {
            return Result<bool>::fail(r.code, r.msg);
        }
        return Result<bool>::make(r.data.affectedRows > 0);
    } catch (const std::exception& e) {
        return Result<bool>::fail(kDbError, std::string("审核通过失败：") + e.what());
    }
}

Result<bool> UserRepo::reject(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<bool>::fail(kInvalidParam, "参数校验失败：账号 id 非法");
    }
    try {
        Stmt st(db,
                "UPDATE `user` SET status = 'REJECTED', updated_at = NOW() "
                "WHERE id = ? AND status = 'PENDING'");
        st.bindI64(id);
        auto r = st.exec();
        if (!r.ok()) {
            return Result<bool>::fail(r.code, r.msg);
        }
        return Result<bool>::make(r.data.affectedRows > 0);
    } catch (const std::exception& e) {
        return Result<bool>::fail(kDbError, std::string("审核拒绝失败：") + e.what());
    }
}

} // namespace rf::repo
