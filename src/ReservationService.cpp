// =============================================================================
// src/ReservationService.cpp  —  预约业务编排（下单 / 列表 / 详情 / 取消 / 审核 / 驳回）
// 对应 ARCH-P1 §1.2（下单 11 步）/ §1.3（事务边界）/ §4（迁移表）/ §5（锁时序）
//
// 三条不可违反的硬约束：
//   1) D-P1-4 加锁顺序：先 acquireResource（零连接占用）→ 再 DbPool::acquire → 再 Transaction。
//      反例（绝不这样写）：先取 Guard 再等锁 → 8 线程 × 8 连接互相饿死；
//   2) D-P1-3 提交前必须查 lock.lost()，为真则回滚返回 3014；
//   3) D-P1-2 资源态只用 convergeResource(current, actives) 双参版，
//      MAINTENANCE / DISABLED / DAMAGE_CLAIM 不被覆盖。
//
// 事务范围（§1.3）：查询在事务外，事务只包「写 reservation + 写 slot + 写 status_log + 收敛资源」。
// =============================================================================
#include "ReservationService.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "Base.h"
#include "SlotUtil.h"

namespace rf {

namespace {

/** 审计动作名（AUDIT 行的 action 字段，便于 grep） */
constexpr const char* kActCreate  = "RESERVATION_CREATE";
constexpr const char* kActCancel  = "RESERVATION_CANCEL";
constexpr const char* kActApprove = "RESERVATION_APPROVE";
constexpr const char* kActReject  = "RESERVATION_REJECT";

/** 单号序列：INCR + 首次 EXPIRE（ARCH-P1 §1.6 第 1 段，照抄勿改） */
constexpr const char* kSeqScript =
    "local v = redis.call('incr', KEYS[1]) "
    "if v == 1 then redis.call('expire', KEYS[1], ARGV[1]) end "
    "return v";

/** 单号序列 TTL：2 天（跨天后自动清理，避免 key 无限增长） */
constexpr const char* kSeqTtlSeconds = "172800";

/** 单号当日流水上限（4 位） */
constexpr int64_t kMaxSeq = 9999;

/** 备注长度上限（与 reservation.remark VARCHAR(255) 对齐） */
constexpr size_t kMaxRemarkLen = 255;

/** 金额保留两位小数（DECIMAL(10,2) 语义） */
double round2(double v) {
    return static_cast<double>(static_cast<int64_t>(v * 100.0 + (v >= 0 ? 0.5 : -0.5))) / 100.0;
}

/** "2026-02-10" → "20260210" */
std::string compactDate(const std::string& date) {
    std::string out;
    out.reserve(date.size());
    for (const char c : date) {
        if (c != '-') {
            out.push_back(c);
        }
    }
    return out;
}

/**
 * 从 start/end 重算槽区间（ARCH-P2 §5.4 注）。
 * 归还/取消后槽位可能已被删除，不能查表；SlotUtil 是纯函数，重算即可。
 */
bool slotRangeOf(const std::string& start, const std::string& end, std::string& date, int& from,
                 int& to) {
    rf::slot::DateTime sd;
    rf::slot::DateTime ed;
    if (!rf::slot::parseIso(start, sd) || !rf::slot::parseIso(end, ed)) {
        return false;
    }
    from = rf::slot::indexOf(sd);
    to   = rf::slot::indexOf(ed);
    if (from < 0 || to <= from) {
        return false;
    }
    char buf[16] = {0};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", sd.year, sd.month, sd.day);
    date = buf;
    return true;
}

/**
 * 给 FactBag 填「与场景无关的默认值」，保证 21 个白名单事实**全部存在**。
 * 这是 T07 的硬要求：reject 类规则只要引用了不存在的 fact 就会 fail-closed（3029），
 * 所以哪怕某个字段在当前阶段没意义，也要显式给默认值（否则会误拒所有请求）。
 */
void putBaseFacts(rf::FactBag& f, const std::string& nowIso) {
    f.setTime("now", nowIso);
    // 预约单未知时的默认值（CREATE 阶段单据还没建）
    f.set("reservation.id", rf::Fact::of(0.0));
    f.set("reservation.status", rf::Fact::of(std::string()));
    f.setTime("reservation.startTime", nowIso);
    f.setTime("reservation.endTime", nowIso);
    f.setTime("reservation.createdAt", nowIso);
    f.set("reservation.deductSeq", rf::Fact::of(0.0));
    f.set("reservation.overdueMinutes", rf::Fact::of(0.0));
    // 押金与损坏金额默认 0
    f.set("deposit.frozen", rf::Fact::of(0.0));
    f.set("deposit.deducted", rf::Fact::of(0.0));
    f.set("deposit.refunded", rf::Fact::of(0.0));
    f.set("damage.requestedAmount", rf::Fact::of(0.0));
    // 资源未知时的默认值
    f.set("resource.id", rf::Fact::of(0.0));
    f.set("resource.type", rf::Fact::of(std::string()));
    f.set("resource.status", rf::Fact::of(std::string()));
    f.set("resource.depositAmount", rf::Fact::of(0.0));
}

}  // namespace

// ============================================================================
// ReservationView JSON
// ============================================================================
nlohmann::json ReservationView::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["id"]            = id;
    j["reservationNo"] = reservationNo;
    j["userId"]        = userId;
    j["username"]      = username;
    j["resourceId"]    = resourceId;
    j["resourceCode"]  = resourceCode;
    j["resourceName"]  = resourceName;
    j["resourceType"]  = model::toStr(resourceType);
    j["startTime"]     = startTime;
    j["endTime"]       = endTime;
    j["status"]        = status;
    j["statusLabel"]   = statusLabel;
    j["depositAmount"] = round2(depositAmount);
    j["remark"]        = remark;
    j["needReview"]    = needReview;
    j["slotDate"]      = slotDate;
    nlohmann::json idx = nlohmann::json::array();
    for (const int i : slotIndexes) {
        idx.push_back(i);
    }
    j["slotIndexes"] = std::move(idx);
    nlohmann::json acts = nlohmann::json::array();
    for (const std::string& a : allowedActions) {
        acts.push_back(a);
    }
    j["allowedActions"] = std::move(acts);
    j["createdAt"]      = createdAt;
    j["updatedAt"]      = updatedAt;
    // P2：仅在对应动作里填充过才输出，P1 的响应形状保持一字不变
    if (!deposit.is_null()) {
        j["deposit"] = deposit;
    }
    if (!resourceStatus.empty()) {
        j["resourceStatus"] = resourceStatus;
    }
    if (offersDispatched >= 0) {
        j["offersDispatched"] = offersDispatched;
    }
    if (!violation.is_null()) {
        j["violation"] = violation;
    }
    if (creditScoreAfter >= 0) {
        j["creditScoreAfter"] = creditScoreAfter;
    }
    return j;
}

nlohmann::json ReservationView::toListJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["id"]            = id;
    j["reservationNo"] = reservationNo;
    j["userId"]        = userId;
    j["username"]      = username;
    j["resourceId"]    = resourceId;
    j["resourceCode"]  = resourceCode;
    j["resourceName"]  = resourceName;
    j["resourceType"]  = model::toStr(resourceType);
    j["startTime"]     = startTime;
    j["endTime"]       = endTime;
    j["status"]        = status;
    j["statusLabel"]   = statusLabel;
    j["needReview"]    = needReview;
    j["createdAt"]     = createdAt;
    // P3 T10：列表也要带资源态与押金（只在有值时才输出，避免详情/列表形状不一致的困惑）
    if (!resourceStatus.empty()) {
        j["resourceStatus"] = resourceStatus;
    }
    if (!deposit.is_null()) {
        j["deposit"] = deposit;
    }
    return j;
}

// ============================================================================
// ReservationService
// ============================================================================
ReservationService::ReservationService(DbPool& pool, RedisClient& redis,
                                       repo::ReservationRepo& rrepo, repo::ResourceRepo& rr,
                                       repo::UserRepo& ur, repo::AuditRepo& ar, StateMachine& sm,
                                       DistributedLock& lock, SettlementService& settlement,
                                       WaitlistService& waitlist, RuleEngine& engine,
                                       const PickupConfig& pickupCfg,
                                       const MaintenanceConfig& maintCfg, int lockWaitMs,
                                       int lockTtlMs, int minCreditScore)
    : pool_(pool),
      redis_(redis),
      rrepo_(rrepo),
      rr_(rr),
      ur_(ur),
      ar_(ar),
      sm_(sm),
      lock_(lock),
      settlement_(settlement),
      waitlist_(waitlist),
      engine_(engine),
      pickupCfg_(pickupCfg),
      maintCfg_(maintCfg),
      lockWaitMs_(lockWaitMs > 0 ? lockWaitMs : 3000),
      lockTtlMs_(lockTtlMs > 0 ? lockTtlMs : 30000),
      minCreditScore_(minCreditScore < 0 ? 0 : minCreditScore) {}

Result<std::optional<model::Reservation>> ReservationService::loadReservation(int64_t id) {
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<std::optional<model::Reservation>>::fail(
            kDbError, "数据库访问失败：获取数据库连接超时");
    }
    return rrepo_.findById(*guard, id);
}

Result<std::string> ReservationService::generateReservationNo(RedisClient& redis,
                                                            const std::string& yyyymmdd) {
    const std::string key = "rf:seq:rv:" + yyyymmdd;
    auto r = redis.eval(kSeqScript, {key}, {kSeqTtlSeconds});
    if (!r.ok()) {
        // D-P1-11：Redis 不可用一律拒单，绝不退化成 MAX(reservation_no)+1（并发必重号）
        return Result<std::string>::fail(kCacheError,
                                         "缓存访问失败，暂时无法生成预约单号：" + r.msg);
    }
    if (r.data <= 0 || r.data > kMaxSeq) {
        return Result<std::string>::fail(kReservationNoExhausted, "当日预约号已用尽，请明天再试");
    }
    char buf[64] = {0};
    std::snprintf(buf, sizeof(buf), "RV%s%04lld", yyyymmdd.c_str(),
                  static_cast<long long>(r.data));
    return Result<std::string>::make(std::string(buf));
}

Result<std::string> ReservationService::nextReservationNo(const std::string& yyyymmdd) {
    return generateReservationNo(redis_, yyyymmdd);
}

Result<Empty> ReservationService::writeStatusLog(DbConn& conn, BizType biz, int64_t businessId,
                                                 const std::string& from, const std::string& to,
                                                 const AuthContext& ctx, const std::string& reason,
                                                 const std::string& ruleCode) {
    repo::StatusLogEntry e;
    e.businessType = toStr(biz);
    e.businessId   = businessId;
    e.fromStatus   = from;
    e.toStatus     = to;
    e.operatorId   = ctx.userId;
    e.reason       = reason;
    // P3：规则触发的迁移写规则码（如 RESERVE_DAMAGE_PENALTY），其余仍为 NULL
    e.ruleCode     = ruleCode.empty() ? std::nullopt : std::optional<std::string>(ruleCode);

    auto r = ar_.insertStatusLog(conn, e);
    if (!r.ok()) {
        return Result<Empty>::fail(r.code, r.msg);
    }
    return Result<Empty>::make(Empty{});
}

Result<Empty> ReservationService::convergeResource(DbConn& conn, int64_t resourceId,
                                                   int64_t excludeReservationId,
                                                   const AuthContext& ctx,
                                                   const std::string& why) {
    auto actives = rrepo_.listActiveStatuses(conn, resourceId, excludeReservationId);
    if (!actives.ok()) {
        return Result<Empty>::fail(actives.code, actives.msg);
    }
    auto cur = rr_.findById(conn, resourceId);
    if (!cur.ok()) {
        return Result<Empty>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<Empty>::make(Empty{});   // 资源已逻辑删除：不收敛
    }

    const std::string from = model::toStr(cur.data->status);
    // 双参版：人工态（MAINTENANCE / DISABLED / DAMAGE_CLAIM）不被覆盖（§4.3 例外）
    const std::string to = model::toStr(sm_.convergeResource(from, actives.data));
    if (from == to) {
        return Result<Empty>::make(Empty{});   // 无真实迁移：不写库、不留痕
    }

    auto upd = rr_.updateStatus(conn, resourceId, model::resourceStateFrom(to).value_or(
                                                    model::ResourceState::FREE));
    if (!upd.ok()) {
        return Result<Empty>::fail(upd.code, upd.msg);
    }
    return writeStatusLog(conn, BizType::RESOURCE, resourceId, from, to, ctx, why);
}

Result<ReservationView> ReservationService::buildView(DbConn& conn, int64_t id) {
    auto v = rrepo_.findView(conn, id);
    if (!v.ok()) {
        return Result<ReservationView>::fail(v.code, v.msg);
    }
    if (!v.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    ReservationView out;
    out.id            = v.data->id;
    out.reservationNo = v.data->reservationNo;
    out.userId        = v.data->userId;
    out.username      = v.data->username;
    out.resourceId    = v.data->resourceId;
    out.resourceCode  = v.data->resourceCode;
    out.resourceName  = v.data->resourceName;
    out.resourceType  = v.data->resourceType;
    out.startTime     = v.data->startTime;
    out.endTime       = v.data->endTime;
    out.status        = v.data->status;
    out.statusLabel   = "未知";
    if (const auto st = model::reservationStatusFrom(v.data->status); st.has_value()) {
        out.statusLabel = model::labelOf(*st);
    }
    out.depositAmount = v.data->depositAmount;
    out.remark        = v.data->remark;
    out.createdAt     = v.data->createdAt;
    out.updatedAt     = v.data->updatedAt;
    out.needReview    = (v.data->resourceType == model::ResourceType::LAB_EQUIPMENT);
    out.resourceStatus = v.data->resourceStatus;   // P3 T10：详情也带资源态

    // P3 T10：详情带押金账目（零押金单据 state=NONE，同样输出，形状稳定）
    {
        auto acc = settlement_.account(conn, id);
        if (acc.ok()) {
            out.deposit = acc.data.toJson();
        }
    }

    auto slots = rrepo_.listSlotIndexes(conn, id);
    if (!slots.ok()) {
        return Result<ReservationView>::fail(slots.code, slots.msg);
    }
    out.slotIndexes = slots.data;

    auto date = rrepo_.slotDateOf(conn, id);
    if (!date.ok()) {
        return Result<ReservationView>::fail(date.code, date.msg);
    }
    out.slotDate = date.data;

    for (const TransitEvent ev : sm_.allowedEvents(BizType::RESERVATION, out.status)) {
        // allowedActions 是「调用方可执行的动作」：定时任务专属事件（TIMEOUT / REPORT_DUE）
        // 不该出现在这里，否则前端会给用户显示一个点了必然 3007 的按钮。
        if (ev == TransitEvent::TIMEOUT || ev == TransitEvent::REPORT_DUE) {
            continue;
        }
        out.allowedActions.push_back(toStr(ev));
    }
    return Result<ReservationView>::make(std::move(out));
}

// ------------------------------------------------------------------ 下单
Result<ReservationView> ReservationService::create(const CreateReservationReq& req,
                                                   const AuthContext& ctx) {
    const std::string rid = requestId();

    // ---- 1) 入参兜底（Controller 已校验，Service 不信任上游）
    if (req.resourceId <= 0) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kInvalidParam, 0);
        return Result<ReservationView>::fail(kInvalidParam,
                                             "参数校验失败：resourceId 必须是正整数");
    }
    if (req.remark.size() > kMaxRemarkLen) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kInvalidParam,
                  req.resourceId);
        return Result<ReservationView>::fail(kInvalidParam, "参数校验失败：remark 长度上限 255");
    }

    // ---- 2) 时间校验（纯计算，无锁无连接）
    const rf::slot::SpanResult span =
        rf::slot::makeSpan(req.startTime, req.endTime, TimeUtil::nowString());
    if (!span.ok) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kReservationTimeInvalid,
                  req.resourceId);
        return Result<ReservationView>::fail(kReservationTimeInvalid, span.err);
    }

    // ---- 3) 抢资源锁：此阶段**零 DB 连接占用**（D-P1-4）
    auto lockRes = lock_.acquireResource(req.resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        LOG_WARN("rid={} 下单抢锁失败 resource={} code={}", rid, req.resourceId, lockRes.code);
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, lockRes.code,
                  req.resourceId);
        return Result<ReservationView>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);   // RAII：任何退出路径都会 detach → unlock

    // ---- 4) 取连接（先锁后连接）
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kDbError, req.resourceId);
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    // ---- 5) 账号校验：存在性 / 黑名单 / 信用分
    auto user = ur_.findById(conn, ctx.userId);
    if (!user.ok()) {
        return Result<ReservationView>::fail(user.code, user.msg);
    }
    if (!user.data.has_value()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kInvalidCredential, 0);
        return Result<ReservationView>::fail(kInvalidCredential, "账号不存在或已被删除");
    }
    if (user.data->status == model::UserStatus::DISABLED) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kAccountDisabled,
                  req.resourceId);
        return Result<ReservationView>::fail(kAccountDisabled, "账号已被禁用，无法使用预约功能");
    }
    // 说明（P3 T07）：黑名单与信用分判定移到「资源校验之后」——
    // 因为规则求值需要 $resource.* 完整事实（#4 押金比例按资源类型分流），
    // 而 reject 类规则 fail-closed 要求 facts 齐全。见下面 6b/6c。

    // ---- 6) 资源校验：存在且未删 / 状态可预约
    auto res = rr_.findById(conn, req.resourceId);
    if (!res.ok()) {
        return Result<ReservationView>::fail(res.code, res.msg);
    }
    if (!res.data.has_value()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kResourceNotFound,
                  req.resourceId);
        return Result<ReservationView>::fail(kResourceNotFound, "资源不存在");
    }
    // D-P2-10：不可预约集合扩到 5 态（IN_USE / DUE_RETURN 仍可预约其他时段）
    if (model::isUnbookableState(res.data->status)) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kResourceNotBookable,
                  req.resourceId);
        return Result<ReservationView>::fail(
            kResourceNotBookable,
            "资源当前不可预约（" + std::string(model::labelOf(res.data->status)) + "）");
    }

    // ---- 6b) 规则求值（CREATE 阶段）：黑名单拦截 / 同时在约上限 / 押金冻结比例
    //      facts 必须**给全 21 项**：reject 类规则引用到不存在的 fact 会 fail-closed 3029，
    //      那会把所有正常用户都拒掉（T07 的坑，已写在 ARCH-P3 §5.3）。
    const std::string nowIso = TimeUtil::nowString();
    FactBag           facts;
    putBaseFacts(facts, nowIso);
    facts.set("user.id", Fact::of(static_cast<double>(user.data->id)));
    facts.set("user.status", Fact::of(model::toStr(user.data->status)));
    facts.set("user.role", Fact::of(model::toStr(user.data->role)));
    facts.set("user.creditScore", Fact::of(static_cast<double>(user.data->creditScore)));
    {
        // 哪怕 #3 停用也要算：规则可能被管理员在运行期开启（§5.3）
        auto activeCnt = rrepo_.countActiveByUser(conn, ctx.userId);
        facts.set("user.activeReservations",
                  Fact::of(static_cast<double>(activeCnt.ok() ? activeCnt.data : 0)));
    }
    facts.set("resource.id", Fact::of(static_cast<double>(res.data->id)));
    facts.set("resource.type", Fact::of(model::toStr(res.data->type)));
    facts.set("resource.status", Fact::of(model::toStr(res.data->status)));
    facts.set("resource.depositAmount", Fact::of(res.data->depositAmount));
    facts.setTime("reservation.startTime", req.startTime);
    facts.setTime("reservation.endTime", req.endTime);
    facts.setTime("reservation.createdAt", nowIso);

    EvalResult evalRes;
    bool       engineActive = engine_.enabled();
    if (engineActive) {
        evalRes = engine_.evaluate(EvalPhase::CREATE, facts);
        if (evalRes.degraded) {
            engineActive = false;   // 整表不可读 → 走 P2 兜底
        }
    }
    if (engineActive && evalRes.rejected) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, evalRes.rejectCode,
                  req.resourceId);
        return Result<ReservationView>::fail(evalRes.rejectCode, evalRes.rejectMsg);
    }
    if (!engineActive) {
        // P2 硬编码兜底（D-P3-6 安全阀 / 引擎降级）：**保留，不可删除**
        if (user.data->status == model::UserStatus::BLACKLIST) {
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kUserBlacklisted,
                      req.resourceId);
            return Result<ReservationView>::fail(kUserBlacklisted, "账号处于黑名单，无法预约");
        }
    }

    // ---- 6c) 信用分门槛（P1 的硬编码校验；P3 没有对应规则，阈值仍取自 config）
    if (minCreditScore_ > 0 && user.data->creditScore < minCreditScore_) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kCreditTooLow,
                  req.resourceId);
        return Result<ReservationView>::fail(
            kCreditTooLow, "信用分不足，无法预约（当前 " + std::to_string(user.data->creditScore) +
                               " 分，要求不低于 " + std::to_string(minCreditScore_) + " 分）");
    }

    // 押金冻结比例：命中 #4 取其 rate；未命中/引擎关闭 → 1.0（等价 P2 全额冻结）
    double freezeRate = 1.0;
    if (engineActive) {
        if (const ResolvedAction* act = evalRes.findAction(ActionType::FreezeDeposit);
            act != nullptr) {
            freezeRate = act->rate;
        }
    }

    // ---- 7) 时段预检（第 2 层防线：给出友好错误，避免无谓的插入失败）
    auto taken = rrepo_.occupiedIndexes(conn, req.resourceId, span.date, span.indexes);
    if (!taken.ok()) {
        return Result<ReservationView>::fail(taken.code, taken.msg);
    }
    if (!taken.data.empty()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kSlotUnavailable,
                  req.resourceId);
        return Result<ReservationView>::fail(kSlotUnavailable, "所选时段已被占用");
    }

    // ---- 8) 单号（Lua INCR；Redis 不可用即拒单，D-P1-11）
    auto no = nextReservationNo(compactDate(span.date));
    if (!no.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, no.code, req.resourceId);
        return Result<ReservationView>::fail(no.code, no.msg);
    }

    // ---- 9) 状态机决定初始状态（免审核分流，D-P1-9）
    TransitRequest tq;
    tq.biz          = BizType::RESERVATION;
    tq.fromStatus   = "";
    tq.event        = TransitEvent::CREATE;
    tq.operatorId   = ctx.userId;
    tq.resourceType = res.data->type;
    auto decision   = sm_.plan(tq);
    if (!decision.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, decision.code,
                  req.resourceId);
        return Result<ReservationView>::fail(decision.code, decision.msg);
    }

    model::Reservation rec;
    rec.reservationNo = no.data;
    rec.userId        = ctx.userId;
    rec.resourceId    = req.resourceId;
    rec.startTime     = req.startTime;
    rec.endTime       = req.endTime;
    rec.status        = model::reservationStatusFrom(decision.data.toStatus)
                     .value_or(model::ReservationStatus::PENDING_REVIEW);
    rec.depositAmount = res.data->depositAmount;
    rec.remark        = req.remark;

    // ---- 10) 事务：主表 → 槽位 → 留痕 → 资源收敛 → 查 lost → commit
    {
        Transaction tx(conn);
        if (!tx.active()) {
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kDbError,
                      req.resourceId);
            return Result<ReservationView>::fail(kDbError, "数据库访问失败：事务开启失败");
        }

        auto ins = rrepo_.insert(conn, rec);
        if (!ins.ok()) {
            tx.rollback();
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, ins.code, req.resourceId);
            return Result<ReservationView>::fail(ins.code, ins.msg);
        }
        const int64_t newId = ins.data;

        auto slots = rrepo_.insertSlots(conn, newId, req.resourceId, span.date, span.indexes);
        if (!slots.ok()) {
            tx.rollback();   // 3005：唯一索引兜底命中（D-P1-3）
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, slots.code, newId);
            return Result<ReservationView>::fail(slots.code, slots.msg);
        }

        // D-P2-12：押金 > 0 才写 FREEZE 流水；会议室 0 押金一条都不写。
        // 冻结额 = 资源押金 × 规则给出的比例（#4 默认 rate=1.0，等价 P2）。
        auto frozen = settlement_.freeze(conn, newId, ctx.userId,
                                         res.data->depositAmount * freezeRate);
        if (!frozen.ok()) {
            tx.rollback();
            return Result<ReservationView>::fail(frozen.code, frozen.msg);
        }

        auto logRes = writeStatusLog(conn, BizType::RESERVATION, newId, "", decision.data.toStatus,
                                     ctx, decision.data.reason);
        if (!logRes.ok()) {
            tx.rollback();
            return Result<ReservationView>::fail(logRes.code, logRes.msg);
        }

        auto conv = convergeResource(conn, req.resourceId, 0, ctx, "预约生效，资源状态收敛");
        if (!conv.ok()) {
            tx.rollback();
            return Result<ReservationView>::fail(conv.code, conv.msg);
        }

        // D-P1-3：提交前必须确认锁还在自己手里
        if (lock.lost()) {
            tx.rollback();
            LOG_WARN("rid={} 提交前发现资源锁已丢失 resource={}", rid, req.resourceId);
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kLockLost, newId);
            return Result<ReservationView>::fail(kLockLost, "资源锁已失效，请重试");
        }

        auto committed = tx.commit();
        if (!committed.ok()) {
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, committed.code, newId);
            return Result<ReservationView>::fail(committed.code, committed.msg);
        }

        auto view = buildView(conn, newId);
        if (!view.ok()) {
            return Result<ReservationView>::fail(view.code, view.msg);
        }
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCreate, kOk, newId);
        return view;
    }
}

// ------------------------------------------------------------------ 列表
Result<repo::ReservationQuery> ReservationService::normalizeQuery(const repo::ReservationQuery& q,
                                                                 const AuthContext& ctx) const {
    repo::ReservationQuery query = q;
    // D-8：分页越界在此收敛
    query.page = (query.page < 1) ? 1 : (query.page > 100000 ? 100000 : query.page);
    query.size = (query.size < 1) ? 10 : (query.size > 100 ? 100 : query.size);

    // 普通用户只能看自己；管理员可看全部，也可显式按 userId 过滤
    if (ctx.role != model::Role::ADMIN) {
        query.userId = ctx.userId;
    }
    if (query.status.has_value()) {
        if (!model::reservationStatusFrom(*query.status).has_value()) {
            return Result<repo::ReservationQuery>::fail(
                kInvalidParam, "参数校验失败：status 取值非法（" + *query.status + "）");
        }
    }
    if (query.resourceId.has_value() && *query.resourceId <= 0) {
        return Result<repo::ReservationQuery>::fail(kInvalidParam,
                                                    "参数校验失败：resourceId 必须是正整数");
    }
    return Result<repo::ReservationQuery>::make(std::move(query));
}

ReservationView ReservationService::viewOfRow(const repo::ReservationRow& row) {
    ReservationView v;
    v.id            = row.id;
    v.reservationNo = row.reservationNo;
    v.userId        = row.userId;
    v.username      = row.username;
    v.resourceId    = row.resourceId;
    v.resourceCode  = row.resourceCode;
    v.resourceName  = row.resourceName;
    v.resourceType  = row.resourceType;
    v.startTime     = row.startTime;
    v.endTime       = row.endTime;
    v.status        = row.status;
    v.statusLabel   = "未知";
    if (const auto st = model::reservationStatusFrom(row.status); st.has_value()) {
        v.statusLabel = model::labelOf(*st);
    }
    v.depositAmount  = row.depositAmount;
    v.remark         = row.remark;
    v.createdAt      = row.createdAt;
    v.updatedAt      = row.updatedAt;
    v.needReview     = (row.resourceType == model::ResourceType::LAB_EQUIPMENT);
    v.resourceStatus = row.resourceStatus;
    return v;
}

Result<repo::ReservationPage> ReservationService::list(const repo::ReservationQuery& q,
                                                       const AuthContext& ctx) {
    auto query = normalizeQuery(q, ctx);
    if (!query.ok()) {
        return Result<repo::ReservationPage>::fail(query.code, query.msg);
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<repo::ReservationPage>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    return rrepo_.search(*guard, query.data);
}

Result<nlohmann::json> ReservationService::listJson(const repo::ReservationQuery& q,
                                                    const AuthContext& ctx) {
    auto query = normalizeQuery(q, ctx);
    if (!query.ok()) {
        return Result<nlohmann::json>::fail(query.code, query.msg);
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<nlohmann::json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto page = rrepo_.search(conn, query.data);
    if (!page.ok()) {
        return Result<nlohmann::json>::fail(page.code, page.msg);
    }

    // 整页单据的押金聚合：**一次** GROUP BY，绝不在循环里查（T10 判据）
    std::vector<int64_t> ids;
    ids.reserve(page.data.list.size());
    for (const auto& row : page.data.list) {
        ids.push_back(row.id);
    }
    auto dep = rrepo_.depositSummaries(conn, ids);
    if (!dep.ok()) {
        return Result<nlohmann::json>::fail(dep.code, dep.msg);
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto& row : page.data.list) {
        ReservationView v = viewOfRow(row);
        const auto      it = dep.data.find(row.id);
        if (it != dep.data.end()) {
            DepositAccount acc;
            acc.frozen   = it->second.frozen;
            acc.deducted = it->second.deducted;
            acc.refunded = it->second.refunded;
            v.deposit    = acc.toJson();
        } else {
            // 零押金单据没有流水，但也要给稳定的形状（state=NONE）
            DepositAccount acc;
            v.deposit = acc.toJson();
        }
        list.push_back(v.toListJson());
    }

    nlohmann::json out = nlohmann::json::object();
    out["list"]  = std::move(list);
    out["total"] = page.data.total;
    out["page"]  = page.data.page;
    out["size"]  = page.data.size;
    return Result<nlohmann::json>::make(std::move(out));
}

// ------------------------------------------------------------------ 详情
Result<ReservationView> ReservationService::detail(int64_t id, const AuthContext& ctx) {
    if (id <= 0) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;
    auto view = buildView(conn, id);
    if (!view.ok()) {
        return view;
    }
    if (ctx.role != model::Role::ADMIN && view.data.userId != ctx.userId) {
        return Result<ReservationView>::fail(kReservationForbidden, "无权操作该预约");
    }
    return view;
}

// ------------------------------------------------------------------ 取消
Result<Empty> ReservationService::cancel(int64_t id, const std::string& reason,
                                         const AuthContext& ctx) {
    const std::string rid = requestId();
    if (id <= 0) {
        return Result<Empty>::fail(kReservationNotFound, "预约不存在");
    }

    // ① 短读定位资源（读完立即归还连接，等锁期间零连接占用）
    auto pre = loadReservation(id);
    if (!pre.ok()) {
        return Result<Empty>::fail(pre.code, pre.msg);
    }
    if (!pre.data.has_value()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCancel, kReservationNotFound, id);
        return Result<Empty>::fail(kReservationNotFound, "预约不存在");
    }
    if (ctx.role != model::Role::ADMIN && pre.data->userId != ctx.userId) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCancel, kReservationForbidden, id);
        return Result<Empty>::fail(kReservationForbidden, "无权操作该预约");
    }
    const int64_t resourceId = pre.data->resourceId;

    // ② 抢资源锁（零连接占用）
    auto lockRes = lock_.acquireResource(resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCancel, lockRes.code, id);
        return Result<Empty>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    // ③ 取连接 + 锁内重读真相
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Empty>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<Empty>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<Empty>::fail(kReservationNotFound, "预约不存在");
    }
    if (ctx.role != model::Role::ADMIN && cur.data->userId != ctx.userId) {
        return Result<Empty>::fail(kReservationForbidden, "无权操作该预约");
    }

    // 业务口径（定稿）：已锁定单只有 now < start_time 才允许取消
    if (cur.data->status == model::ReservationStatus::LOCKED) {
        std::time_t startEpoch = 0;
        if (TimeUtil::parseIso(cur.data->startTime, startEpoch) &&
            TimeUtil::nowEpoch() >= startEpoch) {
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCancel,
                      kReservationStateInvalid, id);
            return Result<Empty>::fail(kReservationStateInvalid,
                                       "当前状态不允许该操作：预约已开始，不能取消");
        }
    }

    // 状态机校验（PENDING_REVIEW / LOCKED → CANCELLED 才合法，其余 3007）
    TransitRequest tq;
    tq.biz        = BizType::RESERVATION;
    tq.fromStatus = model::toStr(cur.data->status);
    tq.event      = TransitEvent::CANCEL;
    tq.operatorId = ctx.userId;
    tq.reason     = reason;
    auto decision = sm_.plan(tq);
    if (!decision.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCancel, decision.code, id);
        return Result<Empty>::fail(decision.code, decision.msg);
    }

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<Empty>::fail(kDbError, "数据库访问失败：事务开启失败");
    }

    // D-P2-5：CAS 裁决，affectedRows==0 即「别人已处理」→ 跳过全部副作用
    auto cas = rrepo_.casUpdateStatus(conn, id, model::toStr(cur.data->status),
                                     decision.data.toStatus);
    if (!cas.ok()) {
        tx.rollback();
        return Result<Empty>::fail(cas.code, cas.msg);
    }
    if (cas.data == 0) {
        tx.rollback();
        return Result<Empty>::fail(kReservationStateInvalid,
                                   "预约状态已被其他操作改变，请刷新后重试");
    }
    auto del = rrepo_.deleteSlots(conn, id);
    if (!del.ok()) {
        tx.rollback();
        return Result<Empty>::fail(del.code, del.msg);
    }
    auto logRes = writeStatusLog(conn, BizType::RESERVATION, id,
                                 model::toStr(cur.data->status), decision.data.toStatus, ctx,
                                 decision.data.reason);
    if (!logRes.ok()) {
        tx.rollback();
        return Result<Empty>::fail(logRes.code, logRes.msg);
    }
    auto conv = convergeResource(conn, resourceId, id, ctx, "预约取消，资源状态收敛");
    if (!conv.ok()) {
        tx.rollback();
        return Result<Empty>::fail(conv.code, conv.msg);
    }
    // §5.4：取消释放的时段立刻派发候补（槽已删，用 start/end 重算区间）
    int offersDispatched = 0;
    {
        std::string d;
        int from = 0;
        int to   = 0;
        if (slotRangeOf(cur.data->startTime, cur.data->endTime, d, from, to)) {
            auto disp = waitlist_.dispatchOffers(conn, resourceId, d, from, to);
            if (!disp.ok()) {
                tx.rollback();
                return Result<Empty>::fail(disp.code, disp.msg);
            }
            offersDispatched = disp.data;
        }
    }
    if (lock.lost()) {
        tx.rollback();
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCancel, kLockLost, id);
        return Result<Empty>::fail(kLockLost, "资源锁已失效，请重试");
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<Empty>::fail(committed.code, committed.msg);
    }

    LOG_INFO("rid={} 预约已取消 id={} 释放槽位 {} 个 派发候补 {} 个", rid, id, del.data,
             offersDispatched);
    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActCancel, kOk, id);
    return Result<Empty>::make(Empty{});
}

// ------------------------------------------------------------------ 审核通过
Result<ReservationView> ReservationService::approve(int64_t id, const std::string& remark,
                                                    const AuthContext& ctx) {
    const std::string rid = requestId();
    if (ctx.role != model::Role::ADMIN) {
        return Result<ReservationView>::fail(kForbidden, "权限不足，需要管理员角色");
    }
    if (id <= 0) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    auto pre = loadReservation(id);
    if (!pre.ok()) {
        return Result<ReservationView>::fail(pre.code, pre.msg);
    }
    if (!pre.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    auto lockRes = lock_.acquireResource(pre.data->resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        return Result<ReservationView>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<ReservationView>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    TransitRequest tq;
    tq.biz        = BizType::RESERVATION;
    tq.fromStatus = model::toStr(cur.data->status);
    tq.event      = TransitEvent::APPROVE;
    tq.operatorId = ctx.userId;
    tq.reason     = remark;
    auto decision = sm_.plan(tq);
    if (!decision.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActApprove, decision.code, id);
        return Result<ReservationView>::fail(decision.code, decision.msg);
    }

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：事务开启失败");
    }

    auto casA = rrepo_.casUpdateStatus(conn, id, model::toStr(cur.data->status),
                                      decision.data.toStatus);
    if (!casA.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(casA.code, casA.msg);
    }
    if (casA.data == 0) {
        tx.rollback();
        return Result<ReservationView>::fail(kReservationStateInvalid,
                                             "预约状态已被其他操作改变，请刷新后重试");
    }
    auto logRes = writeStatusLog(conn, BizType::RESERVATION, id,
                                 model::toStr(cur.data->status), decision.data.toStatus, ctx,
                                 decision.data.reason);
    if (!logRes.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(logRes.code, logRes.msg);
    }
    // 审核通过不改变占用集合，但资源态可能与历史不一致（例如曾被人工置维护），收敛一次
    auto conv = convergeResource(conn, cur.data->resourceId, 0, ctx, "审核通过，资源状态收敛");
    if (!conv.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(conv.code, conv.msg);
    }
    if (lock.lost()) {
        tx.rollback();
        return Result<ReservationView>::fail(kLockLost, "资源锁已失效，请重试");
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<ReservationView>::fail(committed.code, committed.msg);
    }

    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActApprove, kOk, id);
    return buildView(conn, id);
}

// ------------------------------------------------------------------ 驳回
Result<ReservationView> ReservationService::reject(int64_t id, const std::string& reason,
                                                   const AuthContext& ctx) {
    const std::string rid = requestId();
    if (ctx.role != model::Role::ADMIN) {
        return Result<ReservationView>::fail(kForbidden, "权限不足，需要管理员角色");
    }
    if (id <= 0) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    auto pre = loadReservation(id);
    if (!pre.ok()) {
        return Result<ReservationView>::fail(pre.code, pre.msg);
    }
    if (!pre.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    auto lockRes = lock_.acquireResource(pre.data->resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        return Result<ReservationView>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<ReservationView>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    TransitRequest tq;
    tq.biz        = BizType::RESERVATION;
    tq.fromStatus = model::toStr(cur.data->status);
    tq.event      = TransitEvent::REJECT;
    tq.operatorId = ctx.userId;
    tq.reason     = reason;
    auto decision = sm_.plan(tq);
    if (!decision.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActReject, decision.code, id);
        return Result<ReservationView>::fail(decision.code, decision.msg);
    }

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：事务开启失败");
    }

    auto casR = rrepo_.casUpdateStatus(conn, id, model::toStr(cur.data->status),
                                      decision.data.toStatus);
    if (!casR.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(casR.code, casR.msg);
    }
    if (casR.data == 0) {
        tx.rollback();
        return Result<ReservationView>::fail(kReservationStateInvalid,
                                             "预约状态已被其他操作改变，请刷新后重试");
    }
    auto del = rrepo_.deleteSlots(conn, id);
    if (!del.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(del.code, del.msg);
    }
    auto logRes = writeStatusLog(conn, BizType::RESERVATION, id,
                                 model::toStr(cur.data->status), decision.data.toStatus, ctx,
                                 decision.data.reason);
    if (!logRes.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(logRes.code, logRes.msg);
    }
    auto conv = convergeResource(conn, cur.data->resourceId, id, ctx, "预约驳回，资源状态收敛");
    if (!conv.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(conv.code, conv.msg);
    }
    // §5.4：驳斥释放的时段派发候补
    {
        std::string d;
        int from = 0;
        int to   = 0;
        if (slotRangeOf(cur.data->startTime, cur.data->endTime, d, from, to)) {
            auto disp = waitlist_.dispatchOffers(conn, cur.data->resourceId, d, from, to);
            if (!disp.ok()) {
                tx.rollback();
                return Result<ReservationView>::fail(disp.code, disp.msg);
            }
        }
    }
    if (lock.lost()) {
        tx.rollback();
        return Result<ReservationView>::fail(kLockLost, "资源锁已失效，请重试");
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<ReservationView>::fail(committed.code, committed.msg);
    }

    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActReject, kOk, id);
    return buildView(conn, id);
}

// ------------------------------------------------------------------ 占用查询
Result<nlohmann::json> ReservationService::occupiedSlots(int64_t resourceId,
                                                         const std::string& date,
                                                         const AuthContext& /*ctx*/) {
    if (resourceId <= 0) {
        return Result<nlohmann::json>::fail(kInvalidParam,
                                            "参数校验失败：资源 id 必须是正整数");
    }
    rf::slot::DateTime d;
    if (!rf::slot::parseDate(date, d)) {
        return Result<nlohmann::json>::fail(kInvalidParam,
                                            "参数校验失败：date 格式应为 yyyy-MM-dd");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<nlohmann::json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto res = rr_.findById(conn, resourceId);
    if (!res.ok()) {
        return Result<nlohmann::json>::fail(res.code, res.msg);
    }
    if (!res.data.has_value()) {
        return Result<nlohmann::json>::fail(kResourceNotFound, "资源不存在");
    }

    // 候选 = 全天 48 槽；occupiedIndexes 内部走 uk_slot_resource_date_index 最左前缀
    std::vector<int> all;
    all.reserve(static_cast<size_t>(rf::slot::kSlotsPerDay));
    for (int i = 0; i < rf::slot::kSlotsPerDay; ++i) {
        all.push_back(i);
    }
    auto taken = rrepo_.occupiedIndexes(conn, resourceId, date, all);
    if (!taken.ok()) {
        return Result<nlohmann::json>::fail(taken.code, taken.msg);
    }

    std::vector<bool> isTaken(static_cast<size_t>(rf::slot::kSlotsPerDay), false);
    for (const int i : taken.data) {
        if (i >= 0 && i < rf::slot::kSlotsPerDay) {
            isTaken[static_cast<size_t>(i)] = true;
        }
    }

    nlohmann::json occupied   = nlohmann::json::array();
    nlohmann::json available  = nlohmann::json::array();
    nlohmann::json labels     = nlohmann::json::array();
    for (int i = 0; i < rf::slot::kSlotsPerDay; ++i) {
        if (isTaken[static_cast<size_t>(i)]) {
            occupied.push_back(i);
            labels.push_back(rf::slot::slotLabel(i) + "-" + rf::slot::slotLabel(i + 1));
        } else {
            available.push_back(i);
        }
    }

    nlohmann::json out = nlohmann::json::object();
    out["resourceId"]      = resourceId;
    out["date"]            = date;
    out["slotSizeMinutes"] = rf::slot::kSlotMinutes;
    out["totalSlotCount"]  = rf::slot::kSlotsPerDay;
    out["occupied"]        = std::move(occupied);
    out["available"]       = std::move(available);
    out["occupiedLabels"]  = std::move(labels);
    return Result<nlohmann::json>::make(std::move(out));
}

// ============================================================================
// P2 增量：取用 / 归还 / 验收 / 损坏 / 押金流水（ARCH-P2 §1.2 / §3.8 / §5.4）
// 五条路径同构：短读 → 抢锁 → 取连接 → 锁内重读 → plan → 事务 → CAS → 副作用 → lost → commit
// ============================================================================

Result<Empty> ReservationService::releaseSlotsQuietly(DbConn& conn, int64_t reservationId) {
    auto del = rrepo_.deleteSlots(conn, reservationId);
    if (!del.ok()) {
        return Result<Empty>::fail(del.code, del.msg);
    }
    return Result<Empty>::make(Empty{});
}

Result<ReservationView> ReservationService::pickup(int64_t id, const AuthContext& ctx) {
    const std::string rid = requestId();
    if (id <= 0) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    auto pre = loadReservation(id);
    if (!pre.ok()) {
        return Result<ReservationView>::fail(pre.code, pre.msg);
    }
    if (!pre.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }
    if (ctx.role != model::Role::ADMIN && pre.data->userId != ctx.userId) {
        return Result<ReservationView>::fail(kReservationForbidden, "无权操作该预约");
    }
    const int64_t resourceId = pre.data->resourceId;

    auto lockRes = lock_.acquireResource(resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        return Result<ReservationView>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<ReservationView>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }
    if (ctx.role != model::Role::ADMIN && cur.data->userId != ctx.userId) {
        return Result<ReservationView>::fail(kReservationForbidden, "无权操作该预约");
    }

    // ---- 时间窗：now ∈ [start - earlyMin, start + lateMin]
    std::time_t startEpoch = 0;
    if (!TimeUtil::parseIso(cur.data->startTime, startEpoch)) {
        return Result<ReservationView>::fail(kReservationTimeInvalid,
                                             "预约时间不合法：单据时间格式异常");
    }
    const int64_t nowEpoch = static_cast<int64_t>(TimeUtil::nowEpoch());
    const int64_t earliest = static_cast<int64_t>(startEpoch) -
                             static_cast<int64_t>(pickupCfg_.earlyMin) * 60;
    const int64_t latest = static_cast<int64_t>(startEpoch) +
                           static_cast<int64_t>(pickupCfg_.lateMin) * 60;
    if (nowEpoch < earliest) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_PICKUP", kPickupTooEarly,
                  id);
        return Result<ReservationView>::fail(
            kPickupTooEarly, "尚未到可取用时间，最早可提前 " + std::to_string(pickupCfg_.earlyMin) +
                                 " 分钟取用");
    }
    if (nowEpoch > latest) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_PICKUP", kPickupExpired,
                  id);
        return Result<ReservationView>::fail(kPickupExpired,
                                             "已超过取用时限，该预约将被系统自动取消");
    }

    TransitRequest tq;
    tq.biz        = BizType::RESERVATION;
    tq.fromStatus = model::toStr(cur.data->status);
    tq.event      = TransitEvent::START_USE;
    tq.operatorId = ctx.userId;
    auto decision = sm_.plan(tq);
    if (!decision.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_PICKUP", decision.code, id);
        return Result<ReservationView>::fail(decision.code, decision.msg);
    }

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：事务开启失败");
    }
    auto cas = rrepo_.casUpdateStatus(conn, id, tq.fromStatus, decision.data.toStatus);
    if (!cas.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(cas.code, cas.msg);
    }
    if (cas.data == 0) {
        tx.rollback();
        return Result<ReservationView>::fail(kReservationStateInvalid,
                                             "预约状态已被其他操作改变，请刷新后重试");
    }
    auto logRes = writeStatusLog(conn, BizType::RESERVATION, id, tq.fromStatus,
                                 decision.data.toStatus, ctx, decision.data.reason);
    if (!logRes.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(logRes.code, logRes.msg);
    }
    auto conv = convergeResource(conn, resourceId, 0, ctx, "取用，资源状态收敛");
    if (!conv.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(conv.code, conv.msg);
    }
    if (lock.lost()) {
        tx.rollback();
        return Result<ReservationView>::fail(kLockLost, "资源锁已失效，请重试");
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<ReservationView>::fail(committed.code, committed.msg);
    }

    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_PICKUP", kOk, id);
    return buildView(conn, id);
}

Result<ReservationView> ReservationService::returnReservation(int64_t id,
                                                             const std::string& remark,
                                                             const AuthContext& ctx) {
    const std::string rid = requestId();
    if (id <= 0) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    auto pre = loadReservation(id);
    if (!pre.ok()) {
        return Result<ReservationView>::fail(pre.code, pre.msg);
    }
    if (!pre.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }
    if (ctx.role != model::Role::ADMIN && pre.data->userId != ctx.userId) {
        return Result<ReservationView>::fail(kReservationForbidden, "无权操作该预约");
    }
    const int64_t resourceId = pre.data->resourceId;

    auto lockRes = lock_.acquireResource(resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        return Result<ReservationView>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<ReservationView>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }
    if (ctx.role != model::Role::ADMIN && cur.data->userId != ctx.userId) {
        return Result<ReservationView>::fail(kReservationForbidden, "无权操作该预约");
    }

    TransitRequest tq;
    tq.biz        = BizType::RESERVATION;
    tq.fromStatus = model::toStr(cur.data->status);
    tq.event      = TransitEvent::SUBMIT_RETURN;
    tq.operatorId = ctx.userId;
    tq.reason     = remark;
    auto decision = sm_.plan(tq);
    if (!decision.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_RETURN", decision.code, id);
        return Result<ReservationView>::fail(decision.code, decision.msg);
    }

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：事务开启失败");
    }
    auto cas = rrepo_.casUpdateStatus(conn, id, tq.fromStatus, decision.data.toStatus);
    if (!cas.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(cas.code, cas.msg);
    }
    if (cas.data == 0) {
        tx.rollback();
        return Result<ReservationView>::fail(kReservationStateInvalid,
                                             "预约状态已被其他操作改变，请刷新后重试");
    }
    auto logRes = writeStatusLog(conn, BizType::RESERVATION, id, tq.fromStatus,
                                 decision.data.toStatus, ctx, decision.data.reason);
    if (!logRes.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(logRes.code, logRes.msg);
    }
    // D-P2-8：归还即释放槽位，让时段立刻可被别人预约（验收路径还会再删一次，D-P2-9）
    auto rel = releaseSlotsQuietly(conn, id);
    if (!rel.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(rel.code, rel.msg);
    }
    auto conv = convergeResource(conn, resourceId, 0, ctx, "归还，资源状态收敛");
    if (!conv.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(conv.code, conv.msg);
    }
    if (lock.lost()) {
        tx.rollback();
        return Result<ReservationView>::fail(kLockLost, "资源锁已失效，请重试");
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<ReservationView>::fail(committed.code, committed.msg);
    }

    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_RETURN", kOk, id);
    // 归还**不派发候补**（D-P2-8：RETURN_CHECK 期间资源不可约，提前派发会让 confirm 撞 3012）
    return buildView(conn, id);
}

Result<ReservationView> ReservationService::accept(int64_t id, const std::string& remark,
                                                  const AuthContext& ctx) {
    const std::string rid = requestId();
    if (ctx.role != model::Role::ADMIN) {
        return Result<ReservationView>::fail(kForbidden, "权限不足，需要管理员角色");
    }
    if (id <= 0) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    auto pre = loadReservation(id);
    if (!pre.ok()) {
        return Result<ReservationView>::fail(pre.code, pre.msg);
    }
    if (!pre.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }
    const int64_t resourceId = pre.data->resourceId;

    auto lockRes = lock_.acquireResource(resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        return Result<ReservationView>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<ReservationView>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    TransitRequest tq;
    tq.biz        = BizType::RESERVATION;
    tq.fromStatus = model::toStr(cur.data->status);
    tq.event      = TransitEvent::PASS_CHECK;
    tq.operatorId = ctx.userId;
    tq.reason     = remark;
    auto decision = sm_.plan(tq);
    if (!decision.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_ACCEPT", decision.code, id);
        return Result<ReservationView>::fail(decision.code, decision.msg);
    }

    int offersDispatched = 0;
    Transaction tx(conn);
    if (!tx.active()) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：事务开启失败");
    }
    auto cas = rrepo_.casUpdateStatus(conn, id, tq.fromStatus, decision.data.toStatus);
    if (!cas.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(cas.code, cas.msg);
    }
    if (cas.data == 0) {
        tx.rollback();
        return Result<ReservationView>::fail(kReservationStateInvalid,
                                             "预约状态已被其他操作改变，请刷新后重试");
    }
    auto logRes = writeStatusLog(conn, BizType::RESERVATION, id, tq.fromStatus,
                                 decision.data.toStatus, ctx, decision.data.reason);
    if (!logRes.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(logRes.code, logRes.msg);
    }
    // D-P2-9：再删一次（幂等，通常 0 行）——防御历史数据或异常分支漏删
    auto rel = releaseSlotsQuietly(conn, id);
    if (!rel.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(rel.code, rel.msg);
    }
    auto conv = convergeResource(conn, resourceId, 0, ctx, "验收通过，资源状态收敛");
    if (!conv.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(conv.code, conv.msg);
    }
    auto refund = settlement_.refundRemaining(conn, id, cur.data->userId, "验收通过，退还押金");
    if (!refund.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(refund.code, refund.msg);
    }
    // §5.4：验收通过时才派发候补（此时资源已离开 RETURN_CHECK，可正常 confirm）
    {
        std::string d;
        int from = 0;
        int to   = 0;
        if (slotRangeOf(cur.data->startTime, cur.data->endTime, d, from, to)) {
            auto disp = waitlist_.dispatchOffers(conn, resourceId, d, from, to);
            if (!disp.ok()) {
                tx.rollback();
                return Result<ReservationView>::fail(disp.code, disp.msg);
            }
            offersDispatched = disp.data;
        }
    }
    if (lock.lost()) {
        tx.rollback();
        return Result<ReservationView>::fail(kLockLost, "资源锁已失效，请重试");
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<ReservationView>::fail(committed.code, committed.msg);
    }

    auto view = buildView(conn, id);
    if (!view.ok()) {
        return view;
    }
    view.data.offersDispatched = offersDispatched;
    auto acc = settlement_.account(conn, id);
    if (acc.ok()) {
        view.data.deposit = acc.data.toJson();
    }
    auto res = rr_.findById(conn, resourceId);
    if (res.ok() && res.data.has_value()) {
        view.data.resourceStatus = model::toStr(res.data->status);
    }
    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_ACCEPT", kOk, id);
    return view;
}

Result<ReservationView> ReservationService::damage(int64_t id, double deductAmount,
                                                   const std::string& reason,
                                                   const AuthContext& ctx) {
    const std::string rid = requestId();
    if (ctx.role != model::Role::ADMIN) {
        return Result<ReservationView>::fail(kForbidden, "权限不足，需要管理员角色");
    }
    if (id <= 0) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    auto pre = loadReservation(id);
    if (!pre.ok()) {
        return Result<ReservationView>::fail(pre.code, pre.msg);
    }
    if (!pre.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }
    const int64_t resourceId = pre.data->resourceId;

    auto lockRes = lock_.acquireResource(resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        return Result<ReservationView>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<ReservationView>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<ReservationView>::fail(kReservationNotFound, "预约不存在");
    }

    TransitRequest tq;
    tq.biz        = BizType::RESERVATION;
    tq.fromStatus = model::toStr(cur.data->status);
    tq.event      = TransitEvent::MARK_DAMAGE;
    tq.operatorId = ctx.userId;
    tq.reason     = reason;
    auto decision = sm_.plan(tq);
    if (!decision.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_DAMAGE", decision.code, id);
        return Result<ReservationView>::fail(decision.code, decision.msg);
    }

    // 资源侧目标态同样由状态机给出（RESOURCE 的 MARK_DAMAGE 是通配规则 → DAMAGE_CLAIM）
    auto resCur = rr_.findById(conn, resourceId);
    if (!resCur.ok()) {
        return Result<ReservationView>::fail(resCur.code, resCur.msg);
    }
    if (!resCur.data.has_value()) {
        return Result<ReservationView>::fail(kResourceNotFound, "资源不存在");
    }
    TransitRequest rq;
    rq.biz        = BizType::RESOURCE;
    rq.fromStatus = model::toStr(resCur.data->status);
    rq.event      = TransitEvent::MARK_DAMAGE;
    rq.operatorId = ctx.userId;
    rq.reason     = reason;
    auto resDecision = sm_.plan(rq);
    if (!resDecision.ok()) {
        return Result<ReservationView>::fail(resDecision.code, resDecision.msg);
    }
    const auto resTarget =
        model::resourceStateFrom(resDecision.data.toStatus).value_or(model::ResourceState::DAMAGE_CLAIM);

    // ---- SETTLE 阶段求值（ARCH-P3 §1.1 ③）：损坏扣分分值（#9）+ 押金扣减比例/上限（#10）
    //      引擎关闭或降级时回落到 P2 的 config 值，行为与 P2 完全一致。
    int         score     = maintCfg_.damageDeductScore;
    double      ruleRatio = settlement_.config().damageDeductRatio;
    double      ruleMax   = settlement_.config().damageDeductMax;
    std::string settleRuleCode;
    if (engine_.enabled()) {
        FactBag sf;
        putBaseFacts(sf, TimeUtil::nowString());
        sf.set("user.id", Fact::of(static_cast<double>(cur.data->userId)));
        auto owner = ur_.findById(conn, cur.data->userId);
        if (owner.ok() && owner.data.has_value()) {
            sf.set("user.status", Fact::of(model::toStr(owner.data->status)));
            sf.set("user.role", Fact::of(model::toStr(owner.data->role)));
            sf.set("user.creditScore", Fact::of(static_cast<double>(owner.data->creditScore)));
        }
        sf.set("user.activeReservations", Fact::of(0.0));
        sf.set("resource.id", Fact::of(static_cast<double>(resourceId)));
        sf.set("resource.type", Fact::of(model::toStr(resCur.data->type)));
        sf.set("resource.status", Fact::of(model::toStr(resCur.data->status)));
        sf.set("resource.depositAmount", Fact::of(resCur.data->depositAmount));
        sf.set("reservation.id", Fact::of(static_cast<double>(id)));
        sf.set("reservation.status", Fact::of(model::toStr(cur.data->status)));
        sf.setTime("reservation.startTime", cur.data->startTime);
        sf.setTime("reservation.endTime", cur.data->endTime);
        sf.setTime("reservation.createdAt", cur.data->createdAt);
        sf.set("damage.requestedAmount", Fact::of(deductAmount));
        auto acc0 = settlement_.account(conn, id);
        if (acc0.ok()) {
            sf.set("deposit.frozen", Fact::of(acc0.data.frozen));
            sf.set("deposit.deducted", Fact::of(acc0.data.deducted));
            sf.set("deposit.refunded", Fact::of(acc0.data.refunded));
        }
        const EvalResult sev = engine_.evaluate(EvalPhase::SETTLE, sf);
        if (!sev.degraded) {
            if (const ResolvedAction* a = sev.findAction(ActionType::DeductOnly); a != nullptr) {
                if (a->deductScore > 0) {
                    score          = a->deductScore;
                    settleRuleCode = a->ruleCode;
                }
            }
            if (const ResolvedAction* d = sev.findAction(ActionType::DeductDeposit);
                d != nullptr && d->ratio > 0.0) {
                ruleRatio = d->ratio;
                ruleMax   = d->maxAmount;
            }
        }
    }

    double   deducted = 0.0;
    int      creditAfter = -1;
    nlohmann::json violationJson = nlohmann::json::object();
    bool     scoredOk = false;

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<ReservationView>::fail(kDbError, "数据库访问失败：事务开启失败");
    }
    auto cas = rrepo_.casUpdateStatus(conn, id, tq.fromStatus, decision.data.toStatus);
    if (!cas.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(cas.code, cas.msg);
    }
    if (cas.data == 0) {
        tx.rollback();
        return Result<ReservationView>::fail(kReservationStateInvalid,
                                             "预约状态已被其他操作改变，请刷新后重试");
    }
    // ---- 以下副作用全部在 CAS 成功之后（D-P2-5）
    auto logRes = writeStatusLog(conn, BizType::RESERVATION, id, tq.fromStatus,
                                 decision.data.toStatus, ctx, decision.data.reason, settleRuleCode);
    if (!logRes.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(logRes.code, logRes.msg);
    }
    auto rel = releaseSlotsQuietly(conn, id);
    if (!rel.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(rel.code, rel.msg);
    }
    auto ded = settlement_.deductForDamage(conn, id, cur.data->userId, deductAmount,
                                           reason.empty() ? "验收损坏扣减" : reason, &deducted,
                                           ruleRatio, ruleMax);
    if (!ded.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(ded.code, ded.msg);
    }
    auto refund = settlement_.refundRemaining(conn, id, cur.data->userId, "损坏扣减后退还剩余");
    if (!refund.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(refund.code, refund.msg);
    }
    if (score > 0) {
        auto pen = settlement_.penalize(conn, id, cur.data->userId,
                                        model::toStr(model::ViolationType::DAMAGE), 0, score,
                                        ctx.userId,
                                        reason.empty() ? "损坏认定" : reason, settleRuleCode);
        if (pen.ok()) {
            scoredOk = true;
        } else if (pen.code != kSlotUnavailable) {
            tx.rollback();
            return Result<ReservationView>::fail(pen.code, pen.msg);
        }
    }
    // 资源 → DAMAGE_CLAIM（先收敛还原真实占用，再由显式规则覆盖为理赔态）
    auto conv = convergeResource(conn, resourceId, 0, ctx, "损坏结案，资源状态收敛");
    if (!conv.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(conv.code, conv.msg);
    }
    const std::string resFrom = model::toStr(resCur.data->status);
    if (resFrom != resDecision.data.toStatus) {
        auto upd = rr_.updateStatus(conn, resourceId, resTarget);
        if (!upd.ok()) {
            tx.rollback();
            return Result<ReservationView>::fail(upd.code, upd.msg);
        }
        auto rlog = writeStatusLog(conn, BizType::RESOURCE, resourceId, resFrom,
                                   resDecision.data.toStatus, ctx, resDecision.data.reason);
        if (!rlog.ok()) {
            tx.rollback();
            return Result<ReservationView>::fail(rlog.code, rlog.msg);
        }
    }
    // 资源不可约 → 作废其全部候补（派发无意义）
    auto cancelled = waitlist_.cancelAllForResource(conn, resourceId, "资源进入理赔态，候补自动作废");
    if (!cancelled.ok()) {
        tx.rollback();
        return Result<ReservationView>::fail(cancelled.code, cancelled.msg);
    }
    if (scoredOk) {
        auto u = ur_.findById(conn, cur.data->userId);
        if (u.ok() && u.data.has_value()) {
            creditAfter = u.data->creditScore;
        }
        violationJson["type"]        = model::toStr(model::ViolationType::DAMAGE);
        violationJson["deductScore"] = score;
    }
    if (lock.lost()) {
        tx.rollback();
        return Result<ReservationView>::fail(kLockLost, "资源锁已失效，请重试");
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<ReservationView>::fail(committed.code, committed.msg);
    }

    auto view = buildView(conn, id);
    if (!view.ok()) {
        return view;
    }
    view.data.offersDispatched = 0;   // 损坏结案不派发，只作废
    view.data.resourceStatus   = resDecision.data.toStatus;
    if (!violationJson.empty()) {
        view.data.violation = violationJson;
    }
    if (creditAfter >= 0) {
        view.data.creditScoreAfter = creditAfter;
    }
    auto acc = settlement_.account(conn, id);
    if (acc.ok()) {
        view.data.deposit = acc.data.toJson();
    }
    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), "RESERVATION_DAMAGE", kOk, id);
    return view;
}

Result<nlohmann::json> ReservationService::deposits(int64_t id, const AuthContext& ctx) {
    if (id <= 0) {
        return Result<nlohmann::json>::fail(kReservationNotFound, "预约不存在");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<nlohmann::json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<nlohmann::json>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<nlohmann::json>::fail(kReservationNotFound, "预约不存在");
    }
    if (ctx.role != model::Role::ADMIN && cur.data->userId != ctx.userId) {
        return Result<nlohmann::json>::fail(kReservationForbidden, "无权操作该预约");
    }

    auto list = settlement_.listDeposits(conn, id);
    if (!list.ok()) {
        return Result<nlohmann::json>::fail(list.code, list.msg);
    }
    auto acc = settlement_.account(conn, id);
    if (!acc.ok()) {
        return Result<nlohmann::json>::fail(acc.code, acc.msg);
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : list.data) {
        arr.push_back(d.toJson());
    }
    nlohmann::json out = nlohmann::json::object();
    out["reservationId"] = id;
    out["account"]       = acc.data.toJson();
    out["list"]          = std::move(arr);
    return Result<nlohmann::json>::make(std::move(out));
}

// ============================================================================
// P4 T00 新增（只读）：状态时间线
// 复用 ReservationRepo::listTimeline（P2 就实现了，但一直没接线）。
// ============================================================================
Result<nlohmann::json> ReservationService::timeline(int64_t id, const AuthContext& ctx) {
    if (id <= 0) {
        return Result<nlohmann::json>::fail(kReservationNotFound, "预约不存在");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<nlohmann::json>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = rrepo_.findById(conn, id);
    if (!cur.ok()) {
        return Result<nlohmann::json>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        return Result<nlohmann::json>::fail(kReservationNotFound, "预约不存在");
    }
    // 权限与 detail / deposits 一致：本人或管理员
    if (ctx.role != model::Role::ADMIN && cur.data->userId != ctx.userId) {
        return Result<nlohmann::json>::fail(kReservationForbidden, "无权操作该预约");
    }

    auto rows = rrepo_.listTimeline(conn, id);
    if (!rows.ok()) {
        return Result<nlohmann::json>::fail(rows.code, rows.msg);
    }
    // listTimeline 是 id 升序（正序）；时间线按「最新在前」输出
    nlohmann::json list = nlohmann::json::array();
    for (auto it = rows.data.rbegin(); it != rows.data.rend(); ++it) {
        list.push_back(it->toJson());
    }
    nlohmann::json out = nlohmann::json::object();
    out["reservationId"] = id;
    out["total"]         = rows.data.size();
    out["list"]          = std::move(list);
    return Result<nlohmann::json>::make(std::move(out));
}

} // namespace rf
