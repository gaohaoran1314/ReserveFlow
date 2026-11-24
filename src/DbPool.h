// =============================================================================
// src/db/DbPool.h  —  MySQL C API 连接池 + 预处理语句 + 事务 RAII
// 对应 ARCH-P0 §3.4 / §1.3 / §1.4.3
// 分层约束：mysql.h 只允许出现在本文件；上层一律通过 Guard 取 DbConn&
// 并发模型：MYSQL* 非线程安全 → 固定容量池 + mutex/condition_variable 做空闲队列
//           一个请求同时最多持有一个 Guard（容量 == 线程池容量，避免全池饥饿）
// =============================================================================
#pragma once

#include <mysql.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Result.h"
#include "Config.h"

namespace rf {

/** 单元格：SQL NULL → nullopt（区分「空串」与「NULL」，§T04 判据） */
using Cell      = std::optional<std::string>;
using Row       = std::vector<Cell>;
using ResultSet = std::vector<Row>;

/** 写语句影响行数与自增主键 */
struct ExecResult {
    int64_t  affectedRows = 0;
    uint64_t insertId     = 0;
};

class DbConn;   // 前向声明
class DbPool;

/**
 * 预处理语句：所有业务 SQL 一律走这里，杜绝拼接注入。
 * 生命周期内占位绑定顺序必须与 SQL 中 ? 的顺序一致。
 */
class Stmt {
public:
    Stmt(DbConn& db, const std::string& sql);
    ~Stmt();
    Stmt(const Stmt&)            = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bindI64(int64_t v);
    Stmt& bindU64(uint64_t v);
    Stmt& bindStr(const std::string& v);
    Stmt& bindDouble(double v);
    Stmt& bindNull();

    /** INSERT / UPDATE / DELETE */
    Result<ExecResult> exec();
    /** SELECT：内部 store_result + 按列最长值分配缓冲（中文不截断） */
    Result<ResultSet>  query();

    /** 最近一次 exec() 的自增主键 */
    uint64_t lastInsertId() const;

    /**
     * 最近一次 exec() / query() 的 MySQL 错误码（0 表示成功）。
     * 用途：识别 ER_DUP_ENTRY(1062) 唯一键冲突（ARCH-P1 D-P1-6）。
     * @return mysql_stmt_errno 的值；语句句柄不可用时返回预处理阶段的 errno
     */
    int errNo() const;

    /** errNo() == 1062（ER_DUP_ENTRY，定义在 mysqld_error.h） */
    bool isDupEntry() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** 事务 RAII：未 commit 则析构 rollback */
class Transaction {
public:
    explicit Transaction(DbConn& db);
    ~Transaction();
    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;

    Result<Empty> commit();
    void          rollback();
    bool          active() const;

private:
    DbConn& db_;
    bool    active_ = true;
};

/** 单个 MySQL 连接（非线程安全，只能由持有 Guard 的线程独占使用） */
class DbConn {
public:
    explicit DbConn(const MysqlConfig& cfg);
    ~DbConn();
    DbConn(const DbConn&)            = delete;
    DbConn& operator=(const DbConn&) = delete;

    /** 建立连接（utf8mb4 + CLIENT_MULTI_RESULTS + 连接超时） */
    bool open(std::string& err);
    void close();
    /** 连接当前是否可用（会做一次 mysql_ping） */
    bool alive();

    MYSQL* raw();

    bool begin();
    bool commit();
    bool rollback();
    /** mysql_ping，失败时关闭并重连一次 */
    bool ping();
    /** 字符串转义，仅供极少数固定 SQL 使用（业务 SQL 必须走 Stmt） */
    std::string escape(const std::string& s);
    /** 固定只读 SQL（探针 / 字典）直接执行，非业务路径 */
    Result<ResultSet> rawQuery(const std::string& sql);

private:
    MYSQL*            my_ = nullptr;
    const MysqlConfig& cfg_;
};

/** 固定容量连接池 */
class DbPool {
public:
    explicit DbPool(const MysqlConfig& cfg);
    ~DbPool();
    DbPool(const DbPool&)            = delete;
    DbPool& operator=(const DbPool&) = delete;

    /** 预建 poolSize 条连接，任一失败即失败并回收已建连接 */
    bool init(std::string& err);
    void shutdown();

    /** 连接租借句柄：move-only，析构自动归还 */
    class Guard {
    public:
        Guard() = default;
        Guard(DbPool* pool, std::unique_ptr<DbConn> c);
        Guard(Guard&&) noexcept;
        Guard& operator=(Guard&&) noexcept;
        ~Guard();
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;

        explicit operator bool() const { return conn_ != nullptr; }
        DbConn& operator*() const { return *conn_; }
        DbConn* operator->() const { return conn_.get(); }

    private:
        DbPool*                   pool_ = nullptr;
        std::unique_ptr<DbConn>   conn_;
    };

    /**
     * 取一条连接。
     * @param timeoutMs 等待空闲连接的上限
     * @return 空 Guard 表示超时（调用方转 kDbError / 1009）
     */
    Guard acquire(int timeoutMs = 3000);

    /**
     * 探针：从池取连接并 ping，超时 1000ms。
     * @param latencyMs 成功时写入耗时（毫秒），失败写 -1
     * @param err       失败时写入可读原因
     */
    bool ping(int* latencyMs, std::string& err);

    size_t totalCount() const;
    size_t idleCount() const;

private:
    friend class Guard;
    void giveBack(std::unique_ptr<DbConn> c);

    MysqlConfig                          cfg_;
    mutable std::mutex                   mu_;   // mutable：const 计数接口需加锁
    std::condition_variable              cv_;
    std::vector<std::unique_ptr<DbConn>> idle_;
    size_t                               total_   = 0;
    bool                                 stopped_ = false;
};

} // namespace rf
