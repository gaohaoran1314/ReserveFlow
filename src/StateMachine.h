// =============================================================================
// src/StateMachine.h  —  状态机内核（预约单表驱动 + 资源状态收敛函数）
// 对应 ARCH-P1 §3.4 / §4 / D-P1-1 / D-P1-2 / D-P1-9
//
// 两条不同的机制，务必区分：
//   1) 预约单（RESERVATION）：严格表驱动。静态规则表 + phase 门控，plan() 只校验不写库；
//   2) 资源（RESOURCE）：用收敛函数 convergeResource()，不是事件迁移表
//      （事件驱动会在两条预约交错时产生 PENDING_REVIEW → LOCKED 这类非法边）。
//
// 本类是纯函数集合：不持连接、不写库、不抛异常，可被任意层调用与单测。
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Result.h"
#include "Model.h"

namespace rf {

/** 业务对象类型，取值与 status_log.business_type 完全一致 */
enum class BizType { RESOURCE, RESERVATION };

std::string toStr(BizType b);                       // "RESOURCE" / "RESERVATION"

/** 迁移事件 */
enum class TransitEvent {
    CREATE,          // 下单创建（由资源 type 决定落到 LOCKED 还是 PENDING_REVIEW，D-P1-9）
    APPROVE,         // 管理员审核通过
    REJECT,          // 管理员驳回
    CANCEL,          // 取消（本人或管理员）
    START_USE,       // P2：取用
    REPORT_DUE,      // P2：到期未还
    SUBMIT_RETURN,   // P2：提交归还
    PASS_CHECK,      // P2：验收通过
    MARK_DAMAGE,     // P2：标记损坏
    HAND_MAINTAIN,   // P0 已有：手工置维护
    HAND_FREE,       // P0 已有：手工恢复可用
    TIMEOUT          // P2 新增：定时任务判超时（待审核超时 / 爽约）
};

/**
 * 通配前态常量（ARCH-P2 D-P2-3）：`from == kAnyFrom` 表示「任意当前状态」。
 * **仅允许 BizType::RESOURCE 使用**；RESERVATION 若出现通配规则，
 * `plan()` 直接返回 1008（防止有人把预约单写成「全状态可迁移」）。
 */
inline constexpr const char* kAnyFrom = "*";

std::string                 toStr(TransitEvent e);            // "CREATE" / "APPROVE" ...
std::optional<TransitEvent> transitEventFrom(const std::string& s);

/** 一条静态迁移规则 */
struct TransitionRule {
    BizType      biz;
    const char*  from;          ///< 当前状态字面量；"" 表示无前态（创建）
    TransitEvent event;
    const char*  to;            ///< 目标状态字面量
    int          phase;         ///< 1 = P1 启用；2 = P2 启用
    const char*  reason;        ///< 写 status_log.reason 的默认文案
};

/** 一次迁移请求（Service 组装，StateMachine 只校验不写库） */
struct TransitRequest {
    BizType                    biz = BizType::RESERVATION;
    std::string                fromStatus;      ///< 当前状态；创建时传 ""
    TransitEvent               event = TransitEvent::CREATE;
    int64_t                    operatorId = 0;
    std::string                reason;          ///< 覆盖默认文案；空则用规则默认
    std::optional<std::string> ruleCode;        ///< P1 恒为 nullopt（D-P1-10）
    // 仅供 CREATE 使用：资源类型决定免审核分流（D-P1-9）
    std::optional<model::ResourceType> resourceType;
};

/** 校验结论 */
struct TransitDecision {
    std::string toStatus;   ///< 目标状态字面量
    std::string reason;     ///< 实际写入 status_log 的 reason
};

class StateMachine {
public:
    /**
     * @param enabledPhase 当前启用的批次（P1 传 config.feature.phase = 1）
     */
    explicit StateMachine(int enabledPhase = 1);

    /**
     * 校验迁移并给出目标状态（纯函数，不写库、不抛异常）。
     * 失败码：
     *   3007 当前状态不允许该事件；
     *   3007 该迁移属于后续批次（规则存在但 phase > enabledPhase）；
     *   1000 事件非法（如 RESERVATION 的 CREATE 缺少 resourceType）。
     */
    Result<TransitDecision> plan(const TransitRequest& req) const;

    /** 该状态下是否允许某事件（供详情接口产出 allowedActions） */
    bool canTransit(BizType biz, const std::string& from, TransitEvent event) const;

    /** 列出该状态下全部可用事件（phase 门控后，按规则表顺序去重） */
    std::vector<TransitEvent> allowedEvents(BizType biz, const std::string& from) const;

    /**
     * 资源状态收敛（D-P1-2 / §4.3）：由「该资源当前有效预约集合」推导资源应有的粗粒度状态。
     * 优先级（phase=1）：LOCKED > PENDING_REVIEW > FREE
     * 优先级（phase=2）：IN_USE > RETURN_CHECK > DUE_RETURN > LOCKED > PENDING_REVIEW > FREE
     *
     * 说明：本重载**不感知**资源当前状态，故不会保护 MAINTENANCE/DISABLED/DAMAGE_CLAIM；
     *       生产代码请用下面的双参重载（它承担 §4.3「人工态不被覆盖」的职责）。
     *
     * @param activeStatuses 该资源下所有有效预约的 status 字面量（可为空）
     */
    model::ResourceState convergeResource(const std::vector<std::string>& activeStatuses) const;

    /**
     * 带「不回退」保护的收敛（§4.3 例外）：若 currentStatus 属于人工/异常态
     * （MAINTENANCE / DISABLED / DAMAGE_CLAIM），原样返回该状态，不让收敛覆盖。
     * @param currentStatus  资源当前 status 字面量（非法字面量按「不是人工态」处理）
     * @param activeStatuses 已剔除本次操作自身的有效预约 status 集合
     */
    model::ResourceState convergeResource(const std::string& currentStatus,
                                          const std::vector<std::string>& activeStatuses) const;

    /** 规则表只读访问（单测 / 文档生成用） */
    static const std::vector<TransitionRule>& rules();

    /**
     * 规则表自身的不变量校验（ARCH-P2 D-P2-3）：
     * **`BizType::RESERVATION` 不允许出现 `from == kAnyFrom` 的通配规则**。
     * plan() 每次调用都会先校验一遍（22 行比对，代价可忽略），
     * 一旦有人误加通配规则，全部预约单迁移立刻 1008，而不是悄悄变成「全状态可迁移」。
     * @return 表非法时返回 1008（kInternalError）+ 可读原因
     */
    static VoidResult checkRuleTable(const std::vector<TransitionRule>& tbl);

    int enabledPhase() const { return enabledPhase_; }

    /** 人工 / 异常状态集合：这些状态不被收敛函数覆盖（§4.3 例外） */
    static bool isManualState(model::ResourceState s);

private:
    int enabledPhase_;
};

} // namespace rf
