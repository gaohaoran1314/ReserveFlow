// =============================================================================
// src/common/Result.h  —  统一返回信封与错误码（header-only）
// 对应 ARCH-P0 §3.1 / D-6 / §8.5：业务失败一律 HTTP 200 + code != 0
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace rf {

/** 全量错误码。新增错误码必须同步 §3.1 枚举、错误码表、httpStatusOf() 三处。 */
enum ErrorCode : int {
    kOk = 0,
    // ---- 1xxx 框架 / 鉴权 / 基础设施 ----
    kInvalidParam      = 1000,  // 参数校验失败（缺字段、类型错、越界、非法枚举字面量）
    kUnauthorized      = 1001,  // 未带令牌 / 令牌格式非法 / 签名不匹配
    kTokenExpired      = 1002,  // 令牌已过期
    kTokenRevoked      = 1003,  // 令牌已登出（命中 Redis 失效名单）
    kForbidden         = 1004,  // 已登录但角色不足
    kMalformedJson     = 1005,  // 请求体不是合法 JSON
    kRouteNotFound     = 1006,  // 路由不存在
    kMethodNotAllowed  = 1007,  // 方法不允许
    kInternalError     = 1008,  // 未捕获异常
    kDbError           = 1009,  // 数据库访问失败 / 连接获取超时
    kCacheError        = 1010,  // Redis 访问失败
    kConfigError       = 1011,  // 配置缺失或非法
    // ---- 2xxx 账号 ----
    kInvalidCredential = 2000,  // 账号或密码错误（不区分账号是否存在）
    kOldPasswordWrong  = 2001,  // 原密码错误
    kAccountDisabled   = 2002,  // 账号被禁用（DISABLED）
    // ---- 2xxx 账号（P6 增量：注册与审核，ARCH-P6 §3.1 / §3.2）----
    kUsernameTaken     = 2003,  // 学号已被注册（含 PENDING 之外的任何状态）
    kAccountPending    = 2004,  // 账号待管理员审核（PENDING），暂不能登录
    kAccountRejected   = 2005,  // 注册审核未通过（REJECTED），需重新提交
    // 2006 刻意未定义：本轮不做 IP 限流，不留一个永远用不上的码（ARCH-P6 §1.5）
    kUserReviewDenied  = 2007,  // 该账号当前状态不允许审核（不是 PENDING）
    // ---- 3xxx 资源 ----
    kResourceNotFound  = 3000,  // 资源不存在（或已逻辑删除）
    kResourceCodeDup   = 3001,  // 资源编号已存在
    kResourceStatusDenied = 3002, // 该状态值 P0 不允许手工设置
    kResourceHasActiveReservation = 3003, // 资源存在有效预约，禁止删除
    // ---- 3xxx 资源 / 预约（P1 增量，ARCH-P1 §7；注意 3003 已被上方占用，故从 3004 起编）----
    kResourceBusy         = 3004,  // 资源正忙：分布式锁等待超时（D-P1-7）
    kSlotUnavailable      = 3005,  // 所选时段已被占用（唯一索引冲突 / 事务内预检命中）
    kReservationNotFound  = 3006,  // 预约不存在
    kReservationStateInvalid = 3007, // 当前状态不允许该操作（含「该迁移后续批次启用」）
    kReservationForbidden = 3008,  // 无权操作他人预约
    kReservationTimeInvalid = 3009, // 预约时间不合法（msg 已含中文原因）
    kUserBlacklisted      = 3010,  // 账号处于黑名单，无法预约
    kCreditTooLow         = 3011,  // 信用分不足，无法预约
    kResourceNotBookable  = 3012,  // 资源当前不可预约
    kReservationNoExhausted = 3013, // 当日预约号已用尽（4 位流水溢出）
    kLockLost             = 3014,  // 资源锁已失效，请重试（提交前 lock.lost() 为真）
    // ---- 3xxx 取用/押金/候补（P2 增量，ARCH-P2 §9）----
    kPickupTooEarly       = 3015,  // 尚未到可取用时间
    kPickupExpired        = 3016,  // 已超过取用时限（将被系统自动取消）
    kDepositAmountInvalid = 3017,  // 押金金额 / 扣分分值不合法
    kWaitlistNotFound     = 3018,  // 候补记录不存在
    kWaitlistStateInvalid = 3019,  // 当前候补状态不允许该操作
    kWaitlistDup          = 3020,  // 该时段已在候补队列中（或超配额）
    kOfferExpired         = 3021,  // 候补名额已过期，请重新排队
    kMaintenanceBusy      = 3022,  // 维护任务正在执行，请稍后重试
    // ---- 3xxx 规则引擎 / 信用（P3 增量，ARCH-P3 §9）----
    kRuleNotFound         = 3023,  // 规则不存在
    kRuleInvalid          = 3024,  // 规则定义非法（msg 已含中文原因）
    kRuleCodeDup          = 3025,  // 规则编码已存在（uk_rule_code）
    kCreditAdjustInvalid  = 3026,  // 信用分调整不合法
    kActiveLimitExceeded  = 3027,  // 同时进行的预约已达上限（#3 命中）
    kRuleProtected        = 3028,  // 内置规则不允许删除，请改为停用
    kRuleEngineFailClosed = 3029,  // 规则校验暂不可用（reject 类求值失败，fail-closed）
};

/** 无数据的成功返回占位类型（避免 Result<void> 特化） */
struct Empty {};

/**
 * 统一返回信封。
 * @tparam T 业务数据类型；无数据时用 rf::Empty
 */
template <typename T>
struct Result {
    int         code = kOk;
    std::string msg  = "ok";
    T           data{};

    /** 成员函数：判断成功。注意工厂函数刻意不叫 ok()，避免与成员函数同名误用 */
    bool ok() const { return code == kOk; }

    /** 构造成功结果 */
    static Result make(T v) { return Result{kOk, "ok", std::move(v)}; }

    /** 构造失败结果（data 保持默认构造值） */
    static Result fail(int c, std::string m) { return Result{c, std::move(m), T{}}; }
};

using VoidResult = Result<Empty>;

/** 便捷地就地设置失败（用于已有结果对象） */
inline void setError(VoidResult& r, int c, const char* m) {
    r.code = c;
    r.msg  = m;
}

/**
 * 错误码 → HTTP 状态码（D-6）：只有框架 / 鉴权层映射为非 200。
 * @param code 业务错误码
 * @return 对应 HTTP 状态码；业务错误一律 200
 */
inline int httpStatusOf(int code) {
    switch (code) {
        case kUnauthorized:
        case kTokenExpired:
        case kTokenRevoked:      return 401;
        case kForbidden:         return 403;
        case kMalformedJson:     return 400;
        case kRouteNotFound:     return 404;
        case kMethodNotAllowed:  return 405;
        case kInternalError:
        case kDbError:
        case kCacheError:
        case kConfigError:       return 500;
        default:                 return 200;   // 0 与所有业务错误（1xxx 参数/2xxx/3xxx）
    }
}

} // namespace rf
