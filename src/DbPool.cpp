// =============================================================================
// src/db/DbPool.cpp  —  MySQL 连接池 / 预处理语句 / 事务实现
// 对应 ARCH-P0 §3.4 / §1.4.3
// 关键点：
//   1) 预处理语句取结果集必须先 store_result，再按列 max_length 分配缓冲，
//      否则中文 description 会被固定 256B 缓冲截断；
//   2) 所有列统一按 MYSQL_TYPE_STRING 绑定，交由 MySQL 做类型转换，
//      避免 DECIMAL / DATETIME 的二进制表示差异；
//   3) 行内 NULL 映射为 std::nullopt，与空串区分。
// =============================================================================
#include "DbPool.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

#include "Base.h"

namespace rf {

namespace {

/** ER_DUP_ENTRY（唯一键冲突）。定义在 mysqld_error.h，mysql.h 不带它，故本地取常量。 */
constexpr int kErDupEntry = 1062;

/** 进程内只初始化一次 MySQL 客户端库 */
void ensureLibraryInit() {
    static std::once_flag once;
    std::call_once(once, []() {
        if (::mysql_library_init(0, nullptr, nullptr) != 0) {
            LOG_ERROR("mysql_library_init 失败");
        }
    });
}

/** 取一条语句的错误摘要（含 SQL 片段，便于定位） */
std::string stmtError(MYSQL_STMT* stmt) {
    if (stmt == nullptr) {
        return "语句句柄为空";
    }
    const char* e = ::mysql_stmt_error(stmt);
    return (e == nullptr) ? std::string("未知错误") : std::string(e);
}

}  // namespace

// ============================================================================
// DbConn
// ============================================================================

DbConn::DbConn(const MysqlConfig& cfg) : cfg_(cfg) {}

DbConn::~DbConn() {
    close();
}

bool DbConn::open(std::string& err) {
    ensureLibraryInit();
    close();

    my_ = ::mysql_init(nullptr);
    if (my_ == nullptr) {
        err = "mysql_init 失败（内存不足）";
        return false;
    }

    unsigned int connTimeout = static_cast<unsigned int>(
        cfg_.connectTimeoutSec > 0 ? cfg_.connectTimeoutSec : 5);
    ::mysql_options(my_, MYSQL_OPT_CONNECT_TIMEOUT, &connTimeout);

    const std::string charset = cfg_.charset.empty() ? std::string("utf8mb4") : cfg_.charset;
    ::mysql_options(my_, MYSQL_SET_CHARSET_NAME, charset.c_str());

    MYSQL* ok = ::mysql_real_connect(my_,
                                     cfg_.host.c_str(),
                                     cfg_.user.c_str(),
                                     cfg_.password.c_str(),
                                     cfg_.database.c_str(),
                                     static_cast<unsigned int>(cfg_.port),
                                     nullptr,
                                     CLIENT_MULTI_RESULTS);
    if (ok == nullptr) {
        err = std::string("连接 MySQL 失败（") + cfg_.host + ":" + std::to_string(cfg_.port) +
              "/" + cfg_.database + "）：" + ::mysql_error(my_);
        ::mysql_close(my_);
        my_ = nullptr;
        return false;
    }
    return true;
}

void DbConn::close() {
    if (my_ != nullptr) {
        ::mysql_close(my_);
        my_ = nullptr;
    }
}

bool DbConn::alive() {
    if (my_ == nullptr) {
        return false;
    }
    return ::mysql_ping(my_) == 0;
}

MYSQL* DbConn::raw() { return my_; }

bool DbConn::begin() {
    if (my_ == nullptr) {
        return false;
    }
    return ::mysql_query(my_, "START TRANSACTION") == 0;
}

bool DbConn::commit() {
    if (my_ == nullptr) {
        return false;
    }
    return ::mysql_query(my_, "COMMIT") == 0;
}

bool DbConn::rollback() {
    if (my_ == nullptr) {
        return false;
    }
    return ::mysql_query(my_, "ROLLBACK") == 0;
}

bool DbConn::ping() {
    if (my_ == nullptr) {
        std::string err;
        return open(err);
    }
    if (::mysql_ping(my_) == 0) {
        return true;
    }
    // 断线：关闭后重连一次（§8.7 MySQL 不可用不自杀，按需重连）
    const std::string reason = ::mysql_error(my_);
    close();
    std::string err;
    if (open(err)) {
        LOG_WARN("MySQL 连接已重连（原错误：{}）", reason);
        return true;
    }
    LOG_WARN("MySQL 重连失败：{}", err);
    return false;
}

std::string DbConn::escape(const std::string& s) {
    if (my_ == nullptr) {
        return std::string();
    }
    std::string out;
    out.resize(s.size() * 2 + 1);
    const unsigned long n = ::mysql_real_escape_string(
        my_, out.data(), s.c_str(), static_cast<unsigned long>(s.size()));
    out.resize(n);
    return out;
}

Result<ResultSet> DbConn::rawQuery(const std::string& sql) {
    if (my_ == nullptr) {
        return Result<ResultSet>::fail(kDbError, "数据库连接不可用");
    }
    if (::mysql_real_query(my_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        return Result<ResultSet>::fail(kDbError, std::string("SQL 执行失败：") + ::mysql_error(my_));
    }
    MYSQL_RES* res = ::mysql_store_result(my_);
    if (res == nullptr) {
        if (::mysql_field_count(my_) == 0) {
            return Result<ResultSet>::make(ResultSet{});   // 非 SELECT
        }
        return Result<ResultSet>::fail(kDbError, std::string("读取结果集失败：") + ::mysql_error(my_));
    }

    const unsigned int numFields = ::mysql_num_fields(res);
    ResultSet out;
    MYSQL_ROW row = nullptr;
    while ((row = ::mysql_fetch_row(res)) != nullptr) {
        const unsigned long* lens = ::mysql_fetch_lengths(res);
        Row r;
        r.reserve(numFields);
        for (unsigned int i = 0; i < numFields; ++i) {
            if (row[i] == nullptr) {
                r.emplace_back(std::nullopt);
            } else {
                r.emplace_back(std::string(row[i], lens == nullptr ? 0 : lens[i]));
            }
        }
        out.push_back(std::move(r));
    }
    ::mysql_free_result(res);
    return Result<ResultSet>::make(std::move(out));
}

// ============================================================================
// Transaction
// ============================================================================

Transaction::Transaction(DbConn& db) : db_(db) {
    if (!db_.begin()) {
        active_ = false;   // 开启失败：后续 commit() 返回失败，析构不再 rollback
        LOG_ERROR("事务开启失败：无法执行 START TRANSACTION");
    }
}

Transaction::~Transaction() {
    if (active_) {
        db_.rollback();   // 未显式 commit → 回滚
    }
}

Result<Empty> Transaction::commit() {
    if (!active_) {
        return Result<Empty>::fail(kDbError, "事务未激活，无法提交");
    }
    if (!db_.commit()) {
        active_ = false;
        return Result<Empty>::fail(kDbError, "事务提交失败");
    }
    active_ = false;
    return Result<Empty>::make(Empty{});
}

void Transaction::rollback() {
    if (active_) {
        db_.rollback();
        active_ = false;
    }
}

bool Transaction::active() const { return active_; }

// ============================================================================
// Stmt
// ============================================================================

struct Stmt::Impl {
    enum class Kind { I64, U64, Str, Dbl, Null };

    struct Param {
        Kind          kind = Kind::Null;
        int64_t       i64  = 0;
        uint64_t      u64  = 0;
        double        dbl  = 0.0;
        std::string   str;
    };

    DbConn&                db;
    MYSQL_STMT*            stmt    = nullptr;
    unsigned long          expectedParams = 0;
    std::string            error;          // 构造期（prepare）错误
    int                    lastErrno = 0;  // 最近一次 exec/query 的 mysql_stmt_errno（D-P1-6）
    std::vector<Param>     params;

    explicit Impl(DbConn& d, const std::string& sql) : db(d) {
        MYSQL* my = db.raw();
        if (my == nullptr) {
            error = "数据库连接不可用";
            return;
        }
        stmt = ::mysql_stmt_init(my);
        if (stmt == nullptr) {
            error = "mysql_stmt_init 失败";
            return;
        }
        if (::mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
            // mysql_stmt_errno 返回 unsigned int，赋给 int 必须显式转换（否则 MSVC C4244）
            lastErrno = static_cast<int>(::mysql_stmt_errno(stmt));
            error = "SQL 预处理失败：" + stmtError(stmt);
            ::mysql_stmt_close(stmt);
            stmt = nullptr;
            return;
        }
        expectedParams = ::mysql_stmt_param_count(stmt);
    }

    ~Impl() {
        if (stmt != nullptr) {
            ::mysql_stmt_close(stmt);
            stmt = nullptr;
        }
    }

    /**
     * 按 params 构造 MYSQL_BIND 数组并绑定为输入参数。
     * 绑定缓冲直接指向 Param 内部字段（调用期间 params 不被修改，指针稳定）。
     * @return 成功返回空串，失败返回原因
     */
    std::string bindParams(std::vector<MYSQL_BIND>& binds,
                           std::vector<unsigned long>& lens,
                           std::unique_ptr<bool[]>& nullFlags) {
        const size_t n = params.size();
        binds.assign(n, MYSQL_BIND{});
        lens.assign(n, 0);
        nullFlags = std::unique_ptr<bool[]>(new bool[n == 0 ? 1 : n]);
        for (size_t i = 0; i < n; ++i) {
            nullFlags[i] = false;
        }

        for (size_t i = 0; i < n; ++i) {
            MYSQL_BIND& b = binds[i];
            std::memset(&b, 0, sizeof(MYSQL_BIND));
            Param& p = params[i];
            switch (p.kind) {
                case Kind::I64:
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    b.buffer      = &p.i64;
                    b.is_unsigned = false;
                    break;
                case Kind::U64:
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    b.buffer      = &p.u64;
                    b.is_unsigned = true;
                    break;
                case Kind::Dbl:
                    b.buffer_type = MYSQL_TYPE_DOUBLE;
                    b.buffer      = &p.dbl;
                    break;
                case Kind::Str:
                    b.buffer_type   = MYSQL_TYPE_STRING;
                    b.buffer        = const_cast<char*>(p.str.data());
                    b.buffer_length = static_cast<unsigned long>(p.str.size());
                    lens[i]         = static_cast<unsigned long>(p.str.size());
                    b.length        = &lens[i];
                    break;
                case Kind::Null:
                default:
                    b.buffer_type = MYSQL_TYPE_NULL;
                    nullFlags[i]  = true;
                    b.is_null     = &nullFlags[i];
                    break;
            }
        }

        if (::mysql_stmt_bind_param(stmt, binds.data()) != 0) {
            return "参数绑定失败：" + stmtError(stmt);
        }
        return std::string();
    }
};

Stmt::Stmt(DbConn& db, const std::string& sql) : impl_(std::make_unique<Impl>(db, sql)) {}

Stmt::~Stmt() = default;

Stmt& Stmt::bindI64(int64_t v) {
    Impl::Param p;
    p.kind = Impl::Kind::I64;
    p.i64  = v;
    impl_->params.push_back(p);
    return *this;
}

Stmt& Stmt::bindU64(uint64_t v) {
    Impl::Param p;
    p.kind = Impl::Kind::U64;
    p.u64  = v;
    impl_->params.push_back(p);
    return *this;
}

Stmt& Stmt::bindStr(const std::string& v) {
    Impl::Param p;
    p.kind = Impl::Kind::Str;
    p.str  = v;
    impl_->params.push_back(p);
    return *this;
}

Stmt& Stmt::bindDouble(double v) {
    Impl::Param p;
    p.kind = Impl::Kind::Dbl;
    p.dbl  = v;
    impl_->params.push_back(p);
    return *this;
}

Stmt& Stmt::bindNull() {
    Impl::Param p;
    p.kind = Impl::Kind::Null;
    impl_->params.push_back(p);
    return *this;
}

Result<ExecResult> Stmt::exec() {
    Impl& im = *impl_;
    if (im.stmt == nullptr) {
        return Result<ExecResult>::fail(kDbError, im.error.empty() ? "语句不可用" : im.error);
    }
    if (im.params.size() != im.expectedParams) {
        return Result<ExecResult>::fail(
            kDbError, "参数个数不匹配：期望 " + std::to_string(im.expectedParams) +
                          "，实际 " + std::to_string(im.params.size()));
    }

    std::vector<MYSQL_BIND>    binds;
    std::vector<unsigned long> lens;
    std::unique_ptr<bool[]>    nullFlags;
    const std::string bindErr = im.bindParams(binds, lens, nullFlags);
    if (!bindErr.empty()) {
        return Result<ExecResult>::fail(kDbError, bindErr);
    }

    ::mysql_stmt_free_result(im.stmt);
    im.lastErrno = 0;
    if (::mysql_stmt_execute(im.stmt) != 0) {
        im.lastErrno = static_cast<int>(::mysql_stmt_errno(im.stmt));
        return Result<ExecResult>::fail(kDbError, "SQL 执行失败：" + stmtError(im.stmt));
    }

    ExecResult r;
    const my_ulonglong affected = ::mysql_stmt_affected_rows(im.stmt);
    r.affectedRows = (affected == static_cast<my_ulonglong>(-1))
                         ? 0
                         : static_cast<int64_t>(affected);
    r.insertId = static_cast<uint64_t>(::mysql_stmt_insert_id(im.stmt));

    // INSERT 语句取完 insert_id 后同样要 store_result 才能继续复用连接
    if (::mysql_stmt_field_count(im.stmt) > 0) {
        ::mysql_stmt_store_result(im.stmt);
        ::mysql_stmt_free_result(im.stmt);
    }
    return Result<ExecResult>::make(r);
}

Result<ResultSet> Stmt::query() {
    Impl& im = *impl_;
    if (im.stmt == nullptr) {
        return Result<ResultSet>::fail(kDbError, im.error.empty() ? "语句不可用" : im.error);
    }
    if (im.params.size() != im.expectedParams) {
        return Result<ResultSet>::fail(
            kDbError, "参数个数不匹配：期望 " + std::to_string(im.expectedParams) +
                          "，实际 " + std::to_string(im.params.size()));
    }

    std::vector<MYSQL_BIND>    binds;
    std::vector<unsigned long> lens;
    std::unique_ptr<bool[]>    nullFlags;
    const std::string bindErr = im.bindParams(binds, lens, nullFlags);
    if (!bindErr.empty()) {
        return Result<ResultSet>::fail(kDbError, bindErr);
    }

    // STMT_ATTR_UPDATE_MAX_LENGTH 必须在 execute 之前设置，否则 metadata 里
    // 的 max_length 恒为 0，按列分配缓冲就失去意义（§1.4.3）
    bool wantMaxLength = true;
    ::mysql_stmt_attr_set(im.stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &wantMaxLength);

    ::mysql_stmt_free_result(im.stmt);
    im.lastErrno = 0;
    if (::mysql_stmt_execute(im.stmt) != 0) {
        im.lastErrno = static_cast<int>(::mysql_stmt_errno(im.stmt));
        return Result<ResultSet>::fail(kDbError, "SQL 执行失败：" + stmtError(im.stmt));
    }
    if (::mysql_stmt_store_result(im.stmt) != 0) {
        im.lastErrno = static_cast<int>(::mysql_stmt_errno(im.stmt));
        return Result<ResultSet>::fail(kDbError, "结果集缓冲失败：" + stmtError(im.stmt));
    }

    MYSQL_RES* meta = ::mysql_stmt_result_metadata(im.stmt);
    if (meta == nullptr) {
        if (::mysql_stmt_field_count(im.stmt) == 0) {
            return Result<ResultSet>::make(ResultSet{});   // 无结果列
        }
        return Result<ResultSet>::fail(kDbError, "读取结果集元数据失败：" + stmtError(im.stmt));
    }

    const unsigned int numFields = ::mysql_num_fields(meta);
    MYSQL_FIELD*       fields    = ::mysql_fetch_fields(meta);

    std::vector<std::vector<char>> buffers(numFields);
    std::vector<unsigned long>     outLens(numFields, 0);
    std::vector<MYSQL_BIND>        outBinds(numFields);
    std::unique_ptr<bool[]>        outNull(new bool[numFields == 0 ? 1 : numFields]);
    std::unique_ptr<bool[]>        outErr(new bool[numFields == 0 ? 1 : numFields]);
    for (unsigned int i = 0; i < numFields; ++i) {
        outNull[i] = false;
        outErr[i]  = false;
    }

    // 初次按列 max_length 分配；若仍被截断则放大后重取（中文 description 保护）
    for (unsigned int i = 0; i < numFields; ++i) {
        size_t want = static_cast<size_t>(fields[i].max_length) + 1;
        if (want < 1024) {
            want = 1024;
        }
        buffers[i].assign(want, '\0');
    }

    ResultSet rows;
    bool      truncated = false;
    std::string fetchErr;

    for (int attempt = 0; attempt < 3; ++attempt) {
        rows.clear();
        truncated = false;
        fetchErr.clear();

        for (unsigned int i = 0; i < numFields; ++i) {
            std::memset(&outBinds[i], 0, sizeof(MYSQL_BIND));
            outNull[i] = false;
            outErr[i]  = false;
            // 统一按字符串取值：MySQL 负责把 DECIMAL/DATETIME 转成文本
            outBinds[i].buffer_type   = MYSQL_TYPE_STRING;
            outBinds[i].buffer        = buffers[i].data();
            outBinds[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
            outBinds[i].length        = &outLens[i];
            outBinds[i].is_null       = &outNull[i];
            outBinds[i].error         = &outErr[i];
        }
        if (::mysql_stmt_bind_result(im.stmt, outBinds.data()) != 0) {
            fetchErr = "结果绑定失败：" + stmtError(im.stmt);
            break;
        }

        int rc = 0;
        while ((rc = ::mysql_stmt_fetch(im.stmt)) == 0 || rc == MYSQL_DATA_TRUNCATED) {
            if (rc == MYSQL_DATA_TRUNCATED) {
                truncated = true;
                break;
            }
            Row row;
            row.reserve(numFields);
            for (unsigned int i = 0; i < numFields; ++i) {
                if (outNull[i]) {
                    row.emplace_back(std::nullopt);
                } else {
                    row.emplace_back(std::string(buffers[i].data(),
                                                 static_cast<size_t>(outLens[i])));
                }
            }
            rows.push_back(std::move(row));
        }

        if (truncated) {
            // 按实际长度放大缓冲，随后重新 execute 再取（最多 3 轮）
            for (unsigned int i = 0; i < numFields; ++i) {
                const size_t need = static_cast<size_t>(outLens[i]) + 1;
                if (need > buffers[i].size()) {
                    buffers[i].assign(need, '\0');
                }
            }
            ::mysql_stmt_free_result(im.stmt);
            ::mysql_stmt_attr_set(im.stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &wantMaxLength);
            if (!im.bindParams(binds, lens, nullFlags).empty()) {
                fetchErr = "重新绑定参数失败";
                break;
            }
            if (::mysql_stmt_execute(im.stmt) != 0) {
                im.lastErrno = static_cast<int>(::mysql_stmt_errno(im.stmt));
                fetchErr = "重新执行失败：" + stmtError(im.stmt);
                break;
            }
            if (::mysql_stmt_store_result(im.stmt) != 0) {
                im.lastErrno = static_cast<int>(::mysql_stmt_errno(im.stmt));
                fetchErr = "重新缓冲结果集失败：" + stmtError(im.stmt);
                break;
            }
            continue;
        }

        if (rc != MYSQL_NO_DATA && rc != 0) {
            fetchErr = "读取结果行失败：" + stmtError(im.stmt);
        }
        break;
    }

    ::mysql_free_result(meta);
    ::mysql_stmt_free_result(im.stmt);

    if (!fetchErr.empty()) {
        return Result<ResultSet>::fail(kDbError, fetchErr);
    }
    if (truncated) {
        return Result<ResultSet>::fail(kDbError, "结果列超长，多次放大缓冲后仍被截断");
    }
    return Result<ResultSet>::make(std::move(rows));
}

uint64_t Stmt::lastInsertId() const {
    if (impl_ == nullptr || impl_->stmt == nullptr) {
        return 0;
    }
    return static_cast<uint64_t>(::mysql_stmt_insert_id(impl_->stmt));
}

int Stmt::errNo() const {
    if (impl_ == nullptr) {
        return 0;
    }
    return impl_->lastErrno;
}

bool Stmt::isDupEntry() const { return errNo() == kErDupEntry; }

// ============================================================================
// DbPool
// ============================================================================

DbPool::DbPool(const MysqlConfig& cfg) : cfg_(cfg) {}

DbPool::~DbPool() {
    shutdown();
}

bool DbPool::init(std::string& err) {
    err.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!idle_.empty()) {
            return true;   // 已初始化
        }
        stopped_ = false;
    }

    const int want = cfg_.poolSize > 0 ? cfg_.poolSize : 8;
    std::vector<std::unique_ptr<DbConn>> built;
    built.reserve(static_cast<size_t>(want));

    for (int i = 0; i < want; ++i) {
        auto conn = std::make_unique<DbConn>(cfg_);
        std::string openErr;
        if (!conn->open(openErr)) {
            err = "第 " + std::to_string(i + 1) + " 条连接建立失败：" + openErr;
            built.clear();   // unique_ptr 析构自动关闭
            return false;
        }
        built.push_back(std::move(conn));
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& c : built) {
            idle_.push_back(std::move(c));
        }
        total_ = idle_.size();
    }
    LOG_INFO("DbPool 初始化完成：{} 条连接 → {}:{}/{}", total_,
             cfg_.host, cfg_.port, cfg_.database);
    return true;
}

void DbPool::shutdown() {
    std::vector<std::unique_ptr<DbConn>> released;
    bool alreadyStopped = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        alreadyStopped = stopped_;
        stopped_ = true;
        released.swap(idle_);
        total_ = 0;
    }
    cv_.notify_all();
    released.clear();   // 在锁外析构，避免 mysql_close 期间持锁
    if (!alreadyStopped) {
        LOG_INFO("DbPool 已关闭");   // 析构里会再调一次 shutdown，避免重复日志
    }
}

DbPool::Guard DbPool::acquire(int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
    while (true) {
        if (stopped_) {
            return Guard();
        }
        if (!idle_.empty()) {
            std::unique_ptr<DbConn> conn = std::move(idle_.back());
            idle_.pop_back();
            return Guard(this, std::move(conn));
        }
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            if (!idle_.empty() && !stopped_) {
                std::unique_ptr<DbConn> conn = std::move(idle_.back());
                idle_.pop_back();
                return Guard(this, std::move(conn));
            }
            return Guard();
        }
    }
}

bool DbPool::ping(int* latencyMs, std::string& err) {
    err.clear();
    if (latencyMs != nullptr) {
        *latencyMs = -1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    Guard guard = acquire(1000);
    if (!guard) {
        err = "获取数据库连接超时（1000ms）";
        return false;
    }
    const bool ok = guard->ping();
    const auto t1 = std::chrono::steady_clock::now();
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    if (!ok) {
        err = "MySQL ping 失败";
        return false;
    }
    if (latencyMs != nullptr) {
        *latencyMs = ms;
    }
    return true;
}

size_t DbPool::totalCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return total_;
}

size_t DbPool::idleCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return idle_.size();
}

void DbPool::giveBack(std::unique_ptr<DbConn> c) {
    if (!c) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_) {
            total_ = total_ > 0 ? total_ - 1 : 0;
            // 锁外析构：先移出作用域
        } else {
            idle_.push_back(std::move(c));
        }
    }
    cv_.notify_one();
    // stopped_ 分支下 c 仍持有连接，在此（锁外）析构
    c.reset();
}

DbPool::Guard::Guard(DbPool* pool, std::unique_ptr<DbConn> c)
    : pool_(pool), conn_(std::move(c)) {}

DbPool::Guard::Guard(Guard&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)) {
    other.pool_ = nullptr;
}

DbPool::Guard& DbPool::Guard::operator=(Guard&& other) noexcept {
    if (this != &other) {
        if (pool_ != nullptr && conn_) {
            pool_->giveBack(std::move(conn_));
        }
        pool_ = other.pool_;
        conn_ = std::move(other.conn_);
        other.pool_ = nullptr;
    }
    return *this;
}

DbPool::Guard::~Guard() {
    if (pool_ != nullptr && conn_) {
        pool_->giveBack(std::move(conn_));
    }
}

} // namespace rf
