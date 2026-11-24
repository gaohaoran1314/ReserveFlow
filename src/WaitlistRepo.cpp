// =============================================================================
// src/WaitlistRepo.cpp  —  候补队列 SQL 实现
// 对应 ARCH-P2 §3.5 / §7.2 / §7.4
//
// 实现要点：
//   1) 选人 SQL 的窗口判定是「完整包含」（start_index >= from AND end_index <= to），
//      不是「任意重叠」：候补要的是完整窗口，只释放一半对它没意义；
//   2) excludeIds 必须生效，否则 LIMIT 1 会在同一条候选上死循环；
//   3) FOR UPDATE 只用在事务内（confirm / dispatch），避免长事务持锁。
// =============================================================================
#include "WaitlistRepo.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf::repo {

namespace {

/**
 * 12 列主选择串（列序必须与 model::WaitlistItem::fromRow 契约一致），
 * 后面统一再拼两列资源展示字段（第 13/14 列）。
 */
constexpr const char* kWaitSelectBase =
    "SELECT w.id, w.resource_id, w.user_id, DATE_FORMAT(w.slot_date, '%Y-%m-%d') AS slot_date, "
    "w.start_index, w.end_index, w.status, "
    "IFNULL(DATE_FORMAT(w.offer_expires_at, '%Y-%m-%dT%H:%i:%s'), '') AS offer_expires_at, "
    "IFNULL(w.converted_reservation_id, 0) AS converted_reservation_id, "
    "w.remark, "
    "DATE_FORMAT(w.created_at, '%Y-%m-%dT%H:%i:%s') AS created_at, "
    "DATE_FORMAT(w.updated_at, '%Y-%m-%dT%H:%i:%s') AS updated_at, "
    "res.code, res.name "
    "FROM `waitlist` w "
    "JOIN `resource` res ON res.id = w.resource_id ";

std::string cellStr(const Row& row, size_t i) {
    return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
}

int64_t cellI64(const Row& row, size_t i) {
    const std::string v = cellStr(row, i);
    if (v.empty()) {
        return 0;
    }
    return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
}

/** 行 → 实体（前 12 列走 fromRow，后 2 列是资源展示字段） */
model::WaitlistItem fromRow(const Row& row) {
    model::WaitlistItem w = model::WaitlistItem::fromRow(row);
    if (row.size() >= 14) {
        w.resourceCode = cellStr(row, 12);
        w.resourceName = cellStr(row, 13);
    }
    return w;
}

/** ISO("2026-02-10T09:00:00") → SQL DATETIME 字面量 */
std::string isoToSql(const std::string& iso) {
    std::string v = iso;
    if (v.size() >= 11 && (v[10] == 'T' || v[10] == 't')) {
        v[10] = ' ';
    }
    return v;
}

}  // namespace

Result<int64_t> WaitlistRepo::insert(DbConn& db, const model::WaitlistItem& w) {
    if (w.resourceId <= 0 || w.userId <= 0 || w.slotDate.empty()) {
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：候补记录的资源/用户/日期非法");
    }
    if (w.startIndex < 0 || w.endIndex > 48 || w.startIndex >= w.endIndex) {
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：候补时段区间非法");
    }
    try {
        Stmt st(db,
                "INSERT INTO `waitlist` "
                "(resource_id, user_id, slot_date, start_index, end_index, status, remark) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)");
        st.bindI64(w.resourceId)
            .bindI64(w.userId)
            .bindStr(w.slotDate)
            .bindI64(static_cast<int64_t>(w.startIndex))
            .bindI64(static_cast<int64_t>(w.endIndex))
            .bindStr(model::toStr(w.status))
            .bindStr(w.remark);
        auto rr = st.exec();
        if (!rr.ok()) {
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(static_cast<int64_t>(rr.data.insertId));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("加入候补失败：") + e.what());
    }
}

Result<std::optional<model::WaitlistItem>> WaitlistRepo::findById(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<std::optional<model::WaitlistItem>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kWaitSelectBase) + "WHERE w.id = ?");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::WaitlistItem>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::WaitlistItem>>::make(std::nullopt);
        }
        return Result<std::optional<model::WaitlistItem>>::make(
            std::optional<model::WaitlistItem>(fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::WaitlistItem>>::fail(
            kDbError, std::string("查询候补失败：") + e.what());
    }
}

Result<std::optional<model::WaitlistItem>> WaitlistRepo::findByIdForUpdate(DbConn& db,
                                                                          int64_t id) {
    if (id <= 0) {
        return Result<std::optional<model::WaitlistItem>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kWaitSelectBase) + "WHERE w.id = ? FOR UPDATE");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::WaitlistItem>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::WaitlistItem>>::make(std::nullopt);
        }
        return Result<std::optional<model::WaitlistItem>>::make(
            std::optional<model::WaitlistItem>(fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::WaitlistItem>>::fail(
            kDbError, std::string("查询候补（FOR UPDATE）失败：") + e.what());
    }
}

Result<WaitlistPage> WaitlistRepo::search(DbConn& db, const WaitlistQuery& q) {
    const int page = (q.page < 1) ? 1 : q.page;
    const int size = (q.size < 1) ? 10 : (q.size > 100 ? 100 : q.size);

    try {
        std::string where;
        auto addCond = [&where](const char* cond) {
            where += where.empty() ? "" : " AND ";
            where += cond;
        };
        if (q.userId.has_value()) {
            addCond("w.user_id = ?");
        }
        if (q.resourceId.has_value()) {
            addCond("w.resource_id = ?");
        }
        if (q.status.has_value() && !q.status->empty()) {
            addCond("w.status = ?");
        }
        auto bindFilters = [&q](Stmt& st) {
            if (q.userId.has_value()) {
                st.bindI64(*q.userId);
            }
            if (q.resourceId.has_value()) {
                st.bindI64(*q.resourceId);
            }
            if (q.status.has_value() && !q.status->empty()) {
                st.bindStr(*q.status);
            }
        };
        const std::string whereSql = where.empty() ? std::string("1=1") : where;

        int64_t total = 0;
        {
            Stmt cnt(db, std::string("SELECT COUNT(*) AS c FROM `waitlist` w WHERE ") + whereSql);
            bindFilters(cnt);
            auto cr = cnt.query();
            if (!cr.ok()) {
                return Result<WaitlistPage>::fail(cr.code, cr.msg);
            }
            if (!cr.data.empty()) {
                total = cellI64(cr.data.front(), 0);
            }
        }

        WaitlistPage out;
        out.total = total;
        out.page  = page;
        out.size  = size;
        if (total == 0) {
            return Result<WaitlistPage>::make(std::move(out));
        }

        const int64_t offset = static_cast<int64_t>(page - 1) * static_cast<int64_t>(size);
        Stmt lst(db, std::string(kWaitSelectBase) + "WHERE " + whereSql +
                         " ORDER BY w.created_at ASC, w.id ASC LIMIT ? OFFSET ?");
        bindFilters(lst);
        lst.bindI64(static_cast<int64_t>(size)).bindI64(offset);
        auto lr = lst.query();
        if (!lr.ok()) {
            return Result<WaitlistPage>::fail(lr.code, lr.msg);
        }
        out.list.reserve(lr.data.size());
        for (const auto& row : lr.data) {
            out.list.push_back(fromRow(row));
        }
        return Result<WaitlistPage>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<WaitlistPage>::fail(kDbError, std::string("检索候补失败：") + e.what());
    }
}

Result<bool> WaitlistRepo::existsWaiting(DbConn& db, int64_t resourceId, int64_t userId,
                                         const std::string& slotDate, int startIndex,
                                         int endIndex) {
    if (resourceId <= 0 || userId <= 0 || slotDate.empty()) {
        return Result<bool>::make(false);
    }
    try {
        Stmt st(db,
                "SELECT COUNT(*) AS c FROM `waitlist` WHERE resource_id = ? AND user_id = ? "
                "AND slot_date = ? AND start_index = ? AND end_index = ? AND status = 'WAITING'");
        st.bindI64(resourceId)
            .bindI64(userId)
            .bindStr(slotDate)
            .bindI64(static_cast<int64_t>(startIndex))
            .bindI64(static_cast<int64_t>(endIndex));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<bool>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<bool>::make(false);
        }
        return Result<bool>::make(cellI64(rs.data.front(), 0) > 0);
    } catch (const std::exception& e) {
        return Result<bool>::fail(kDbError, std::string("检查候补重复失败：") + e.what());
    }
}

Result<int64_t> WaitlistRepo::countActiveByUser(DbConn& db, int64_t userId) {
    if (userId <= 0) {
        return Result<int64_t>::make(0);
    }
    try {
        Stmt st(db,
                "SELECT COUNT(*) AS c FROM `waitlist` WHERE user_id = ? "
                "AND status IN ('WAITING','OFFERED')");
        st.bindI64(userId);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<int64_t>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<int64_t>::make(0);
        }
        return Result<int64_t>::make(cellI64(rs.data.front(), 0));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("统计用户候补数失败：") + e.what());
    }
}

Result<int64_t> WaitlistRepo::positionOf(DbConn& db, const model::WaitlistItem& w) {
    if (w.resourceId <= 0 || w.slotDate.empty() || w.status != model::WaitlistStatus::WAITING) {
        return Result<int64_t>::make(0);
    }
    try {
        Stmt st(db,
                "SELECT COUNT(*) AS c FROM `waitlist` "
                "WHERE resource_id = ? AND slot_date = ? AND status = 'WAITING' "
                "AND (created_at < ? OR (created_at = ? AND id <= ?))");
        st.bindI64(w.resourceId)
            .bindStr(w.slotDate)
            .bindStr(isoToSql(w.createdAt))
            .bindStr(isoToSql(w.createdAt))
            .bindI64(w.id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<int64_t>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<int64_t>::make(0);
        }
        return Result<int64_t>::make(cellI64(rs.data.front(), 0));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("计算候补名次失败：") + e.what());
    }
}

Result<std::optional<model::WaitlistItem>> WaitlistRepo::pickFirstEligible(
    DbConn& db, int64_t resourceId, const std::string& slotDate, int fromIndex, int toIndex,
    bool forUpdate, const std::vector<int64_t>& excludeIds) {
    if (resourceId <= 0 || slotDate.empty() || fromIndex >= toIndex) {
        return Result<std::optional<model::WaitlistItem>>::make(std::nullopt);
    }
    try {
        std::string sql = std::string(kWaitSelectBase) +
                          "WHERE w.resource_id = ? AND w.slot_date = ? AND w.status = 'WAITING' "
                          "AND w.start_index >= ? AND w.end_index <= ?";
        if (!excludeIds.empty()) {
            sql += " AND w.id NOT IN (";
            for (size_t i = 0; i < excludeIds.size(); ++i) {
                sql += (i == 0) ? "?" : ",?";
            }
            sql += ")";
        }
        sql += " ORDER BY w.created_at ASC, w.id ASC LIMIT 1";
        if (forUpdate) {
            sql += " FOR UPDATE";
        }

        Stmt st(db, sql);
        st.bindI64(resourceId)
            .bindStr(slotDate)
            .bindI64(static_cast<int64_t>(fromIndex))
            .bindI64(static_cast<int64_t>(toIndex));
        for (const int64_t id : excludeIds) {
            st.bindI64(id);
        }
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::WaitlistItem>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::WaitlistItem>>::make(std::nullopt);
        }
        return Result<std::optional<model::WaitlistItem>>::make(
            std::optional<model::WaitlistItem>(fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::WaitlistItem>>::fail(
            kDbError, std::string("选出候补失败：") + e.what());
    }
}

Result<bool> WaitlistRepo::hasOfferedOverlap(DbConn& db, int64_t resourceId,
                                            const std::string& slotDate, int fromIndex, int toIndex) {
    if (resourceId <= 0 || slotDate.empty() || fromIndex >= toIndex) {
        return Result<bool>::make(false);
    }
    try {
        Stmt st(db,
                "SELECT COUNT(*) AS c FROM `waitlist` WHERE resource_id = ? AND slot_date = ? "
                "AND status = 'OFFERED' AND start_index < ? AND end_index > ?");
        st.bindI64(resourceId)
            .bindStr(slotDate)
            .bindI64(static_cast<int64_t>(toIndex))
            .bindI64(static_cast<int64_t>(fromIndex));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<bool>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<bool>::make(false);
        }
        return Result<bool>::make(cellI64(rs.data.front(), 0) > 0);
    } catch (const std::exception& e) {
        return Result<bool>::fail(kDbError, std::string("检查 offer 重叠失败：") + e.what());
    }
}

Result<int64_t> WaitlistRepo::casUpdateStatus(DbConn& db, int64_t id,
                                              const std::string& fromStatus,
                                              const std::string& toStatus,
                                              const std::optional<std::string>& offerExpiresAt,
                                              const std::optional<int64_t>& convertedReservationId) {
    if (id <= 0 || fromStatus.empty() || toStatus.empty()) {
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：候补 CAS 参数非法");
    }
    try {
        std::string sql = "UPDATE `waitlist` SET status = ?";
        if (offerExpiresAt.has_value()) {
            sql += ", offer_expires_at = ?";
        }
        if (convertedReservationId.has_value()) {
            sql += ", converted_reservation_id = ?";
        }
        sql += " WHERE id = ? AND status = ?";

        Stmt st(db, sql);
        st.bindStr(toStatus);
        if (offerExpiresAt.has_value()) {
            st.bindStr(isoToSql(*offerExpiresAt));
        }
        if (convertedReservationId.has_value()) {
            st.bindI64(*convertedReservationId);
        }
        st.bindI64(id).bindStr(fromStatus);

        auto rr = st.exec();
        if (!rr.ok()) {
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(rr.data.affectedRows);
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("候补状态 CAS 失败：") + e.what());
    }
}

Result<std::vector<model::WaitlistItem>> WaitlistRepo::listExpiredOffers(DbConn& db,
                                                                        const std::string& nowIso,
                                                                        int limit) {
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    try {
        Stmt st(db, std::string(kWaitSelectBase) +
                        "WHERE w.status = 'OFFERED' AND w.offer_expires_at < ? "
                        "ORDER BY w.offer_expires_at ASC LIMIT ?");
        st.bindStr(isoToSql(nowIso)).bindI64(static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<model::WaitlistItem>>::fail(rs.code, rs.msg);
        }
        std::vector<model::WaitlistItem> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(fromRow(row));
        }
        return Result<std::vector<model::WaitlistItem>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<model::WaitlistItem>>::fail(
            kDbError, std::string("扫描过期 offer 失败：") + e.what());
    }
}

Result<int64_t> WaitlistRepo::cancelAllForResource(DbConn& db, int64_t resourceId,
                                                   const std::string& reason) {
    if (resourceId <= 0) {
        return Result<int64_t>::make(0);
    }
    try {
        Stmt st(db,
                "UPDATE `waitlist` SET status = 'CANCELLED', remark = ? "
                "WHERE resource_id = ? AND status IN ('WAITING','OFFERED')");
        st.bindStr(reason).bindI64(resourceId);
        auto rr = st.exec();
        if (!rr.ok()) {
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(rr.data.affectedRows);
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("作废资源候补失败：") + e.what());
    }
}

} // namespace rf::repo
