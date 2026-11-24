// =============================================================================
// src/ReservationRepo.cpp  —  预约与时间槽的 SQL 实现
// 对应 ARCH-P1 §3.6 / §1.5 / D-P1-3 / D-P1-6
//
// 实现要点：
//   1) 所有值走 Stmt 绑定；动态 WHERE 只拼「列名 + ?」，无注入面；
//   2) 批量插槽用单条 INSERT 多 VALUES（≤16 槽 → ≤64 个占位符），一次往返、
//      一次原子性判定：任一槽冲突即整条失败，不会出现「写了一半槽」；
//   3) 冲突判定用 Stmt::isDupEntry()（D-P1-6），把 1062 精确转成 3005，
//      绝不让它漏成 1009；
//   4) total 与列表用同一 WHERE，值按同样的顺序绑定两次。
// =============================================================================
#include "ReservationRepo.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"

namespace rf::repo {

namespace {

/** 主表列清单（11 列），列序必须与 model::Reservation::fromRow 的契约一致 */
constexpr const char* kReservationSelect =
    "SELECT id, reservation_no, user_id, resource_id, "
    "DATE_FORMAT(start_time, '%Y-%m-%dT%H:%i:%s') AS start_time, "
    "DATE_FORMAT(end_time, '%Y-%m-%dT%H:%i:%s') AS end_time, "
    "status, deposit_amount, remark, "
    "DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%s') AS created_at, "
    "DATE_FORMAT(updated_at, '%Y-%m-%dT%H:%i:%s') AS updated_at "
    "FROM `reservation` ";

/**
 * 投影列清单（15 列）。JOIN user / resource 取展示字段。
 * 资源即使已逻辑删除也要能显示（历史单据），故 JOIN 不过滤 deleted_at。
 */
constexpr const char* kViewSelect =
    "SELECT r.id, r.reservation_no, r.user_id, u.username, r.resource_id, "
    "res.code, res.name, res.type, "
    "DATE_FORMAT(r.start_time, '%Y-%m-%dT%H:%i:%s') AS start_time, "
    "DATE_FORMAT(r.end_time, '%Y-%m-%dT%H:%i:%s') AS end_time, "
    "r.status, r.deposit_amount, r.remark, "
    "DATE_FORMAT(r.created_at, '%Y-%m-%dT%H:%i:%s') AS created_at, "
    "DATE_FORMAT(r.updated_at, '%Y-%m-%dT%H:%i:%s') AS updated_at, "
    "res.status AS resource_status "
    "FROM `reservation` r "
    "JOIN `user` u     ON u.id   = r.user_id "
    "JOIN `resource` res ON res.id = r.resource_id ";

/** 有效预约状态集合：与 model::isActiveReservationStatus 严格同集合（固定常量，无外部输入） */
constexpr const char* kActiveReservationStatuses =
    "('PENDING_REVIEW','LOCKED','IN_USE','DUE_RETURN','RETURN_CHECK')";

/** 稳定取单元格字符串 */
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

double cellDbl(const Row& row, size_t i) {
    const std::string v = cellStr(row, i);
    if (v.empty()) {
        return 0.0;
    }
    return std::strtod(v.c_str(), nullptr);
}

/**
 * ISO("2026-02-10T09:00:00") → SQL DATETIME 字面量("2026-02-10 09:00:00")。
 * 不依赖 MySQL 对 'T' 分隔符的容忍度，先在本层归一，行为确定。
 */
std::string isoToSql(const std::string& iso) {
    std::string v = iso;
    if (v.size() >= 11 && (v[10] == 'T' || v[10] == 't')) {
        v[10] = ' ';
    }
    return v;
}

/** 投影行 → ReservationRow（列序与 kViewSelect 严格一致） */
ReservationRow viewFromRow(const Row& row) {
    ReservationRow v;
    v.id            = cellI64(row, 0);
    v.reservationNo = cellStr(row, 1);
    v.userId        = cellI64(row, 2);
    v.username      = cellStr(row, 3);
    v.resourceId    = cellI64(row, 4);
    v.resourceCode  = cellStr(row, 5);
    v.resourceName  = cellStr(row, 6);
    v.resourceType  = model::resourceTypeFrom(cellStr(row, 7)).value_or(model::ResourceType::MEETING_ROOM);
    v.startTime     = cellStr(row, 8);
    v.endTime       = cellStr(row, 9);
    v.status        = cellStr(row, 10);
    v.depositAmount = cellDbl(row, 11);
    v.remark        = cellStr(row, 12);
    v.createdAt     = cellStr(row, 13);
    v.updatedAt     = cellStr(row, 14);
    v.resourceStatus = cellStr(row, 15);   // P3：资源当前状态
    return v;
}

/** 生成 "?,?,?" 占位符串 */
std::string placeholders(size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        if (i != 0) {
            out += ",";
        }
        out += "?";
    }
    return out;
}

}  // namespace

// ============================================================================
// reservation
// ============================================================================

Result<int64_t> ReservationRepo::insert(DbConn& db, const model::Reservation& r) {
    if (r.reservationNo.empty() || r.userId <= 0 || r.resourceId <= 0) {
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：预约单号 / 用户 / 资源不能为空");
    }
    try {
        Stmt st(db,
                "INSERT INTO `reservation` "
                "(reservation_no, user_id, resource_id, start_time, end_time, status, "
                " deposit_amount, remark) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        st.bindStr(r.reservationNo)
            .bindI64(r.userId)
            .bindI64(r.resourceId)
            .bindStr(isoToSql(r.startTime))
            .bindStr(isoToSql(r.endTime))
            .bindStr(model::toStr(r.status))
            .bindDouble(r.depositAmount)
            .bindStr(r.remark);
        auto rr = st.exec();
        if (!rr.ok()) {
            if (st.isDupEntry()) {
                // uk_reservation_no 冲突：正常路径下不该发生（单号由 Redis INCR 保证唯一），
                // 只可能是序列 key 被人工重置，故如实报错让调用方重试，不谎报「号已用尽」
                return Result<int64_t>::fail(kDbError, "新增预约失败：预约单号冲突，请重试");
            }
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(static_cast<int64_t>(rr.data.insertId));
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("新增预约失败：") + e.what());
    }
}

Result<std::optional<model::Reservation>> ReservationRepo::findById(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<std::optional<model::Reservation>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kReservationSelect) + "WHERE id = ?");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<model::Reservation>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<model::Reservation>>::make(std::nullopt);
        }
        return Result<std::optional<model::Reservation>>::make(
            std::optional<model::Reservation>(model::Reservation::fromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<model::Reservation>>::fail(
            kDbError, std::string("查询预约失败：") + e.what());
    }
}

Result<std::optional<ReservationRow>> ReservationRepo::findView(DbConn& db, int64_t id) {
    if (id <= 0) {
        return Result<std::optional<ReservationRow>>::make(std::nullopt);
    }
    try {
        Stmt st(db, std::string(kViewSelect) + "WHERE r.id = ?");
        st.bindI64(id);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::optional<ReservationRow>>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::optional<ReservationRow>>::make(std::nullopt);
        }
        return Result<std::optional<ReservationRow>>::make(
            std::optional<ReservationRow>(viewFromRow(rs.data.front())));
    } catch (const std::exception& e) {
        return Result<std::optional<ReservationRow>>::fail(
            kDbError, std::string("查询预约详情失败：") + e.what());
    }
}

Result<ReservationPage> ReservationRepo::search(DbConn& db, const ReservationQuery& q) {
    // 防御性收敛：Service 已保证范围，这里再兜一次，避免非法 LIMIT
    const int page = (q.page < 1) ? 1 : q.page;
    const int size = (q.size < 1) ? 10 : (q.size > 100 ? 100 : q.size);

    try {
        // ---- 动态 WHERE：只拼列名 + ?，值按顺序绑定两次（COUNT 与 LIST）
        std::string where;
        auto addCond = [&where](const char* cond) {
            where += where.empty() ? "" : " AND ";
            where += cond;
        };
        if (q.userId.has_value()) {
            addCond("r.user_id = ?");
        }
        if (q.status.has_value() && !q.status->empty()) {
            addCond("r.status = ?");
        }
        if (q.resourceId.has_value()) {
            addCond("r.resource_id = ?");
        }

        auto bindFilters = [&q](Stmt& st) {
            if (q.userId.has_value()) {
                st.bindI64(*q.userId);
            }
            if (q.status.has_value() && !q.status->empty()) {
                st.bindStr(*q.status);
            }
            if (q.resourceId.has_value()) {
                st.bindI64(*q.resourceId);
            }
        };

        const std::string whereSql = where.empty() ? std::string("1=1") : where;

        int64_t total = 0;
        {
            Stmt cnt(db, std::string("SELECT COUNT(*) AS c FROM `reservation` r WHERE ") + whereSql);
            bindFilters(cnt);
            auto cr = cnt.query();
            if (!cr.ok()) {
                return Result<ReservationPage>::fail(cr.code, cr.msg);
            }
            if (!cr.data.empty()) {
                total = cellI64(cr.data.front(), 0);
            }
        }

        ReservationPage out;
        out.total = total;
        out.page  = page;
        out.size  = size;
        if (total == 0) {
            return Result<ReservationPage>::make(std::move(out));   // 空结果不再发第二条 SQL
        }

        const int64_t offset = static_cast<int64_t>(page - 1) * static_cast<int64_t>(size);
        Stmt lst(db, std::string(kViewSelect) + "WHERE " + whereSql +
                         " ORDER BY r.id DESC LIMIT ? OFFSET ?");
        bindFilters(lst);
        lst.bindI64(static_cast<int64_t>(size)).bindI64(offset);
        auto lr = lst.query();
        if (!lr.ok()) {
            return Result<ReservationPage>::fail(lr.code, lr.msg);
        }
        out.list.reserve(lr.data.size());
        for (const auto& row : lr.data) {
            out.list.push_back(viewFromRow(row));
        }
        return Result<ReservationPage>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<ReservationPage>::fail(kDbError, std::string("检索预约失败：") + e.what());
    }
}

VoidResult ReservationRepo::updateStatus(DbConn& db, int64_t id, const std::string& toStatus) {
    if (id <= 0) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：预约 id 非法");
    }
    try {
        Stmt st(db, "UPDATE `reservation` SET status = ? WHERE id = ?");
        st.bindStr(toStatus).bindI64(id);
        auto rr = st.exec();
        if (!rr.ok()) {
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("更新预约状态失败：") + e.what());
    }
}

Result<std::vector<std::string>> ReservationRepo::listActiveStatuses(DbConn& db, int64_t resourceId,
                                                                    int64_t excludeReservationId) {
    if (resourceId <= 0) {
        return Result<std::vector<std::string>>::make(std::vector<std::string>{});
    }
    try {
        std::string sql = std::string("SELECT status FROM `reservation` WHERE resource_id = ? "
                                      "AND status IN ") +
                          kActiveReservationStatuses;
        if (excludeReservationId > 0) {
            sql += " AND id <> ?";
        }
        Stmt st(db, sql);
        st.bindI64(resourceId);
        if (excludeReservationId > 0) {
            st.bindI64(excludeReservationId);
        }
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<std::string>>::fail(rs.code, rs.msg);
        }
        std::vector<std::string> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(cellStr(row, 0));
        }
        return Result<std::vector<std::string>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<std::string>>::fail(
            kDbError, std::string("查询资源有效预约失败：") + e.what());
    }
}

Result<int64_t> ReservationRepo::countActiveByUser(DbConn& db, int64_t userId) {
    if (userId <= 0) {
        return Result<int64_t>::make(0);
    }
    try {
        Stmt st(db, std::string("SELECT COUNT(*) AS c FROM `reservation` WHERE user_id = ? "
                                "AND status IN ") +
                        kActiveReservationStatuses);
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
        return Result<int64_t>::fail(kDbError, std::string("统计用户有效预约失败：") + e.what());
    }
}

Result<bool> ReservationRepo::hasOtherActive(DbConn& db, int64_t resourceId,
                                             int64_t excludeReservationId) {
    auto r = listActiveStatuses(db, resourceId, excludeReservationId);
    if (!r.ok()) {
        return Result<bool>::fail(r.code, r.msg);
    }
    return Result<bool>::make(!r.data.empty());
}

// ============================================================================
// reservation_slot
// ============================================================================

Result<std::vector<int>> ReservationRepo::occupiedIndexes(DbConn& db, int64_t resourceId,
                                                          const std::string& date,
                                                          const std::vector<int>& candidates) {
    if (resourceId <= 0 || date.empty() || candidates.empty()) {
        return Result<std::vector<int>>::make(std::vector<int>{});
    }
    try {
        Stmt st(db, "SELECT slot_index FROM `reservation_slot` "
                    "WHERE resource_id = ? AND slot_date = ? AND slot_index IN (" +
                        placeholders(candidates.size()) + ") ORDER BY slot_index ASC");
        st.bindI64(resourceId).bindStr(date);
        for (const int idx : candidates) {
            st.bindI64(static_cast<int64_t>(idx));
        }
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<int>>::fail(rs.code, rs.msg);
        }
        std::vector<int> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(static_cast<int>(cellI64(row, 0)));
        }
        return Result<std::vector<int>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<int>>::fail(kDbError,
                                              std::string("查询时段占用失败：") + e.what());
    }
}

VoidResult ReservationRepo::insertSlots(DbConn& db, int64_t reservationId, int64_t resourceId,
                                        const std::string& date, const std::vector<int>& indexes) {
    if (reservationId <= 0 || resourceId <= 0 || date.empty()) {
        return VoidResult::fail(kInvalidParam, "参数校验失败：槽位写入参数非法");
    }
    if (indexes.empty()) {
        return VoidResult::make(Empty{});   // 无槽可写（理论上不会发生）
    }
    try {
        // 单条 INSERT 多 VALUES：整条语句原子，任一槽冲突则全部不写
        std::string sql =
            "INSERT INTO `reservation_slot` (reservation_id, resource_id, slot_date, slot_index) "
            "VALUES ";
        for (size_t i = 0; i < indexes.size(); ++i) {
            sql += (i == 0) ? "(?,?,?,?)" : ",(?,?,?,?)";
        }
        Stmt st(db, sql);
        for (const int idx : indexes) {
            st.bindI64(reservationId).bindI64(resourceId).bindStr(date).bindI64(
                static_cast<int64_t>(idx));
        }
        auto rr = st.exec();
        if (!rr.ok()) {
            if (st.isDupEntry()) {
                // D-P1-3：唯一索引兜底命中 —— 转成业务码 3005，绝不漏成 1009
                return VoidResult::fail(kSlotUnavailable, "所选时段已被占用");
            }
            return VoidResult::fail(rr.code, rr.msg);
        }
        return VoidResult::make(Empty{});
    } catch (const std::exception& e) {
        return VoidResult::fail(kDbError, std::string("写入时段失败：") + e.what());
    }
}

Result<std::vector<int>> ReservationRepo::listSlotIndexes(DbConn& db, int64_t reservationId) {
    if (reservationId <= 0) {
        return Result<std::vector<int>>::make(std::vector<int>{});
    }
    try {
        Stmt st(db, "SELECT slot_index FROM `reservation_slot` WHERE reservation_id = ? "
                    "ORDER BY slot_index ASC");
        st.bindI64(reservationId);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<int>>::fail(rs.code, rs.msg);
        }
        std::vector<int> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(static_cast<int>(cellI64(row, 0)));
        }
        return Result<std::vector<int>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<int>>::fail(kDbError, std::string("查询预约槽位失败：") + e.what());
    }
}

Result<std::string> ReservationRepo::slotDateOf(DbConn& db, int64_t reservationId) {
    if (reservationId <= 0) {
        return Result<std::string>::make(std::string());
    }
    try {
        Stmt st(db, "SELECT DATE_FORMAT(slot_date, '%Y-%m-%d') FROM `reservation_slot` "
                    "WHERE reservation_id = ? ORDER BY slot_index ASC LIMIT 1");
        st.bindI64(reservationId);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::string>::fail(rs.code, rs.msg);
        }
        if (rs.data.empty()) {
            return Result<std::string>::make(std::string());
        }
        return Result<std::string>::make(cellStr(rs.data.front(), 0));
    } catch (const std::exception& e) {
        return Result<std::string>::fail(kDbError, std::string("查询预约日期失败：") + e.what());
    }
}

Result<int64_t> ReservationRepo::deleteSlots(DbConn& db, int64_t reservationId) {
    if (reservationId <= 0) {
        return Result<int64_t>::make(0);
    }
    try {
        Stmt st(db, "DELETE FROM `reservation_slot` WHERE reservation_id = ?");
        st.bindI64(reservationId);
        auto rr = st.exec();
        if (!rr.ok()) {
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        return Result<int64_t>::make(rr.data.affectedRows);
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("释放时段失败：") + e.what());
    }
}

// ============================================================================
// P2 增量：CAS + 定时任务候选查询 + 时间线（ARCH-P2 §3.7）
// ============================================================================

Result<int64_t> ReservationRepo::casUpdateStatus(DbConn& db, int64_t id,
                                                 const std::string& fromStatus,
                                                 const std::string& toStatus) {
    if (id <= 0 || fromStatus.empty() || toStatus.empty()) {
        return Result<int64_t>::fail(kInvalidParam, "参数校验失败：CAS 迁移参数非法");
    }
    try {
        Stmt st(db, "UPDATE `reservation` SET status = ? WHERE id = ? AND status = ?");
        st.bindStr(toStatus).bindI64(id).bindStr(fromStatus);
        auto rr = st.exec();
        if (!rr.ok()) {
            return Result<int64_t>::fail(rr.code, rr.msg);
        }
        // affectedRows 是唯一裁决依据：1 = 本线程赢下迁移；0 = 别人已处理
        return Result<int64_t>::make(rr.data.affectedRows);
    } catch (const std::exception& e) {
        return Result<int64_t>::fail(kDbError, std::string("CAS 更新预约状态失败：") + e.what());
    }
}

Result<std::vector<int64_t>> ReservationRepo::listStalePending(DbConn& db,
                                                              const std::string& cutoffIso,
                                                              int limit) {
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    try {
        Stmt st(db,
                "SELECT id FROM `reservation` WHERE status = 'PENDING_REVIEW' "
                "AND created_at < ? ORDER BY created_at ASC LIMIT ?");
        st.bindStr(isoToSql(cutoffIso)).bindI64(static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<int64_t>>::fail(rs.code, rs.msg);
        }
        std::vector<int64_t> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(cellI64(row, 0));
        }
        return Result<std::vector<int64_t>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<int64_t>>::fail(
            kDbError, std::string("扫描待审核超时失败：") + e.what());
    }
}

Result<std::vector<int64_t>> ReservationRepo::listNoShow(DbConn& db, int pickupLateMin,
                                                         const std::string& nowIso, int limit) {
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    try {
        // 时间加减统一用 TIMESTAMPADD（ARCH-P2 §1.6 SQL 铁律）
        Stmt st(db,
                "SELECT id FROM `reservation` WHERE status = 'LOCKED' "
                "AND TIMESTAMPADD(MINUTE, ?, start_time) < ? "
                "ORDER BY start_time ASC LIMIT ?");
        st.bindI64(static_cast<int64_t>(pickupLateMin))
            .bindStr(isoToSql(nowIso))
            .bindI64(static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<int64_t>>::fail(rs.code, rs.msg);
        }
        std::vector<int64_t> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(cellI64(row, 0));
        }
        return Result<std::vector<int64_t>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<int64_t>>::fail(kDbError,
                                                  std::string("扫描爽约失败：") + e.what());
    }
}

Result<std::vector<int64_t>> ReservationRepo::listOverdueInUse(DbConn& db,
                                                              const std::string& nowIso, int limit) {
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    try {
        Stmt st(db,
                "SELECT id FROM `reservation` WHERE status = 'IN_USE' AND end_time < ? "
                "ORDER BY end_time ASC LIMIT ?");
        st.bindStr(isoToSql(nowIso)).bindI64(static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<int64_t>>::fail(rs.code, rs.msg);
        }
        std::vector<int64_t> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(cellI64(row, 0));
        }
        return Result<std::vector<int64_t>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<int64_t>>::fail(kDbError,
                                                  std::string("扫描逾期失败：") + e.what());
    }
}

Result<std::vector<ReservationRepo::OverdueCandidate>> ReservationRepo::listOverdueDueReturn(
    DbConn& db, int periodMin, const std::string& nowIso, int limit) {
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    if (periodMin < 1) {
        return Result<std::vector<OverdueCandidate>>::fail(
            kInvalidParam, "参数校验失败：扣分周期必须 >= 1 分钟");
    }
    try {
        Stmt st(db,
                "SELECT id, user_id, resource_id, "
                "TIMESTAMPDIFF(MINUTE, end_time, ?) DIV ? AS deduct_seq "
                "FROM `reservation` WHERE status = 'DUE_RETURN' AND end_time < ? "
                "ORDER BY end_time ASC LIMIT ?");
        const std::string nowSql = isoToSql(nowIso);
        st.bindStr(nowSql).bindI64(static_cast<int64_t>(periodMin)).bindStr(nowSql).bindI64(
            static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<OverdueCandidate>>::fail(rs.code, rs.msg);
        }
        std::vector<OverdueCandidate> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            OverdueCandidate c;
            c.id         = cellI64(row, 0);
            c.userId     = cellI64(row, 1);
            c.resourceId = cellI64(row, 2);
            c.deductSeq  = static_cast<int>(cellI64(row, 3));
            out.push_back(c);
        }
        return Result<std::vector<OverdueCandidate>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<OverdueCandidate>>::fail(
            kDbError, std::string("扫描逾期待续失败：") + e.what());
    }
}

Result<std::vector<int64_t>> ReservationRepo::listStuckReturnCheck(DbConn& db,
                                                                  const std::string& cutoffIso,
                                                                  int limit) {
    const int lim = (limit < 1) ? 50 : (limit > 500 ? 500 : limit);
    try {
        Stmt st(db,
                "SELECT id FROM `reservation` WHERE status = 'RETURN_CHECK' AND updated_at < ? "
                "ORDER BY updated_at ASC LIMIT ?");
        st.bindStr(isoToSql(cutoffIso)).bindI64(static_cast<int64_t>(lim));
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<int64_t>>::fail(rs.code, rs.msg);
        }
        std::vector<int64_t> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(cellI64(row, 0));
        }
        return Result<std::vector<int64_t>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<int64_t>>::fail(
            kDbError, std::string("扫描待验收超时失败：") + e.what());
    }
}

Result<std::map<int64_t, ReservationRepo::DepositSummary>> ReservationRepo::depositSummaries(
    DbConn& db, const std::vector<int64_t>& ids) {
    std::map<int64_t, DepositSummary> out;
    if (ids.empty()) {
        return Result<std::map<int64_t, DepositSummary>>::make(std::move(out));
    }
    try {
        // 一次 GROUP BY 取整页的押金聚合（列表接口不再逐行查，杜绝 N+1）
        Stmt st(db, "SELECT reservation_id, "
                    "COALESCE(SUM(CASE WHEN record_type='FREEZE' THEN amount ELSE 0 END), 0) AS f, "
                    "COALESCE(SUM(CASE WHEN record_type='DEDUCT' THEN amount ELSE 0 END), 0) AS d, "
                    "COALESCE(SUM(CASE WHEN record_type='REFUND' THEN amount ELSE 0 END), 0) AS r "
                    "FROM `deposit_record` WHERE reservation_id IN (" +
                        placeholders(ids.size()) + ") GROUP BY reservation_id");
        for (const int64_t id : ids) {
            st.bindI64(id);
        }
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::map<int64_t, DepositSummary>>::fail(rs.code, rs.msg);
        }
        for (const auto& row : rs.data) {
            DepositSummary s;
            const int64_t  id = cellI64(row, 0);
            s.frozen          = cellDbl(row, 1);
            s.deducted        = cellDbl(row, 2);
            s.refunded        = cellDbl(row, 3);
            out[id]           = s;
        }
        return Result<std::map<int64_t, DepositSummary>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::map<int64_t, DepositSummary>>::fail(
            kDbError, std::string("批量汇总押金失败：") + e.what());
    }
}

Result<std::vector<model::StatusLogEntry>> ReservationRepo::listTimeline(DbConn& db,
                                                                         int64_t reservationId) {
    if (reservationId <= 0) {
        return Result<std::vector<model::StatusLogEntry>>::make(
            std::vector<model::StatusLogEntry>{});
    }
    try {
        // 列序必须与 model::StatusLogEntry::fromRow 的契约一致（9 列）
        Stmt st(db,
                "SELECT id, business_type, business_id, from_status, to_status, operator_id, "
                "reason, rule_code, "
                "DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%s') AS created_at "
                "FROM `status_log` WHERE business_type = 'RESERVATION' AND business_id = ? "
                "ORDER BY id ASC");
        st.bindI64(reservationId);
        auto rs = st.query();
        if (!rs.ok()) {
            return Result<std::vector<model::StatusLogEntry>>::fail(rs.code, rs.msg);
        }
        std::vector<model::StatusLogEntry> out;
        out.reserve(rs.data.size());
        for (const auto& row : rs.data) {
            out.push_back(model::StatusLogEntry::fromRow(row));
        }
        return Result<std::vector<model::StatusLogEntry>>::make(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<model::StatusLogEntry>>::fail(
            kDbError, std::string("查询预约时间线失败：") + e.what());
    }
}

} // namespace rf::repo
