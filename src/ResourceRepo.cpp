// =============================================================================
// src/repo/ResourceRepo.cpp  —  资源仓储实现
// 对应 ARCH-P0 §3.9 / D-4 / D-8 / D-10 / §8.4
// 实现要点：
//   1) 动态 WHERE 只拼「列名 + ?」，所有值走 Stmt 绑定，无注入面；
//   2) 半更新只 SET 提交过的列；一个字段都没提交时直接返回成功，不发 SQL；
//   3) total 用同条件 SELECT COUNT(*) 单独取，不复用列表长度。
// =============================================================================
#include "ResourceRepo.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf::repo {

namespace {

/** 统一列清单，列序必须与 model::Resource::fromRow 的契约一致（13 列） */
constexpr const char* kResourceSelect =
    "SELECT id, code, name, type, status, location, capacity, deposit_amount, description, "
    "owner_admin_id, "
    "DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%s') AS created_at, "
    "DATE_FORMAT(updated_at, '%Y-%m-%dT%H:%i:%s') AS updated_at, "
    "IFNULL(DATE_FORMAT(deleted_at, '%Y-%m-%dT%H:%i:%s'), '') AS deleted_at "
    "FROM `resource` ";

/**
 * D-4 有效预约状态集合。
 * 与 model::isActiveReservationStatus 保持一致，此处以 SQL 字面量形式出现，
 * 不含任何外部输入，属固定常量。
 */
constexpr const char* kActiveReservationStatuses =
    "('PENDING_REVIEW','LOCKED','IN_USE','DUE_RETURN','RETURN_CHECK')";

/** 延迟绑定值：把「列名拼装」与「值绑定」解耦，保证值永远走预处理 */
struct BindValue {
    enum class Kind { Str, I64, Dbl } kind = Kind::Str;
    std::string s;
    int64_t     i = 0;
    double      d = 0.0;

    static BindValue ofStr(const std::string& v) {
        BindValue b;
        b.kind = Kind::Str;
        b.s = v;
        return b;
    }
    static BindValue ofI64(int64_t v) {
        BindValue b;
        b.kind = Kind::I64;
        b.i = v;
        return b;
    }
    static BindValue ofDbl(double v) {
        BindValue b;
        b.kind = Kind::Dbl;
        b.d = v;
        return b;
    }
};

/** 按顺序把 binds 绑定到语句 */
void applyBinds(Stmt& st, const std::vector<BindValue>& binds) {
    for (const auto& b : binds) {
        switch (b.kind) {
            case BindValue::Kind::I64: st.bindI64(b.i); break;
            case BindValue::Kind::Dbl: st.bindDouble(b.d); break;
            case BindValue::Kind::Str:
            default:                   st.bindStr(b.s); break;
        }
    }
}

/** "a AND b" 拼接 */
std::string joinAnd(const std::vector<std::string>& conds) {
    std::string out;
    for (size_t i = 0; i < conds.size(); ++i) {
        if (i != 0) {
            out += " AND ";
        }
        out += conds[i];
    }
    return out;
}

/**
 * 构造 search 的 WHERE 子句（不含关键字 "WHERE"）。
 * 未删除过滤恒定在首位；其余条件按提交情况追加。
 */
std::string buildWhere(const model::ResourceQuery& q, std::vector<BindValue>& binds) {
    std::vector<std::string> conds;
    conds.emplace_back("deleted_at IS NULL");

    if (q.type.has_value()) {
        conds.emplace_back("type = ?");
        binds.push_back(BindValue::ofStr(model::toStr(*q.type)));
    }
    if (q.status.has_value()) {
        conds.emplace_back("status = ?");
        binds.push_back(BindValue::ofStr(model::toStr(*q.status)));
    }
    if (q.location.has_value() && !q.location->empty()) {
        conds.emplace_back("location = ?");
        binds.push_back(BindValue::ofStr(*q.location));
    }
    if (q.keyword.has_value() && !q.keyword->empty()) {
        // 名称或描述命中；通配符加在绑定值里，仍然走预处理
        conds.emplace_back("(name LIKE ? OR description LIKE ?)");
        const std::string pattern = "%" + *q.keyword + "%";
        binds.push_back(BindValue::ofStr(pattern));
        binds.push_back(BindValue::ofStr(pattern));
    }
    return joinAnd(conds);
}

/** 单元格 → 字符串 */
std::string cellStr(const Row& row, size_t i) {
    return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
}

/** 单元格 → 整数 */
int64_t cellI64(const Row& row, size_t i) {
    const std::string v = cellStr(row, i);
    if (v.empty()) {
        return 0;
    }
    return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
}

/** 唯一键冲突判定（并发下 findByCode 判重可能被插队，按 SQL 错误兜底映射 3001） */
bool isDuplicateCodeError(const std::string& msg) {
    return msg.find("Duplicate entry") != std::string::npos ||
           msg.find("uk_resource_code") != std::string::npos;
}

}  // namespace

Result<std::optional<model::Resource>> ResourceRepo::findById(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<std::optional<model::Resource>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kResourceSelect) + "WHERE id = ? AND deleted_at IS NULL LIMIT 1");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::Resource>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::Resource>>::make(std::nullopt);
        }
        return Result<std::optional<model::Resource>>::make(
            std::optional<model::Resource>(fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::Resource>>::fail(
            kDbError, std::string("查询资源失败：") + e.what());
    }
}

Result<std::optional<model::Resource>> ResourceRepo::findByIdAny(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<std::optional<model::Resource>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kResourceSelect) + "WHERE id = ? LIMIT 1");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::Resource>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::Resource>>::make(std::nullopt);
        }
        return Result<std::optional<model::Resource>>::make(
            std::optional<model::Resource>(fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::Resource>>::fail(
            kDbError, std::string("查询资源失败：") + e.what());
    }
}

Result<std::optional<model::Resource>> ResourceRepo::findByCode(DbConn& db,
                                                              const std::string& code) {
    if (code.empty()) {
        return Result<std::optional<model::Resource>>::make(std::nullopt);
    }
    try {
        // 不加大 deleted_at 过滤：编号永久不复用，已删资源的编号也算被占用（D-10）
        Stmt st(db, std::string(kResourceSelect) + "WHERE code = ? LIMIT 1");
        st.bindStr(code);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::Resource>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::Resource>>::make(std::nullopt);
        }
        return Result<std::optional<model::Resource>>::make(
            std::optional<model::Resource>(fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::Resource>>::fail(
            kDbError, std::string("查询资源失败：") + e.what());
    }
}

Result<model::PageResult> ResourceRepo::search(DbConn& db, const model::ResourceQuery& q) {
    // 防御性收敛：Service 已保证范围，这里再兜一次，避免非法 LIMIT
    const int page = (q.page < 1) ? 1 : q.page;
    const int size = (q.size < 1) ? 10 : (q.size > 100 ? 100 : q.size);

    try {
        std::vector<BindValue> binds;
        const std::string where = buildWhere(q, binds);

        // ---- 1) total：同条件单独 COUNT(*)
        int64_t total = 0;
        {
            Stmt cnt(db, std::string("SELECT COUNT(*) AS c FROM `resource` WHERE ") + where);
            applyBinds(cnt, binds);
            auto cr = cnt.query();
            if (!cr.ok()) {
                return Result<model::PageResult>::fail(cr.code, cr.msg);
            }
            if (!cr.data.empty()) {
                total = cellI64(cr.data.front(), 0);
            }
        }

        model::PageResult out;
        out.total = total;
        out.page  = page;
        out.size  = size;
        if (total == 0) {
            return Result<model::PageResult>::make(std::move(out));   // 空结果不再发第二条 SQL
        }

        // ---- 2) 当页数据
        const int64_t offset = static_cast<int64_t>(page - 1) * static_cast<int64_t>(size);
        Stmt lst(db, std::string(kResourceSelect) + "WHERE " + where +
                         " ORDER BY id ASC LIMIT ? OFFSET ?");
        applyBinds(lst, binds);
        lst.bindI64(static_cast<int64_t>(size)).bindI64(offset);
        auto lr = lst.query();
        if (!lr.ok()) {
            return Result<model::PageResult>::fail(lr.code, lr.msg);
        }
        out.list.reserve(lr.data.size());
        for (const auto& row : lr.data) {
            out.list.push_back(fromRow(row));
        }
        return Result<model::PageResult>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<model::PageResult>::fail(kDbError, std::string("检索资源失败：") + e.what());
    }
}

Result<int64_t> ResourceRepo::insert(DbConn& db, const model::Resource& r) {
    try {
        Stmt st(db,
                "INSERT INTO `resource` "
                "(code, name, type, status, location, capacity, deposit_amount, description, "
                " owner_admin_id) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        st.bindStr(r.code)
            .bindStr(r.name)
            .bindStr(model::toStr(r.type))
            .bindStr(model::toStr(r.status))
            .bindStr(r.location)
            .bindI64(static_cast<int64_t>(r.capacity))
            .bindDouble(r.depositAmount)
            .bindStr(r.description);
        if (r.ownerAdminId > 0) {
            st.bindI64(r.ownerAdminId);
        } else {
            st.bindNull();   // owner_admin_id 允许 NULL；不能绑 0（会撞外键）
        }
        auto rr = st.exec();
        if (!rr.ok()) {
            if (isDuplicateCodeError(rr.msg)) {
                return Result<int64_t>::fail(kResourceCodeDup, "资源编号已存在");
            }
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(static_cast<int64_t>(rr.data.insertId));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("新增资源失败：") + e.what());
    }
}

VoidResult ResourceRepo::update(DbConn& db, int64_t id,
                                        const model::ResourceUpdateReq& req) {
    if (id <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：资源 id 非法");
    }
    try {
        // ---- 半更新：只为提交过的字段拼 SET 片段（列名常量，值仍绑定）
        std::vector<std::string> sets;
        std::vector<BindValue>   binds;

        if (req.name.has_value()) {
            sets.emplace_back("name = ?");
            binds.push_back(BindValue::ofStr(*req.name));
        }
        if (req.type.has_value()) {
            sets.emplace_back("type = ?");
            binds.push_back(BindValue::ofStr(model::toStr(*req.type)));
        }
        if (req.location.has_value()) {
            sets.emplace_back("location = ?");
            binds.push_back(BindValue::ofStr(*req.location));
        }
        if (req.capacity.has_value()) {
            sets.emplace_back("capacity = ?");
            binds.push_back(BindValue::ofI64(static_cast<int64_t>(*req.capacity)));
        }
        if (req.depositAmount.has_value()) {
            sets.emplace_back("deposit_amount = ?");
            binds.push_back(BindValue::ofDbl(*req.depositAmount));
        }
        if (req.description.has_value()) {
            sets.emplace_back("description = ?");
            binds.push_back(BindValue::ofStr(*req.description));
        }

        if (sets.empty()) {
            // 一个可变字段都没提交：不发 SQL，按成功处理（对调用方是幂等空操作）
            LOG_DEBUG("ResourceRepo::update 无提交字段，跳过写库（id={}）", id);
            return VoidResult::make(Empty{});
        }

        std::string sql = "UPDATE `resource` SET ";
        for (size_t i = 0; i < sets.size(); ++i) {
            if (i != 0) {
                sql += ", ";
            }
            sql += sets[i];
        }
        sql += " WHERE id = ? AND deleted_at IS NULL";

        Stmt st(db, sql);
        applyBinds(st, binds);
        st.bindI64(id);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        if (rr.data.affectedRows == 0) {
            LOG_DEBUG("ResourceRepo::update 未影响行（id={}）：值未变化或资源已删除", id);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("更新资源失败：") + e.what());
    }
}

VoidResult ResourceRepo::softDelete(DbConn& db, int64_t id) {
    if (id <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：资源 id 非法");
    }
    try {
        Stmt st(db, "UPDATE `resource` SET deleted_at = NOW() WHERE id = ? AND deleted_at IS NULL");
        st.bindI64(id);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("删除资源失败：") + e.what());
    }
}

VoidResult ResourceRepo::updateStatus(DbConn& db, int64_t id, model::ResourceState st) {
    if (id <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：资源 id 非法");
    }
    try {
        Stmt s(db, "UPDATE `resource` SET status = ? WHERE id = ? AND deleted_at IS NULL");
        s.bindStr(model::toStr(st)).bindI64(id);
        auto rr = s.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("更新资源状态失败：") + e.what());
    }
}

Result<int64_t> ResourceRepo::countActiveReservations(DbConn& db, int64_t resourceId) {
    if (resourceId <= 0) {
        return Result<int64_t>::make(0);
    }
    try {
        Stmt st(db, std::string("SELECT COUNT(*) AS c FROM `reservation` WHERE resource_id = ? "
                                "AND status IN ") +
                         kActiveReservationStatuses);
        st.bindI64(resourceId);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<int64_t>::fail(rs.code, rs.msg);
        }
        // 空表 → COUNT(*) 仍返回一行 0；结果集为空时也按 0 处理
        if (rs.data.empty()) {
            return Result<int64_t>::make(0);
        }
        return Result<int64_t>::make(cellI64(rs.data.front(), 0));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("统计有效预约失败：") + e.what());
    }
}

Result<std::vector<std::string>> ResourceRepo::listLocations(DbConn& db) {
    try {
        Stmt st(db,
                "SELECT DISTINCT location FROM `resource` "
                "WHERE deleted_at IS NULL AND location <> '' ORDER BY location ASC");
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<std::string>>::fail(rs.code, rs.msg);
        }
        std::vector<std::string> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            const std::string loc = cellStr(row, 0);
            if (!loc.empty()) {
                out.push_back(loc);
            }
        }
        return Result<std::vector<std::string>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<std::string>>::fail(
            kDbError, std::string("读取地点清单失败：") + e.what());
    }
}

Result<bool> ResourceRepo::existsNotDeleted(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<bool>::make(false);
    }
    try {
        Stmt st(db,
                "SELECT COUNT(*) AS c FROM `resource` WHERE id = ? AND deleted_at IS NULL");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<bool>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<bool>::make(false);
        }
        return Result<bool>::make(cellI64(rs.data.front(), 0) > 0);
    } catch (const std::exception& e) {
        return Result<bool>::fail(kDbError, std::string("查询资源存在性失败：") + e.what());
    }
}

} // namespace rf::repo
