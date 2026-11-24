// =============================================================================
// src/model/Model.h  —  领域枚举与实体（header-only，纯数据，无 DB 访问）
// 对应 ARCH-P0 §3.8 / §8.2：JSON ↔ DB 字段手工映射，禁止反射式自动转换
// 约束：本文件不得包含 mysql.h（隔离 C API），fromRow 只吃「字符串可空行」
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace rf::model {

/** 账号角色（P0 只有两级） */
enum class Role { USER, ADMIN };
/** 账号状态：黑名单只拦预约，DISABLED 连登录都拦 */
/**
 * 账号状态（P6 起 5 态）。
 *
 * PENDING / REJECTED 是「注册审核」引入的两个状态（ARCH-P6 §1.2 / §1.3）：
 *   · PENDING  注册已提交、等管理员审核 —— **不能登录**（2004），也没有任何其它入口能改它
 *   · REJECTED 审核未通过 —— 不能登录（2005），但学生可再次调注册接口自助重提 → 回到 PENDING
 *
 * ⚠️ CreditService::nextStatus() 只认 NORMAL / BLACKLIST，其余状态原样返回，
 *    所以信用分变动**不会**把这两个新状态改掉（这是 P6 安全的关键，别改那个守卫）。
 */
enum class UserStatus { NORMAL, BLACKLIST, DISABLED, PENDING, REJECTED };
/** 资源类型 */
enum class ResourceType { LAB_EQUIPMENT, MEETING_ROOM, LOCKER };

/**
 * 资源状态。
 * P0 只允许写 FREE / MAINTENANCE；其余为 P1-P3 预留，必须一次定义全。
 */
enum class ResourceState {
    FREE, MAINTENANCE,                                        // P0 可写
    PENDING_REVIEW, LOCKED, IN_USE, DUE_RETURN, RETURN_CHECK, // P1-P2 流转
    DAMAGE_CLAIM, DISABLED, DELETED                           // P3 追责 / 停用 / 已删
};

// ------------------------------------------------------------------ 双向转换
inline std::string toStr(Role v) {
    switch (v) {
        case Role::ADMIN: return "ADMIN";
        case Role::USER:
        default:          return "USER";
    }
}

inline std::optional<Role> roleFrom(const std::string& s) {
    if (s == "USER")  return Role::USER;
    if (s == "ADMIN") return Role::ADMIN;
    return std::nullopt;
}

inline std::string toStr(UserStatus v) {
    switch (v) {
        case UserStatus::BLACKLIST: return "BLACKLIST";
        case UserStatus::DISABLED:  return "DISABLED";
        case UserStatus::PENDING:   return "PENDING";    // P6
        case UserStatus::REJECTED:  return "REJECTED";   // P6
        case UserStatus::NORMAL:
        default:                    return "NORMAL";
    }
}

inline std::optional<UserStatus> userStatusFrom(const std::string& s) {
    if (s == "NORMAL")    return UserStatus::NORMAL;
    if (s == "BLACKLIST") return UserStatus::BLACKLIST;
    if (s == "DISABLED")  return UserStatus::DISABLED;
    if (s == "PENDING")   return UserStatus::PENDING;    // P6
    if (s == "REJECTED")  return UserStatus::REJECTED;   // P6
    return std::nullopt;
}

/** 账号状态中文标签（P3 增量：/credit/me 需要展示「正常 / 黑名单 / 已停用」） */
inline const char* labelOf(UserStatus v) {
    switch (v) {
        case UserStatus::BLACKLIST: return "黑名单";
        case UserStatus::DISABLED:  return "已停用";
        case UserStatus::PENDING:   return "待审核";     // P6
        case UserStatus::REJECTED:  return "已拒绝";     // P6
        case UserStatus::NORMAL:
        default:                    return "正常";
    }
}

inline std::string toStr(ResourceType v) {
    switch (v) {
        case ResourceType::LAB_EQUIPMENT: return "LAB_EQUIPMENT";
        case ResourceType::LOCKER:        return "LOCKER";
        case ResourceType::MEETING_ROOM:
        default:                          return "MEETING_ROOM";
    }
}

inline std::optional<ResourceType> resourceTypeFrom(const std::string& s) {
    if (s == "LAB_EQUIPMENT") return ResourceType::LAB_EQUIPMENT;
    if (s == "MEETING_ROOM")  return ResourceType::MEETING_ROOM;
    if (s == "LOCKER")        return ResourceType::LOCKER;
    return std::nullopt;
}

inline std::string toStr(ResourceState v) {
    switch (v) {
        case ResourceState::FREE:           return "FREE";
        case ResourceState::MAINTENANCE:    return "MAINTENANCE";
        case ResourceState::PENDING_REVIEW: return "PENDING_REVIEW";
        case ResourceState::LOCKED:         return "LOCKED";
        case ResourceState::IN_USE:         return "IN_USE";
        case ResourceState::DUE_RETURN:     return "DUE_RETURN";
        case ResourceState::RETURN_CHECK:   return "RETURN_CHECK";
        case ResourceState::DAMAGE_CLAIM:   return "DAMAGE_CLAIM";
        case ResourceState::DISABLED:       return "DISABLED";
        case ResourceState::DELETED:        return "DELETED";
    }
    return "FREE";
}

inline std::optional<ResourceState> resourceStateFrom(const std::string& s) {
    if (s == "FREE")           return ResourceState::FREE;
    if (s == "MAINTENANCE")    return ResourceState::MAINTENANCE;
    if (s == "PENDING_REVIEW") return ResourceState::PENDING_REVIEW;
    if (s == "LOCKED")         return ResourceState::LOCKED;
    if (s == "IN_USE")         return ResourceState::IN_USE;
    if (s == "DUE_RETURN")     return ResourceState::DUE_RETURN;
    if (s == "RETURN_CHECK")   return ResourceState::RETURN_CHECK;
    if (s == "DAMAGE_CLAIM")   return ResourceState::DAMAGE_CLAIM;
    if (s == "DISABLED")       return ResourceState::DISABLED;
    if (s == "DELETED")        return ResourceState::DELETED;
    return std::nullopt;
}

/** 资源类型中文标签（只读展示，不参与写入） */
inline const char* labelOf(ResourceType t) {
    switch (t) {
        case ResourceType::LAB_EQUIPMENT: return "实验室仪器";
        case ResourceType::MEETING_ROOM:  return "共享会议室";
        case ResourceType::LOCKER:        return "闲置储物柜";
    }
    return "共享会议室";
}

/** 资源状态中文标签（只读展示，不参与写入） */
inline const char* labelOf(ResourceState s) {
    switch (s) {
        case ResourceState::FREE:           return "可用";
        case ResourceState::MAINTENANCE:    return "维护中";
        case ResourceState::PENDING_REVIEW: return "待审核";
        case ResourceState::LOCKED:         return "已锁定";
        case ResourceState::IN_USE:         return "使用中";
        case ResourceState::DUE_RETURN:     return "待归还";
        case ResourceState::RETURN_CHECK:   return "归还验收中";
        case ResourceState::DAMAGE_CLAIM:   return "损坏理赔中";
        case ResourceState::DISABLED:       return "已停用";
        case ResourceState::DELETED:        return "已删除";
    }
    return "可用";
}

/** P0 允许手工设置的状态（D-5）：仅 FREE / MAINTENANCE */
inline bool isP0WritableState(ResourceState s) {
    return s == ResourceState::FREE || s == ResourceState::MAINTENANCE;
}

/** D-4：构成「有效预约」的状态集合（存在即禁止删除资源） */
inline bool isActiveReservationStatus(const std::string& s) {
    return s == "PENDING_REVIEW" || s == "LOCKED" || s == "IN_USE" ||
           s == "DUE_RETURN" || s == "RETURN_CHECK";
}

/**
 * P2 「不可预约」状态集合（ARCH-P2 D-P2-10）：从 P1 的 3 个扩到 5 个。
 * `IN_USE` **不**在内：仪器/会议室都是时段模型，使用中的资源仍可被预约其他时段。
 * 本函数是 create / 候补 join / 派发复检三处的唯一口径来源。
 */
inline bool isUnbookableState(ResourceState s) {
    return s == ResourceState::MAINTENANCE || s == ResourceState::DISABLED ||
           s == ResourceState::DELETED || s == ResourceState::DAMAGE_CLAIM ||
           s == ResourceState::RETURN_CHECK;
}

// ------------------------------------------------------------------ 实体
/** 账号实体 */
struct User {
    int64_t     id = 0;
    std::string username;
    std::string passwordHash;
    std::string realName;
    std::string phone;
    Role        role        = Role::USER;
    int         creditScore = 100;
    UserStatus  status      = UserStatus::NORMAL;
    std::string createdAt;   // ISO 字符串，直接来自 DATE_FORMAT
    std::string updatedAt;   // ISO 字符串

    /** 全字段 JSON（不输出 passwordHash） */
    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]          = id;
        j["username"]    = username;
        j["realName"]    = realName;
        j["phone"]       = phone;
        j["role"]        = toStr(role);
        j["creditScore"] = creditScore;
        j["status"]      = toStr(status);
        j["createdAt"]   = createdAt;
        j["updatedAt"]   = updatedAt;
        return j;
    }

    /** 概要 JSON：登录响应与列表用 */
    nlohmann::json toBriefJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]          = id;
        j["username"]    = username;
        j["role"]        = toStr(role);
        j["creditScore"] = creditScore;
        return j;
    }
};

/** 资源实体 */
struct Resource {
    int64_t       id          = 0;
    std::string   code;
    std::string   name;
    std::string   location;
    std::string   description;
    ResourceType  type        = ResourceType::MEETING_ROOM;
    ResourceState status      = ResourceState::FREE;
    int           capacity    = 1;
    double        depositAmount = 0.0;
    int64_t       ownerAdminId  = 0;
    std::string   createdAt;
    std::string   updatedAt;
    std::string   deletedAt;   // 空串表示未删除（DB 为 NULL）

    /** 接口 JSON：含 typeLabel / statusLabel（§8.2） */
    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]            = id;
        j["code"]          = code;
        j["name"]          = name;
        j["type"]          = toStr(type);
        j["typeLabel"]     = labelOf(type);
        j["status"]        = toStr(status);
        j["statusLabel"]   = labelOf(status);
        j["location"]      = location;
        j["capacity"]      = capacity;
        j["depositAmount"] = round2(depositAmount);
        j["description"]   = description;
        j["ownerAdminId"]  = ownerAdminId;
        j["createdAt"]     = createdAt;
        j["updatedAt"]     = updatedAt;
        return j;
    }

    /**
     * 行 → 实体。列顺序必须与 ResourceRepo 的 SELECT 列表严格一致：
     * id, code, name, type, status, location, capacity, deposit_amount,
     * description, owner_admin_id, created_at, updated_at, deleted_at
     * @param row 每列 std::optional<std::string>，SQL NULL → nullopt
     */
    static Resource fromRow(const std::vector<std::optional<std::string>>& row) {
        Resource r;
        if (row.size() < 13) {
            return r;
        }
        r.id          = i64At(row, 0);
        r.code        = strAt(row, 1);
        r.name        = strAt(row, 2);
        r.type        = resourceTypeFrom(strAt(row, 3)).value_or(ResourceType::MEETING_ROOM);
        r.status      = resourceStateFrom(strAt(row, 4)).value_or(ResourceState::FREE);
        r.location    = strAt(row, 5);
        r.capacity    = static_cast<int>(i64At(row, 6));
        r.depositAmount = dblAt(row, 7);
        r.description   = strAt(row, 8);
        r.ownerAdminId  = i64At(row, 9);
        r.createdAt     = strAt(row, 10);
        r.updatedAt     = strAt(row, 11);
        r.deletedAt     = strAt(row, 12);
        return r;
    }

private:
    static std::string strAt(const std::vector<std::optional<std::string>>& row, size_t i) {
        return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
    }

    static int64_t i64At(const std::vector<std::optional<std::string>>& row, size_t i) {
        const std::string v = strAt(row, i);
        if (v.empty()) {
            return 0;
        }
        return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
    }

    static double dblAt(const std::vector<std::optional<std::string>>& row, size_t i) {
        const std::string v = strAt(row, i);
        if (v.empty()) {
            return 0.0;
        }
        return std::strtod(v.c_str(), nullptr);
    }

    /** 金额保留两位小数（DECIMAL(10,2) 语义） */
    static double round2(double v) {
        return static_cast<double>(static_cast<int64_t>(v * 100.0 + (v >= 0 ? 0.5 : -0.5))) / 100.0;
    }
};

/** 列表查询条件（D-8：page/size 越界由 Service 收敛） */
struct ResourceQuery {
    int page = 1;
    int size = 10;
    std::optional<ResourceType>  type;
    std::optional<ResourceState> status;
    std::optional<std::string>   location;
    std::optional<std::string>   keyword;   // 命中 name 或 description（LIKE %kw%）
};

/** 分页结果（total 由独立 COUNT(*) 得到） */
struct PageResult {
    std::vector<Resource> list;
    int64_t total = 0;
    int     page  = 1;
    int     size  = 10;
};

/** 新增资源入参 */
struct ResourceCreateReq {
    std::string  code;
    std::string  name;
    ResourceType type = ResourceType::MEETING_ROOM;
    std::string  location;
    int          capacity      = 1;
    double       depositAmount = 0.0;
    std::string  description;
};

/** 半更新入参：std::nullopt 表示「未提交，保持原值」 */
struct ResourceUpdateReq {
    std::optional<std::string>  name;
    std::optional<std::string>  location;
    std::optional<std::string>  description;
    std::optional<int>          capacity;
    std::optional<double>       depositAmount;
    std::optional<ResourceType> type;
};

// ------------------------------------------------------------------ 状态留痕
/**
 * status_log 一行（D-12 / D-13）。
 * 列顺序：id / business_type / business_id / from_status / to_status /
 *        operator_id / reason / rule_code / created_at
 *
 * ruleCode 用 std::optional 表达 DB 的 NULL：P0 全部为手工操作，一律 std::nullopt；
 * P1 规则引擎触发时填 rule_config.rule_code（D-12）。
 */
struct StatusLogEntry {
    int64_t                    id           = 0;      ///< 主键（写库时忽略）
    std::string                businessType;          ///< RESOURCE / RESERVATION
    int64_t                    businessId   = 0;      ///< 被变更对象主键
    std::string                fromStatus;            ///< 变更前状态；创建场景为空串
    std::string                toStatus;              ///< 变更后状态
    int64_t                    operatorId   = 0;      ///< 操作人 user.id
    std::string                reason;                ///< 自由文本，给人看
    std::optional<std::string> ruleCode;              ///< 规则编码；手工操作为 nullopt
    std::string                createdAt;             ///< ISO 字符串，来自 DATE_FORMAT

    /** 接口 JSON（ruleCode 无值时省略键，§8.2） */
    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]           = id;
        j["businessType"] = businessType;
        j["businessId"]   = businessId;
        j["fromStatus"]   = fromStatus;
        j["toStatus"]     = toStatus;
        j["operatorId"]   = operatorId;
        j["reason"]       = reason;
        if (ruleCode.has_value() && !ruleCode->empty()) {
            j["ruleCode"] = *ruleCode;
        }
        j["createdAt"] = createdAt;
        return j;
    }

    /**
     * 行 → 实体。列顺序必须与 AuditRepo 的 SELECT 列表严格一致。
     * @param row 每列 std::optional<std::string>，SQL NULL → nullopt
     */
    static StatusLogEntry fromRow(const std::vector<std::optional<std::string>>& row) {
        StatusLogEntry e;
        e.id           = i64At(row, 0);
        e.businessType = strAt(row, 1);
        e.businessId   = i64At(row, 2);
        e.fromStatus   = strAt(row, 3);
        e.toStatus     = strAt(row, 4);
        e.operatorId   = i64At(row, 5);
        e.reason       = strAt(row, 6);
        if (row.size() > 7 && row[7].has_value()) {
            e.ruleCode = *row[7];   // NULL → 保持 nullopt
        }
        e.createdAt = strAt(row, 8);
        return e;
    }

private:
    static std::string strAt(const std::vector<std::optional<std::string>>& row, size_t i) {
        return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
    }

    static int64_t i64At(const std::vector<std::optional<std::string>>& row, size_t i) {
        const std::string v = strAt(row, i);
        if (v.empty()) {
            return 0;
        }
        return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
    }
};

/**
 * 预约单状态（ARCH-P1 §3.3 / §4.4）。
 * 8 个值一次定义全：P1 实际可达前 4 个 + CANCELLED/REJECTED；
 * COMPLETED / IN_USE / DUE_RETURN / RETURN_CHECK 由 phase=2 启用（D-P1-1）。
 * EXPIRED 属 P2，届时只改 reservation.status 的列注释即可（VARCHAR(32) 可容纳）。
 */
enum class ReservationStatus {
    PENDING_REVIEW, LOCKED, IN_USE, DUE_RETURN, RETURN_CHECK,
    COMPLETED, CANCELLED, REJECTED
};

inline std::string toStr(ReservationStatus v) {
    switch (v) {
        case ReservationStatus::PENDING_REVIEW: return "PENDING_REVIEW";
        case ReservationStatus::LOCKED:         return "LOCKED";
        case ReservationStatus::IN_USE:         return "IN_USE";
        case ReservationStatus::DUE_RETURN:     return "DUE_RETURN";
        case ReservationStatus::RETURN_CHECK:   return "RETURN_CHECK";
        case ReservationStatus::COMPLETED:      return "COMPLETED";
        case ReservationStatus::CANCELLED:      return "CANCELLED";
        case ReservationStatus::REJECTED:       return "REJECTED";
    }
    return "PENDING_REVIEW";
}

inline std::optional<ReservationStatus> reservationStatusFrom(const std::string& s) {
    if (s == "PENDING_REVIEW") return ReservationStatus::PENDING_REVIEW;
    if (s == "LOCKED")         return ReservationStatus::LOCKED;
    if (s == "IN_USE")         return ReservationStatus::IN_USE;
    if (s == "DUE_RETURN")     return ReservationStatus::DUE_RETURN;
    if (s == "RETURN_CHECK")   return ReservationStatus::RETURN_CHECK;
    if (s == "COMPLETED")      return ReservationStatus::COMPLETED;
    if (s == "CANCELLED")      return ReservationStatus::CANCELLED;
    if (s == "REJECTED")       return ReservationStatus::REJECTED;
    return std::nullopt;
}

/** 预约单状态中文标签（只读展示，不参与写入） */
inline const char* labelOf(ReservationStatus v) {
    switch (v) {
        case ReservationStatus::PENDING_REVIEW: return "待审核";
        case ReservationStatus::LOCKED:         return "已锁定";
        case ReservationStatus::IN_USE:         return "使用中";
        case ReservationStatus::DUE_RETURN:     return "待归还";
        case ReservationStatus::RETURN_CHECK:   return "归还验收中";
        case ReservationStatus::COMPLETED:      return "已完成";
        case ReservationStatus::CANCELLED:      return "已取消";
        case ReservationStatus::REJECTED:       return "已驳回";
    }
    return "待审核";
}

/**
 * 是否属于「有效预约」（D-4）。
 * 与 isActiveReservationStatus(const std::string&) 严格同集合，二者必须同时修改。
 */
inline bool isActiveReservation(ReservationStatus v) {
    return v == ReservationStatus::PENDING_REVIEW || v == ReservationStatus::LOCKED ||
           v == ReservationStatus::IN_USE || v == ReservationStatus::DUE_RETURN ||
           v == ReservationStatus::RETURN_CHECK;
}

/** 预约实体（DB 行映射，不含 resource 概要；ARCH-P1 §3.3） */
struct Reservation {
    int64_t           id = 0;
    std::string       reservationNo;
    int64_t           userId     = 0;
    int64_t           resourceId = 0;
    std::string       startTime;   ///< ISO 字符串，来自 DATE_FORMAT
    std::string       endTime;
    ReservationStatus status        = ReservationStatus::PENDING_REVIEW;
    double            depositAmount = 0.0;
    std::string       remark, createdAt, updatedAt;

    /** 接口 JSON（含 statusLabel，与 Resource::toJson 风格一致） */
    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]            = id;
        j["reservationNo"] = reservationNo;
        j["userId"]        = userId;
        j["resourceId"]    = resourceId;
        j["startTime"]     = startTime;
        j["endTime"]       = endTime;
        j["status"]        = toStr(status);
        j["statusLabel"]   = labelOf(status);
        j["depositAmount"] = round2(depositAmount);
        j["remark"]        = remark;
        j["createdAt"]     = createdAt;
        j["updatedAt"]     = updatedAt;
        return j;
    }

    /**
     * 行 → 实体。列序契约（与 ReservationRepo 的 SELECT 严格一致）：
     * id, reservation_no, user_id, resource_id, start_time, end_time,
     * status, deposit_amount, remark, created_at, updated_at
     * @param row 每列 std::optional<std::string>，SQL NULL → nullopt
     */
    static Reservation fromRow(const std::vector<std::optional<std::string>>& row) {
        Reservation r;
        if (row.size() < 11) {
            return r;
        }
        r.id            = i64At(row, 0);
        r.reservationNo = strAt(row, 1);
        r.userId        = i64At(row, 2);
        r.resourceId    = i64At(row, 3);
        r.startTime     = strAt(row, 4);
        r.endTime       = strAt(row, 5);
        r.status        = reservationStatusFrom(strAt(row, 6))
                              .value_or(ReservationStatus::PENDING_REVIEW);
        r.depositAmount = dblAt(row, 7);
        r.remark        = strAt(row, 8);
        r.createdAt     = strAt(row, 9);
        r.updatedAt     = strAt(row, 10);
        return r;
    }

private:
    static std::string strAt(const std::vector<std::optional<std::string>>& row, size_t i) {
        return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
    }

    static int64_t i64At(const std::vector<std::optional<std::string>>& row, size_t i) {
        const std::string v = strAt(row, i);
        if (v.empty()) {
            return 0;
        }
        return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
    }

    static double dblAt(const std::vector<std::optional<std::string>>& row, size_t i) {
        const std::string v = strAt(row, i);
        if (v.empty()) {
            return 0.0;
        }
        return std::strtod(v.c_str(), nullptr);
    }

    /** 金额保留两位小数（DECIMAL(10,2) 语义） */
    static double round2(double v) {
        return static_cast<double>(static_cast<int64_t>(v * 100.0 + (v >= 0 ? 0.5 : -0.5))) / 100.0;
    }
};

// ============================================================ P2 增量（ARCH-P2 §3.2）
/**
 * P2 三个新实体的公共行取值工具。
 * 不做成成员函数是因为三个 struct 都要用，重复三份没必要；
 * 与既有 struct 的私有 strAt 互不影响（那是各自的实现细节，不动）。
 */
namespace detail {

inline std::string rowStr(const std::vector<std::optional<std::string>>& row, size_t i) {
    return (i < row.size() && row[i].has_value()) ? *row[i] : std::string();
}

inline int64_t rowI64(const std::vector<std::optional<std::string>>& row, size_t i) {
    const std::string v = rowStr(row, i);
    if (v.empty()) {
        return 0;
    }
    return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
}

inline double rowDbl(const std::vector<std::optional<std::string>>& row, size_t i) {
    const std::string v = rowStr(row, i);
    if (v.empty()) {
        return 0.0;
    }
    return std::strtod(v.c_str(), nullptr);
}

/** 金额保留两位小数（DECIMAL(10,2) 语义） */
inline double round2(double v) {
    return static_cast<double>(static_cast<int64_t>(v * 100.0 + (v >= 0 ? 0.5 : -0.5))) / 100.0;
}

}  // namespace detail

/** 押金流水类型（deposit_record.record_type） */
enum class DepositRecordType { FREEZE, DEDUCT, REFUND };

inline std::string toStr(DepositRecordType v) {
    switch (v) {
        case DepositRecordType::DEDUCT: return "DEDUCT";
        case DepositRecordType::REFUND: return "REFUND";
        case DepositRecordType::FREEZE:
        default:                        return "FREEZE";
    }
}

inline std::optional<DepositRecordType> depositRecordTypeFrom(const std::string& s) {
    if (s == "FREEZE") return DepositRecordType::FREEZE;
    if (s == "DEDUCT") return DepositRecordType::DEDUCT;
    if (s == "REFUND") return DepositRecordType::REFUND;
    return std::nullopt;
}

/** 违约类型（violation_record.violation_type） */
enum class ViolationType { OVERDUE, DAMAGE, NO_SHOW };

inline std::string toStr(ViolationType v) {
    switch (v) {
        case ViolationType::DAMAGE:  return "DAMAGE";
        case ViolationType::NO_SHOW: return "NO_SHOW";
        case ViolationType::OVERDUE:
        default:                     return "OVERDUE";
    }
}

inline std::optional<ViolationType> violationTypeFrom(const std::string& s) {
    if (s == "OVERDUE") return ViolationType::OVERDUE;
    if (s == "DAMAGE")  return ViolationType::DAMAGE;
    if (s == "NO_SHOW") return ViolationType::NO_SHOW;
    return std::nullopt;
}

/** 候补状态（waitlist.status，取值与 DDL 注释一致） */
enum class WaitlistStatus { WAITING, OFFERED, CONVERTED, CANCELLED, EXPIRED };

inline std::string toStr(WaitlistStatus v) {
    switch (v) {
        case WaitlistStatus::OFFERED:  return "OFFERED";
        case WaitlistStatus::CONVERTED: return "CONVERTED";
        case WaitlistStatus::CANCELLED: return "CANCELLED";
        case WaitlistStatus::EXPIRED:   return "EXPIRED";
        case WaitlistStatus::WAITING:
        default:                        return "WAITING";
    }
}

inline std::optional<WaitlistStatus> waitlistStatusFrom(const std::string& s) {
    if (s == "WAITING")   return WaitlistStatus::WAITING;
    if (s == "OFFERED")  return WaitlistStatus::OFFERED;
    if (s == "CONVERTED") return WaitlistStatus::CONVERTED;
    if (s == "CANCELLED") return WaitlistStatus::CANCELLED;
    if (s == "EXPIRED")   return WaitlistStatus::EXPIRED;
    return std::nullopt;
}

/** 候补状态中文标签 */
inline const char* labelOf(WaitlistStatus v) {
    switch (v) {
        case WaitlistStatus::OFFERED:  return "已获名额待确认";
        case WaitlistStatus::CONVERTED: return "已转正式预约";
        case WaitlistStatus::CANCELLED: return "已退出";
        case WaitlistStatus::EXPIRED:   return "名额已过期";
        case WaitlistStatus::WAITING:
        default:                        return "排队中";
    }
}

/** 押金流水（ARCH-P2 §6 / D-P2-11：账目意图流水，不是资金账户） */
struct DepositRecord {
    int64_t     id            = 0;
    int64_t     reservationId = 0;
    int64_t     userId        = 0;
    double      amount        = 0.0;
    std::string recordType;      ///< FREEZE / DEDUCT / REFUND
    std::string status;          ///< P2 恒 SUCCESS（表示「已记账」）
    std::string transactionNo;   ///< 幂等键 uk_deposit_txn
    std::string remark, createdAt;

    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]            = id;
        j["reservationId"] = reservationId;
        j["userId"]        = userId;
        j["amount"]        = detail::round2(amount);
        j["recordType"]    = recordType;
        j["status"]        = status;
        j["transactionNo"] = transactionNo;
        j["remark"]        = remark;
        j["createdAt"]     = createdAt;
        return j;
    }

    /**
     * 行 → 实体。列序契约：
     * id, reservation_id, user_id, amount, record_type, status, transaction_no, remark, created_at
     */
    static DepositRecord fromRow(const std::vector<std::optional<std::string>>& row) {
        DepositRecord d;
        if (row.size() < 9) {
            return d;
        }
        d.id            = detail::rowI64(row, 0);
        d.reservationId = detail::rowI64(row, 1);
        d.userId        = detail::rowI64(row, 2);
        d.amount        = detail::rowDbl(row, 3);
        d.recordType    = detail::rowStr(row, 4);
        d.status        = detail::rowStr(row, 5);
        d.transactionNo = detail::rowStr(row, 6);
        d.remark        = detail::rowStr(row, 7);
        d.createdAt     = detail::rowStr(row, 8);
        return d;
    }
};

/** 违约记录（ARCH-P2 §1.4 第 2 层：uk_violation_dedup 是扣分幂等的最终防线） */
struct ViolationRecord {
    int64_t     id            = 0;
    int64_t     userId        = 0;
    int64_t     reservationId = 0;      ///< DB 可为 NULL；实体用 0 表示「无关联单据」
    std::string violationType;          ///< OVERDUE / DAMAGE / NO_SHOW
    int         deductScore   = 0;
    int         deductSeq     = 0;      ///< 周期序号；一次性违约恒 0
    int64_t     operatorId    = 0;
    std::string remark, createdAt;

    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]            = id;
        j["userId"]        = userId;
        j["reservationId"] = reservationId;
        j["violationType"] = violationType;
        j["deductScore"]   = deductScore;
        j["deductSeq"]     = deductSeq;
        j["operatorId"]    = operatorId;
        j["remark"]        = remark;
        j["createdAt"]     = createdAt;
        return j;
    }

    /**
     * 行 → 实体。列序契约：
     * id, user_id, reservation_id, violation_type, deduct_score, deduct_seq,
     * operator_id, remark, created_at
     */
    static ViolationRecord fromRow(const std::vector<std::optional<std::string>>& row) {
        ViolationRecord v;
        if (row.size() < 9) {
            return v;
        }
        v.id            = detail::rowI64(row, 0);
        v.userId        = detail::rowI64(row, 1);
        v.reservationId = detail::rowI64(row, 2);
        v.violationType = detail::rowStr(row, 3);
        v.deductScore   = static_cast<int>(detail::rowI64(row, 4));
        v.deductSeq     = static_cast<int>(detail::rowI64(row, 5));
        v.operatorId    = detail::rowI64(row, 6);
        v.remark        = detail::rowStr(row, 7);
        v.createdAt     = detail::rowStr(row, 8);
        return v;
    }
};

/** 候补队列项（ARCH-P2 §7.2 / §7.3） */
struct WaitlistItem {
    int64_t      id            = 0;
    int64_t      resourceId    = 0;
    int64_t      userId        = 0;
    std::string  slotDate;                  ///< "yyyy-MM-dd"
    int          startIndex    = 0;         ///< 含
    int          endIndex      = 0;         ///< 不含
    WaitlistStatus status      = WaitlistStatus::WAITING;
    std::string  offerExpiresAt;            ///< 空串表示 NULL
    int64_t      convertedReservationId = 0;///< 0 表示 NULL
    std::string  remark, createdAt, updatedAt;

    /** FIFO 名次（1 起；由 Service 用 COUNT(*) 算出，非 DB 列） */
    int64_t      position      = 0;
    /** 展示用：资源编号/名称（list 接口 LEFT JOIN 可得，未 JOIN 时为空） */
    std::string  resourceCode, resourceName;

    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]                     = id;
        j["resourceId"]             = resourceId;
        j["resourceCode"]           = resourceCode;
        j["resourceName"]           = resourceName;
        j["userId"]                 = userId;
        j["slotDate"]               = slotDate;
        j["startIndex"]             = startIndex;
        j["endIndex"]               = endIndex;
        j["status"]                 = toStr(status);
        j["statusLabel"]            = labelOf(status);
        j["offerExpiresAt"]         = offerExpiresAt;
        j["convertedReservationId"] = convertedReservationId;
        j["remark"]                 = remark;
        j["position"]               = position;
        j["createdAt"]              = createdAt;
        j["updatedAt"]              = updatedAt;
        return j;
    }

    /**
     * 行 → 实体。列序契约：
     * id, resource_id, user_id, slot_date, start_index, end_index, status,
     * offer_expires_at, converted_reservation_id, remark, created_at, updated_at
     */
    static WaitlistItem fromRow(const std::vector<std::optional<std::string>>& row) {
        WaitlistItem w;
        if (row.size() < 12) {
            return w;
        }
        w.id            = detail::rowI64(row, 0);
        w.resourceId    = detail::rowI64(row, 1);
        w.userId        = detail::rowI64(row, 2);
        w.slotDate      = detail::rowStr(row, 3);
        w.startIndex    = static_cast<int>(detail::rowI64(row, 4));
        w.endIndex      = static_cast<int>(detail::rowI64(row, 5));
        w.status        = waitlistStatusFrom(detail::rowStr(row, 6))
                              .value_or(WaitlistStatus::WAITING);
        w.offerExpiresAt = detail::rowStr(row, 7);       // NULL → 空串
        w.convertedReservationId = detail::rowI64(row, 8);
        w.remark        = detail::rowStr(row, 9);
        w.createdAt     = detail::rowStr(row, 10);
        w.updatedAt     = detail::rowStr(row, 11);
        return w;
    }
};

// ============================================================ P3 增量（ARCH-P3 §6 / §7.4）
/**
 * 信用分变动原因（credit_log.reason_type）。
 * AUTO_RECOVER 是「解除黑名单」；P3 不做随时间自动加分（主理人决策 4），
 * 但类型先定义好，P4 若要实现「无违约 N 天 +5」直接复用。
 */
enum class CreditReasonType {
    NO_SHOW, OVERDUE, DAMAGE, MANUAL, AUTO_BLACKLIST, AUTO_RECOVER, AUTO_REPAIR
};

inline std::string toStr(CreditReasonType v) {
    switch (v) {
        case CreditReasonType::OVERDUE:       return "OVERDUE";
        case CreditReasonType::DAMAGE:        return "DAMAGE";
        case CreditReasonType::MANUAL:        return "MANUAL";
        case CreditReasonType::AUTO_BLACKLIST:return "AUTO_BLACKLIST";
        case CreditReasonType::AUTO_RECOVER:  return "AUTO_RECOVER";
        case CreditReasonType::AUTO_REPAIR:   return "AUTO_REPAIR";
        case CreditReasonType::NO_SHOW:
        default:                              return "NO_SHOW";
    }
}

inline std::optional<CreditReasonType> creditReasonTypeFrom(const std::string& s) {
    if (s == "NO_SHOW")        return CreditReasonType::NO_SHOW;
    if (s == "OVERDUE")        return CreditReasonType::OVERDUE;
    if (s == "DAMAGE")         return CreditReasonType::DAMAGE;
    if (s == "MANUAL")         return CreditReasonType::MANUAL;
    if (s == "AUTO_BLACKLIST") return CreditReasonType::AUTO_BLACKLIST;
    if (s == "AUTO_RECOVER")   return CreditReasonType::AUTO_RECOVER;
    if (s == "AUTO_REPAIR")    return CreditReasonType::AUTO_REPAIR;
    return std::nullopt;
}

/** 信用变动原因中文标签（只读展示） */
inline const char* labelOf(CreditReasonType v) {
    switch (v) {
        case CreditReasonType::OVERDUE:        return "逾期归还";
        case CreditReasonType::DAMAGE:         return "损坏赔偿";
        case CreditReasonType::MANUAL:         return "管理员调整";
        case CreditReasonType::AUTO_BLACKLIST: return "自动进入黑名单";
        case CreditReasonType::AUTO_RECOVER:   return "自动解除黑名单";
        case CreditReasonType::AUTO_REPAIR:    return "定时纠偏";
        case CreditReasonType::NO_SHOW:
        default:                               return "爽约";
    }
}

/**
 * 信用分变动明细（credit_log 一行，D-P3-8）。
 * 列序契约（与 CreditRepo 的 SELECT 严格一致）：
 *   id, user_id, reservation_id, change_score, score_before, score_after,
 *   status_before, status_after, reason_type, rule_code, source_ref,
 *   operator_id, remark, created_at
 */
struct CreditLog {
    int64_t     id            = 0;
    int64_t     userId        = 0;
    int64_t     reservationId = 0;      ///< 0 表示 DB 的 NULL（手工/自动调整）
    int         changeScore   = 0;      ///< 负数扣分，正数恢复
    int         scoreBefore   = 0;
    int         scoreAfter    = 0;
    std::string statusBefore, statusAfter;
    std::string reasonType;             ///< NO_SHOW/OVERDUE/DAMAGE/MANUAL/AUTO_*
    std::string ruleCode;               ///< 空串表示 DB 的 NULL（非规则触发）
    std::string sourceRef;              ///< 幂等键 uk_credit_source
    int64_t     operatorId    = 0;      ///< 0 表示 DB 的 NULL（系统触发）
    std::string remark, createdAt;

    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["id"]            = id;
        j["userId"]        = userId;
        j["reservationId"] = reservationId;
        j["changeScore"]   = changeScore;
        j["scoreBefore"]   = scoreBefore;
        j["scoreAfter"]    = scoreAfter;
        j["statusBefore"]  = statusBefore;
        j["statusAfter"]   = statusAfter;
        j["reasonType"]    = reasonType;
        j["reasonLabel"]   = "未知";
        if (const auto r = creditReasonTypeFrom(reasonType); r.has_value()) {
            j["reasonLabel"] = labelOf(*r);
        }
        if (!ruleCode.empty()) {
            j["ruleCode"] = ruleCode;
        }
        j["sourceRef"] = sourceRef;
        if (operatorId > 0) {
            j["operatorId"] = operatorId;
        }
        j["remark"]    = remark;
        j["createdAt"] = createdAt;
        return j;
    }

    /** 行 → 实体（14 列，NULL 映射见字段注释） */
    static CreditLog fromRow(const std::vector<std::optional<std::string>>& row) {
        CreditLog c;
        if (row.size() < 14) {
            return c;
        }
        c.id            = detail::rowI64(row, 0);
        c.userId        = detail::rowI64(row, 1);
        c.reservationId = detail::rowI64(row, 2);
        c.changeScore   = static_cast<int>(detail::rowI64(row, 3));
        c.scoreBefore   = static_cast<int>(detail::rowI64(row, 4));
        c.scoreAfter    = static_cast<int>(detail::rowI64(row, 5));
        c.statusBefore  = detail::rowStr(row, 6);
        c.statusAfter   = detail::rowStr(row, 7);
        c.reasonType    = detail::rowStr(row, 8);
        c.ruleCode      = detail::rowStr(row, 9);   // NULL → 空串
        c.sourceRef     = detail::rowStr(row, 10);
        c.operatorId    = detail::rowI64(row, 11);
        c.remark        = detail::rowStr(row, 12);
        c.createdAt     = detail::rowStr(row, 13);
        return c;
    }
};

/**
 * 信誉概览（/credit/me 与管理员查单用户都返回它）。
 * 距离字段是给前端直接展示「再扣 N 分进黑名单 / 再涨 M 分解除」。
 */
struct UserCreditView {
    int64_t     userId   = 0;
    std::string username;
    int         score    = 100;
    std::string status;                 ///< NORMAL / BLACKLIST / DISABLED
    int         enterScore = 60;
    int         exitScore  = 80;

    /**
     * 还差多少分进黑名单；对已经在黑名单/停用的账号无意义 → 0。
     * 与 ARCH-P3 附录 B 的示例一致（score=60/NORMAL → 1）。
     */
    int distanceToBlacklist() const {
        if (status != "NORMAL") {
            return 0;
        }
        const int d = score - enterScore + 1;
        return d > 0 ? d : 0;
    }
    /** 还差多少分解除黑名单；非黑名单账号无意义 → 0。 */
    int distanceToRecover() const {
        if (status != "BLACKLIST") {
            return 0;
        }
        const int d = exitScore - score;
        return d > 0 ? d : 0;
    }

    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["userId"]    = userId;
        j["username"]  = username;
        j["score"]     = score;
        j["status"]    = status;
        j["statusLabel"] = "未知";
        if (const auto r = userStatusFrom(status); r.has_value()) {
            j["statusLabel"] = labelOf(*r);
        }
        j["enterScore"]          = enterScore;
        j["exitScore"]           = exitScore;
        j["distanceToBlacklist"] = distanceToBlacklist();
        j["distanceToRecover"]   = distanceToRecover();
        if (status == "NORMAL") {
            j["hint"] = (distanceToBlacklist() <= 5)
                            ? "当前处于迟滞区边缘：再扣 " + std::to_string(distanceToBlacklist()) +
                                  " 分将进入黑名单"
                            : "信用良好";
        } else if (status == "BLACKLIST") {
            j["hint"] = "已进入黑名单：恢复到 " + std::to_string(exitScore) + " 分自动解除（还差 " +
                        std::to_string(distanceToRecover() > 0 ? distanceToRecover() : 0) + " 分）";
        } else {
            j["hint"] = "账号已被停用，请联系管理员";
        }
        return j;
    }
};

} // namespace rf::model
