// =============================================================================
// src/StateMachine.cpp  —  静态规则表 + 迁移校验 + 资源状态收敛实现
// 对应 ARCH-P1 §3.4 / §4.1 / §4.2 / §4.3
//
// 关键实现约定：
//   1) 规则表 kRules 是**唯一**的迁移知识来源；新增批次只改规则的 phase 字段，
//      或把 config.feature.phase 调成 2，不改本文件任何逻辑（D-P1-1）；
//   2) plan() 是纯函数：只读规则表，不写库、不打日志副作用、不抛异常；
//   3) 资源状态用收敛函数（D-P1-2）：输入「有效预约集合」，输出粗粒度资源态，
//      人工态（MAINTENANCE/DISABLED/DAMAGE_CLAIM）由双参重载保护，不被覆盖。
// =============================================================================
#include "StateMachine.h"

#include <string>
#include <utility>
#include <vector>

namespace rf {

std::string toStr(BizType b) {
    switch (b) {
        case BizType::RESOURCE:    return "RESOURCE";
        case BizType::RESERVATION:
        default:                   return "RESERVATION";
    }
}

std::string toStr(TransitEvent e) {
    switch (e) {
        case TransitEvent::CREATE:        return "CREATE";
        case TransitEvent::APPROVE:       return "APPROVE";
        case TransitEvent::REJECT:        return "REJECT";
        case TransitEvent::CANCEL:        return "CANCEL";
        case TransitEvent::START_USE:     return "START_USE";
        case TransitEvent::REPORT_DUE:    return "REPORT_DUE";
        case TransitEvent::SUBMIT_RETURN: return "SUBMIT_RETURN";
        case TransitEvent::PASS_CHECK:    return "PASS_CHECK";
        case TransitEvent::MARK_DAMAGE:   return "MARK_DAMAGE";
        case TransitEvent::HAND_MAINTAIN: return "HAND_MAINTAIN";
        case TransitEvent::HAND_FREE:     return "HAND_FREE";
        case TransitEvent::TIMEOUT:       return "TIMEOUT";
    }
    return "CREATE";
}

std::optional<TransitEvent> transitEventFrom(const std::string& s) {
    if (s == "CREATE")        return TransitEvent::CREATE;
    if (s == "APPROVE")       return TransitEvent::APPROVE;
    if (s == "REJECT")        return TransitEvent::REJECT;
    if (s == "CANCEL")        return TransitEvent::CANCEL;
    if (s == "START_USE")     return TransitEvent::START_USE;
    if (s == "REPORT_DUE")    return TransitEvent::REPORT_DUE;
    if (s == "SUBMIT_RETURN") return TransitEvent::SUBMIT_RETURN;
    if (s == "PASS_CHECK")    return TransitEvent::PASS_CHECK;
    if (s == "MARK_DAMAGE")   return TransitEvent::MARK_DAMAGE;
    if (s == "HAND_MAINTAIN") return TransitEvent::HAND_MAINTAIN;
    if (s == "HAND_FREE")     return TransitEvent::HAND_FREE;
    if (s == "TIMEOUT")       return TransitEvent::TIMEOUT;
    return std::nullopt;
}

/** 规则 from 列的安全取值（nullptr 视为空串） */
namespace {
std::string fromOf(const TransitionRule& r) {
    return r.from == nullptr ? std::string() : std::string(r.from);
}
}  // namespace

// ============================================================================
// 静态规则表
// ============================================================================
const std::vector<TransitionRule>& StateMachine::rules() {
    // clang-format off
    static const std::vector<TransitionRule> kRules = {
        // ---------------- 预约单（business_type = RESERVATION）§4.1 #1~#11 ----------------
        // #1 需审核类型（LAB_EQUIPMENT）：创建 → 待审核。事件 CREATE 的两条规则靠
        //    TransitRequest::resourceType 分流（D-P1-9），由 plan() 负责二选一。
        {BizType::RESERVATION, "",
         TransitEvent::CREATE, "PENDING_REVIEW", 1, "需审核资源：提交后进入待审核"},
        // #2 免审核类型（MEETING_ROOM / LOCKER）：创建 → 直接锁定
        {BizType::RESERVATION, "",
         TransitEvent::CREATE, "LOCKED", 1, "免审核类型，直接锁定"},
        // #3 审核通过：槽位不变
        {BizType::RESERVATION, "PENDING_REVIEW",
         TransitEvent::APPROVE, "LOCKED", 1, "管理员审核通过，锁定所选时段"},
        // #4 驳回：释放槽位
        {BizType::RESERVATION, "PENDING_REVIEW",
         TransitEvent::REJECT, "REJECTED", 1, "管理员驳回预约申请"},
        // #5 待审核状态取消：释放槽位
        {BizType::RESERVATION, "PENDING_REVIEW",
         TransitEvent::CANCEL, "CANCELLED", 1, "预约人取消待审核预约"},
        // #6 已锁定状态取消：释放槽位（前置条件 now < start_time 由 Service 校验）
        {BizType::RESERVATION, "LOCKED",
         TransitEvent::CANCEL, "CANCELLED", 1, "预约人取消已锁定预约"},

        // ---------------- 预约单：P2 规则（表内已定义，phase=1 时返回 3007）§4.1 #7~#11 ----
        // #7 开始使用
        {BizType::RESERVATION, "LOCKED",
         TransitEvent::START_USE, "IN_USE", 2, "预约人开始使用资源"},
        // #8 到期未还
        {BizType::RESERVATION, "IN_USE",
         TransitEvent::REPORT_DUE, "DUE_RETURN", 2, "资源到期未归还"},
        // #9 提交归还
        {BizType::RESERVATION, "DUE_RETURN",
         TransitEvent::SUBMIT_RETURN, "RETURN_CHECK", 2, "预约人提交归还，等待验收"},
        // #10 验收通过
        {BizType::RESERVATION, "RETURN_CHECK",
         TransitEvent::PASS_CHECK, "COMPLETED", 2, "管理员验收通过，流程结束"},
        // #11 验收损坏
        {BizType::RESERVATION, "RETURN_CHECK",
         TransitEvent::MARK_DAMAGE, "COMPLETED", 2, "资源损坏，转入理赔后结束流程"},

        // ---------------- 资源手工操作（business_type = RESOURCE）§4.2 #13~#16 ----------------
        // #13 管理员新增资源
        {BizType::RESOURCE, "",
         TransitEvent::CREATE, "FREE", 1, "管理员新增资源"},
        // #14 手工置维护（P0 已实现，P1 纳入统一校验）
        {BizType::RESOURCE, "FREE",
         TransitEvent::HAND_MAINTAIN, "MAINTENANCE", 1, "管理员手工置维护中"},
        // #15 手工恢复可用（P0 已实现）
        {BizType::RESOURCE, "MAINTENANCE",
         TransitEvent::HAND_FREE, "FREE", 1, "管理员手工恢复可用"},
        // #16 收敛函数产生的「回到可用」：FREE / MAINTENANCE → FREE（系统触发）
        {BizType::RESOURCE, "FREE",
         TransitEvent::CANCEL, "FREE", 1, "有效预约集合已清空，资源收敛为可用"},
        {BizType::RESOURCE, "MAINTENANCE",
         TransitEvent::CANCEL, "FREE", 1, "有效预约集合已清空，资源收敛为可用"},

        // ---------------- P2 增量：预约单补 4 条（ARCH-P2 §4.1 #17~#20）----------------
        // #17 P1 缺的一条边：用户在 IN_USE 期间提前归还没有合法边（D-P2-2）
        {BizType::RESERVATION, "IN_USE",
         TransitEvent::SUBMIT_RETURN, "RETURN_CHECK", 2, "用户提交归还，等待验收"},
        // #18 待审核超时（定时任务；rule_code=RESERVE_REVIEW_TIMEOUT）
        {BizType::RESERVATION, "PENDING_REVIEW",
         TransitEvent::TIMEOUT, "REJECTED", 2, "待审核超时，系统自动驳回"},
        // #19 爽约（定时任务；rule_code=RESERVE_NO_SHOW，并扣 NO_SHOW 信用分）
        {BizType::RESERVATION, "LOCKED",
         TransitEvent::TIMEOUT, "CANCELLED", 2, "超时未取用，系统取消预约"},
        // #20 长期未还 / 实物无法归还时人工结案（D-P2-7）
        {BizType::RESERVATION, "DUE_RETURN",
         TransitEvent::MARK_DAMAGE, "COMPLETED", 2, "逾期未还，管理员人工结案并认定损坏"},

        // ---------------- P2 增量：资源补 2 条（ARCH-P2 §4.2 #21~#22）----------------
        // #21 任意资源态 → DAMAGE_CLAIM（通配前态，仅 RESOURCE 允许，D-P2-3）
        {BizType::RESOURCE, kAnyFrom,
         TransitEvent::MARK_DAMAGE, "DAMAGE_CLAIM", 2, "资源损坏，进入理赔态"},
        // #22 理赔结案后恢复可用（P2 过渡出口，P3 理赔接口接入后取代）
        {BizType::RESOURCE, "DAMAGE_CLAIM",
         TransitEvent::HAND_FREE, "FREE", 2, "理赔处理完毕，恢复可用"},
    };
    // clang-format on
    return kRules;
}

VoidResult StateMachine::checkRuleTable(const std::vector<TransitionRule>& tbl) {
    for (const auto& r : tbl) {
        if (r.biz == BizType::RESERVATION && fromOf(r) == kAnyFrom) {
            return VoidResult::fail(
                kInternalError,
                "状态机规则表非法：预约单（RESERVATION）不允许通配前态 \"" +
                    std::string(kAnyFrom) + "\"（事件 " + toStr(r.event) +
                    "）——预约单必须逐条列举合法迁移，防止出现「全状态可迁移」");
        }
    }
    return VoidResult::make(Empty{});
}

// ============================================================================
// StateMachine
// ============================================================================
StateMachine::StateMachine(int enabledPhase) : enabledPhase_(enabledPhase) {
    if (enabledPhase_ < 1) {
        enabledPhase_ = 1;   // 非法值收敛到 P1，避免「所有迁移都被拒绝」的静默故障
    }
}

bool StateMachine::isManualState(model::ResourceState s) {
    return s == model::ResourceState::MAINTENANCE || s == model::ResourceState::DISABLED ||
           s == model::ResourceState::DAMAGE_CLAIM;
}

Result<TransitDecision> StateMachine::plan(const TransitRequest& req) const {
    const std::string from      = req.fromStatus;
    const std::string fromLabel = from.empty() ? std::string("(创建)") : from;

    // ---- ⓿ 表自身不变量：RESERVATION 禁止通配前态（D-P2-3）
    const VoidResult tbl = checkRuleTable(rules());
    if (!tbl.ok()) {
        return Result<TransitDecision>::fail(tbl.code, tbl.msg);
    }

    // ---- ① 按 (biz, from, event) 收集候选规则；from=="*" 为通配（仅 RESOURCE 有此能力）
    std::vector<const TransitionRule*> cand;
    cand.reserve(2);
    for (const auto& r : rules()) {
        if (r.biz != req.biz) {
            continue;
        }
        if (r.event != req.event) {
            continue;
        }
        const std::string ruleFrom = fromOf(r);
        const bool        wildcard = (ruleFrom == kAnyFrom);
        if (!wildcard && ruleFrom != from) {
            continue;
        }
        cand.push_back(&r);
    }
    if (cand.empty()) {
        return Result<TransitDecision>::fail(
            kReservationStateInvalid,
            "当前状态不允许该操作：" + fromLabel + " 不允许事件 " + toStr(req.event));
    }

    // ---- ② 创建场景的免审核分流（D-P1-9）：由资源 type 决定落到哪个目标态 ----
    if (req.biz == BizType::RESERVATION && req.event == TransitEvent::CREATE) {
        if (!req.resourceType.has_value()) {
            return Result<TransitDecision>::fail(
                kInvalidParam, "创建预约必须提供资源类型（resourceType），否则无法判定审核分流");
        }
        const char* want = (*req.resourceType == model::ResourceType::LAB_EQUIPMENT)
                               ? "PENDING_REVIEW"
                               : "LOCKED";
        std::vector<const TransitionRule*> filtered;
        filtered.reserve(cand.size());
        for (const TransitionRule* r : cand) {
            if (std::string(r->to) == want) {
                filtered.push_back(r);
            }
        }
        if (filtered.empty()) {
            return Result<TransitDecision>::fail(
                kInvalidParam,
                std::string("创建预约缺少对应的分流规则：资源类型 ") +
                    model::toStr(*req.resourceType) + " 应落入 " + want);
        }
        cand.swap(filtered);
    }

    // ---- ③ phase 门控（D-P1-1）----
    const TransitionRule* chosen = nullptr;
    const TransitionRule* future = nullptr;
    for (const TransitionRule* r : cand) {
        if (r->phase <= enabledPhase_) {
            chosen = r;
            break;
        }
        if (future == nullptr) {
            future = r;
        }
    }
    if (chosen == nullptr) {
        if (future != nullptr) {
            const std::string to = future->to == nullptr ? std::string() : std::string(future->to);
            return Result<TransitDecision>::fail(
                kReservationStateInvalid,
                "该迁移将在后续批次启用：" + fromLabel + " --" + toStr(req.event) + "--> " + to +
                    "（规则 phase=" + std::to_string(future->phase) + "，当前 phase=" +
                    std::to_string(enabledPhase_) + "）");
        }
        return Result<TransitDecision>::fail(
            kReservationStateInvalid,
            "当前状态不允许该操作：" + fromLabel + " 不允许事件 " + toStr(req.event));
    }

    // ---- ④ 组装结论：reason 有外部值则覆盖规则默认文案 ----
    TransitDecision d;
    d.toStatus = chosen->to == nullptr ? std::string() : std::string(chosen->to);
    d.reason   = req.reason.empty()
                     ? (chosen->reason == nullptr ? std::string() : std::string(chosen->reason))
                     : req.reason;
    if (d.toStatus.empty()) {
        return Result<TransitDecision>::fail(kInternalError, "状态机规则表存在非法目标状态（空串）");
    }
    return Result<TransitDecision>::make(std::move(d));
}

bool StateMachine::canTransit(BizType biz, const std::string& from, TransitEvent event) const {
    for (const auto& r : rules()) {
        if (r.biz != biz || r.event != event || r.phase > enabledPhase_) {
            continue;
        }
        const std::string ruleFrom = fromOf(r);
        if (ruleFrom == from || ruleFrom == kAnyFrom) {
            return true;
        }
    }
    return false;
}

std::vector<TransitEvent> StateMachine::allowedEvents(BizType biz,
                                                      const std::string& from) const {
    std::vector<TransitEvent> out;
    for (const auto& r : rules()) {
        if (r.biz != biz || r.phase > enabledPhase_) {
            continue;
        }
        const std::string ruleFrom = fromOf(r);
        if (ruleFrom != from && ruleFrom != kAnyFrom) {
            continue;
        }
        bool dup = false;
        for (const TransitEvent e : out) {
            if (e == r.event) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out.push_back(r.event);
        }
    }
    return out;
}

model::ResourceState StateMachine::convergeResource(
    const std::vector<std::string>& activeStatuses) const {
    bool hasInUse       = false;
    bool hasReturnCheck = false;
    bool hasDueReturn   = false;
    bool hasLocked      = false;
    bool hasPending     = false;

    for (const std::string& s : activeStatuses) {
        if (s == "IN_USE") {
            hasInUse = true;
        } else if (s == "RETURN_CHECK") {
            hasReturnCheck = true;
        } else if (s == "DUE_RETURN") {
            hasDueReturn = true;
        } else if (s == "LOCKED") {
            hasLocked = true;
        } else if (s == "PENDING_REVIEW") {
            hasPending = true;
        }
        // CANCELLED / REJECTED / COMPLETED / EXPIRED 及未知字面量不参与收敛
    }

    // phase=2 才允许产出 IN_USE / RETURN_CHECK / DUE_RETURN（D-P1-12）
    if (enabledPhase_ >= 2) {
        if (hasInUse) {
            return model::ResourceState::IN_USE;
        }
        if (hasReturnCheck) {
            return model::ResourceState::RETURN_CHECK;
        }
        if (hasDueReturn) {
            return model::ResourceState::DUE_RETURN;
        }
    }
    if (hasLocked) {
        return model::ResourceState::LOCKED;
    }
    if (hasPending) {
        return model::ResourceState::PENDING_REVIEW;
    }
    return model::ResourceState::FREE;
}

model::ResourceState StateMachine::convergeResource(
    const std::string& currentStatus, const std::vector<std::string>& activeStatuses) const {
    const std::optional<model::ResourceState> cur = model::resourceStateFrom(currentStatus);
    if (cur.has_value() && isManualState(*cur)) {
        return *cur;   // §4.3 例外：人工 / 异常态不被收敛覆盖
    }
    return convergeResource(activeStatuses);
}

} // namespace rf
