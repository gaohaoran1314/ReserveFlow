// =============================================================================
// src/repo/UserRepo.h  —  账号仓储（无状态，方法首参 DbConn&）
// 对应 ARCH-P0 §3.9 / §1.1：只做参数化 SQL 与行 ↔ 结构体映射，不做业务判断，
//                          不开启/提交事务（事务由 Service 掌握）。
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "Model.h"

namespace rf::repo {

/**
 * 账号读取与写回。
 * @note 所有方法都不持有连接：调用方先用 DbPool::acquire() 取 Guard，再把 *guard 传进来。
 */
class UserRepo {
public:
    UserRepo()  = default;
    ~UserRepo() = default;
    UserRepo(const UserRepo&)            = delete;
    UserRepo& operator=(const UserRepo&) = delete;

    /**
     * 按主键查账号。
     * @param db   连接（由调用方持有）
     * @param id   user.id
     * @return 成功时 data 为 nullopt 表示不存在
     */
    Result<std::optional<model::User>> findById(DbConn& db, int64_t id);

    /**
     * 按登录名查账号（登录入口）。
     * @param db       连接
     * @param username 登录名，精确匹配（uk_user_username）
     * @return 成功时 data 为 nullopt 表示账号不存在
     */
    Result<std::optional<model::User>> findByUsername(DbConn& db, const std::string& username);

    /**
     * 覆盖写入口令哈希。
     * @param db      连接
     * @param id      user.id
     * @param newHash 完整 pbkdf2_sha256$iter$b64salt$b64hash 串
     * @return 影响行数为 0 时仍视为成功（幂等）；SQL 失败返回 kDbError
     */
    VoidResult updatePasswordHash(DbConn& db, int64_t id, const std::string& newHash);

    /**
     * 更新信用分（P1 预留，P0 不调用）。
     * @param db       连接
     * @param id       user.id
     * @param newScore 新信用分（0..100）
     */
    VoidResult updateCreditScore(DbConn& db, int64_t id, int newScore);

    // ---- P4 T00：只读用户列表（管理后台 /users 页与概览「黑名单人数」用）----

    /** 用户分页结果（total 与 list 同条件） */
    struct UserPage {
        std::vector<model::User> list;
        int64_t                  total = 0;
        int                      page  = 1;
        int                      size  = 10;
    };

    /**
     * 分页查账号（只读）。
     * @param status  可选：NORMAL / BLACKLIST / DISABLED / PENDING / REJECTED（空串 = 不过滤）
     * @param keyword 可选：模糊匹配 username 或 real_name（空串 = 不过滤）
     * @note 按 id 升序，保证分页稳定；不返回 password_hash 之外的多余列。
     */
    Result<UserPage> list(DbConn& db, int page, int size, const std::string& status,
                          const std::string& keyword);

    // ---- P6：注册与审核（ARCH-P6 §3.1 / §3.2）----

    /** 新账号的入库字段（role / creditScore / status 由服务层定，**不接受客户端传入**） */
    struct NewUser {
        std::string        username;
        std::string        passwordHash;
        std::string        realName;
        model::Role        role        = model::Role::USER;
        int                creditScore = 100;
        model::UserStatus  status      = model::UserStatus::PENDING;
    };

    /**
     * 插入一个新账号。
     * @return 成功时 data 为新 id；
     *         `uk_user_username` 冲突 → **kUsernameTaken(2003)**（而不是通用 kDbError）
     * @note 唯一性**只靠唯一索引兜底**，不做"先查后插"（并发下先查后插必漏）。
     */
    Result<int64_t> create(DbConn& db, const NewUser& u);

    /**
     * 被拒账号重新提交（REJECTED → PENDING），复用原 id：
     * `UPDATE user SET real_name=?, password_hash=?, status='PENDING' WHERE id=? AND status='REJECTED'`
     * @return data=true 表示真的改到了行；false 表示状态已不是 REJECTED（并发下被别人改过）
     * @note 必须带 `AND status='REJECTED'`：否则会把 NORMAL 账号覆盖成 PENDING
     */
    Result<bool> resubmit(DbConn& db, int64_t id, const std::string& realName,
                          const std::string& passwordHash);

    /**
     * 审核通过：`UPDATE ... SET status='NORMAL' WHERE id=? AND status='PENDING'`
     * @return data=true 表示真的改到了行；false 表示当前不是 PENDING（并发重复审核）
     * @note 必须带 `AND status='PENDING'`：否则并发下可能把刚变成 NORMAL/BLACKLIST 的账号改回去
     */
    Result<bool> approve(DbConn& db, int64_t id);

    /**
     * 审核拒绝：`UPDATE ... SET status='REJECTED' WHERE id=? AND status='PENDING'`
     * @return 语义同 approve
     */
    Result<bool> reject(DbConn& db, int64_t id);

private:
    /**
     * 行 → 账号实体。
     * 列顺序必须与 .cpp 中的 kUserSelect 严格一致：
     * id, username, password_hash, real_name, role, credit_score, status, phone,
     * created_at, updated_at
     */
    static model::User fromRow(const Row& row);
};

} // namespace rf::repo
