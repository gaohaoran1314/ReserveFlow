// =============================================================================
// src/WaitlistService.cpp  —  候补业务实现（T05 范围：join / leave / list）
// 对应 ARCH-P2 §3.6 / §7.1 / §7.3 / D-P2-13
//
// 本轮刻意只实现三个方法；confirm / dispatchOffers / createReservationForOffer
// 的定义由 T08 补齐（声明已在头文件，当前无调用点，不影响链接）。
//
// 入队为什么持资源锁：去重是「查 WAITING 行 + 插入」两步，两个并发请求会同时查到 0 条。
// 锁把这两步串起来；退队不持锁是安全的（CAS 本身即可裁决，且无跨表副作用）。
// =============================================================================
#include "WaitlistService.h"

#include <string>
#include <utility>

#include <ctime>

#include "Base.h"
#include "ReservationService.h"   // 复用 generateReservationNo（单号唯一来源）
#include "SlotUtil.h"

namespace rf {

namespace {

/** 候补备注长度上限（与 waitlist.remark VARCHAR(255) 对齐） */
constexpr size_t kMaxRemarkLen = 255;

constexpr const char* kActJoin    = "WAITLIST_JOIN";
constexpr const char* kActLeave   = "WAITLIST_LEAVE";
constexpr const char* kActConfirm = "WAITLIST_CONFIRM";
constexpr const char* kActDispatch = "WAITLIST_DISPATCH";

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
 * 槽位序号 → ISO 时间点。index==48（当日 24:00）折到次日 00:00，
 * 避免往 DATETIME 里写 "24:00:00"。
 */
std::string slotIso(const std::string& date, int index) {
    if (index < rf::slot::kSlotsPerDay) {
        return date + "T" + rf::slot::slotLabel(index) + ":00";
    }
    rf::slot::DateTime d;
    if (!rf::slot::parseDate(date, d)) {
        return date + "T00:00:00";
    }
    std::tm t{};
    t.tm_year  = d.year - 1900;
    t.tm_mon   = d.month - 1;
    t.tm_mday  = d.day + 1;   // mktime 会自动进位到月末/年末
    t.tm_hour  = 0;
    t.tm_min   = 0;
    t.tm_sec   = 0;
    t.tm_isdst = -1;
    const std::time_t v = std::mktime(&t);
    if (v == static_cast<std::time_t>(-1)) {
        return date + "T00:00:00";
    }
    return rf::TimeUtil::format(v);
}

}  // namespace

WaitlistService::WaitlistService(DbPool& pool, RedisClient& redis, repo::WaitlistRepo& wrepo,
                                 repo::ReservationRepo& rrepo, repo::ResourceRepo& rr,
                                 repo::UserRepo& ur, repo::AuditRepo& ar, StateMachine& sm,
                                 DistributedLock& lock, SettlementService& settlement,
                                 const WaitlistConfig& wcfg, int lockWaitMs, int lockTtlMs,
                                 int minCreditScore)
    : pool_(pool),
      redis_(redis),
      wrepo_(wrepo),
      rrepo_(rrepo),
      rr_(rr),
      ur_(ur),
      ar_(ar),
      sm_(sm),
      lock_(lock),
      settlement_(settlement),
      cfg_(wcfg),
      lockWaitMs_(lockWaitMs > 0 ? lockWaitMs : 3000),
      lockTtlMs_(lockTtlMs > 0 ? lockTtlMs : 30000),
      minCreditScore_(minCreditScore < 0 ? 0 : minCreditScore) {}

// ------------------------------------------------------------------ 入队
Result<model::WaitlistItem> WaitlistService::join(const JoinWaitlistReq& req,
                                                  const AuthContext& ctx) {
    const std::string rid = requestId();

    // ---- 1) 入参兜底
    if (req.resourceId <= 0) {
        return Result<model::WaitlistItem>::fail(kInvalidParam,
                                                 "参数校验失败：resourceId 必须是正整数");
    }
    if (req.remark.size() > kMaxRemarkLen) {
        return Result<model::WaitlistItem>::fail(kInvalidParam,
                                                 "参数校验失败：remark 长度上限 255");
    }

    // ---- 2) 时段校验（纯计算，无锁无连接）
    const rf::slot::SpanResult span =
        rf::slot::makeSpan(req.startTime, req.endTime, TimeUtil::nowString());
    if (!span.ok) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, kReservationTimeInvalid,
                  req.resourceId);
        return Result<model::WaitlistItem>::fail(kReservationTimeInvalid, span.err);
    }

    // ---- 3) 抢资源锁（零连接占用，D-P1-4）：让「去重查询 + 插入」成为原子段
    auto lockRes = lock_.acquireResource(req.resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, lockRes.code, req.resourceId);
        return Result<model::WaitlistItem>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<model::WaitlistItem>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    // ---- 4) 账号合规（禁用户/黑名单不允许排队）
    auto user = ur_.findById(conn, ctx.userId);
    if (!user.ok()) {
        return Result<model::WaitlistItem>::fail(user.code, user.msg);
    }
    if (!user.data.has_value()) {
        return Result<model::WaitlistItem>::fail(kInvalidCredential, "账号不存在或已被删除");
    }
    if (user.data->status == model::UserStatus::DISABLED) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, kAccountDisabled,
                  req.resourceId);
        return Result<model::WaitlistItem>::fail(kAccountDisabled, "账号已被禁用，无法使用候补功能");
    }
    if (user.data->status == model::UserStatus::BLACKLIST) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, kUserBlacklisted,
                  req.resourceId);
        return Result<model::WaitlistItem>::fail(kUserBlacklisted, "账号处于黑名单，无法候补");
    }

    // ---- 5) 资源存在且可约（5 态不可约集合，D-P2-10）
    auto res = rr_.findById(conn, req.resourceId);
    if (!res.ok()) {
        return Result<model::WaitlistItem>::fail(res.code, res.msg);
    }
    if (!res.data.has_value()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, kResourceNotFound,
                  req.resourceId);
        return Result<model::WaitlistItem>::fail(kResourceNotFound, "资源不存在");
    }
    if (model::isUnbookableState(res.data->status)) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, kResourceNotBookable,
                  req.resourceId);
        return Result<model::WaitlistItem>::fail(
            kResourceNotBookable,
            "资源当前不可预约（" + std::string(model::labelOf(res.data->status)) + "）");
    }

    // ---- 6) 去重：同用户同资源同日同窗口已有 WAITING → 3020
    auto dup = wrepo_.existsWaiting(conn, req.resourceId, ctx.userId, span.date, span.startIndex,
                                    span.endIndex);
    if (!dup.ok()) {
        return Result<model::WaitlistItem>::fail(dup.code, dup.msg);
    }
    if (dup.data) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, kWaitlistDup, req.resourceId);
        return Result<model::WaitlistItem>::fail(kWaitlistDup, "该时段已在候补队列中");
    }

    // ---- 7) 配额：WAITING + OFFERED 不超过 maxPerUser
    auto cnt = wrepo_.countActiveByUser(conn, ctx.userId);
    if (!cnt.ok()) {
        return Result<model::WaitlistItem>::fail(cnt.code, cnt.msg);
    }
    if (cnt.data >= static_cast<int64_t>(cfg_.maxPerUser)) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, kWaitlistDup, req.resourceId);
        return Result<model::WaitlistItem>::fail(
            kWaitlistDup, "候补数量已达上限（最多 " + std::to_string(cfg_.maxPerUser) + " 条）");
    }

    // ---- 8) 落库
    model::WaitlistItem w;
    w.resourceId = req.resourceId;
    w.userId     = ctx.userId;
    w.slotDate   = span.date;
    w.startIndex = span.startIndex;
    w.endIndex   = span.endIndex;
    w.status     = model::WaitlistStatus::WAITING;
    w.remark     = req.remark;

    auto ins = wrepo_.insert(conn, w);
    if (!ins.ok()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, ins.code, req.resourceId);
        return Result<model::WaitlistItem>::fail(ins.code, ins.msg);
    }

    auto saved = wrepo_.findById(conn, ins.data);
    if (!saved.ok()) {
        return Result<model::WaitlistItem>::fail(saved.code, saved.msg);
    }
    if (!saved.data.has_value()) {
        return Result<model::WaitlistItem>::fail(kWaitlistNotFound, "候补记录不存在");
    }
    model::WaitlistItem item = *saved.data;
    auto pos = wrepo_.positionOf(conn, item);
    if (pos.ok()) {
        item.position = pos.data;
    }

    LOG_INFO("rid={} 加入候补 id={} 资源={} {} 槽[{},{}) 名次={}", rid, item.id, item.resourceId,
             item.slotDate, item.startIndex, item.endIndex, item.position);
    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActJoin, kOk, item.id);
    return Result<model::WaitlistItem>::make(std::move(item));
}

// ------------------------------------------------------------------ 我的候补
Result<repo::WaitlistPage> WaitlistService::list(const repo::WaitlistQuery& q,
                                                 const AuthContext& ctx) {
    repo::WaitlistQuery query = q;
    query.page = (query.page < 1) ? 1 : (query.page > 100000 ? 100000 : query.page);
    query.size = (query.size < 1) ? 10 : (query.size > 100 ? 100 : query.size);

    // 普通用户只能看自己；管理员可看全部，也可显式按 userId 过滤
    if (ctx.role != model::Role::ADMIN) {
        query.userId = ctx.userId;
    }
    if (query.status.has_value()) {
        if (!model::waitlistStatusFrom(*query.status).has_value()) {
            return Result<repo::WaitlistPage>::fail(
                kInvalidParam, "参数校验失败：status 取值非法（" + *query.status + "）");
        }
    }
    if (query.resourceId.has_value() && *query.resourceId <= 0) {
        return Result<repo::WaitlistPage>::fail(kInvalidParam,
                                                "参数校验失败：resourceId 必须是正整数");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<repo::WaitlistPage>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto page = wrepo_.search(conn, query);
    if (!page.ok()) {
        return page;
    }
    // 只给「还在排队」的行补 FIFO 名次（前端展示「你前面还有 N 位」）；
    // OFFERED/CONVERTED 等不存在"名次"概念，保持 0。
    for (auto& item : page.data.list) {
        if (item.status != model::WaitlistStatus::WAITING) {
            continue;
        }
        auto pos = wrepo_.positionOf(conn, item);
        if (pos.ok()) {
            item.position = pos.data;
        }
    }
    return page;
}

// ------------------------------------------------------------------ 退队
Result<Empty> WaitlistService::leave(int64_t waitlistId, const AuthContext& ctx) {
    const std::string rid = requestId();
    if (waitlistId <= 0) {
        return Result<Empty>::fail(kWaitlistNotFound, "候补记录不存在");
    }

    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<Empty>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    auto cur = wrepo_.findById(conn, waitlistId);
    if (!cur.ok()) {
        return Result<Empty>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActLeave, kWaitlistNotFound,
                  waitlistId);
        return Result<Empty>::fail(kWaitlistNotFound, "候补记录不存在");
    }
    if (ctx.role != model::Role::ADMIN && cur.data->userId != ctx.userId) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActLeave, kReservationForbidden,
                  waitlistId);
        return Result<Empty>::fail(kReservationForbidden, "无权操作该候补记录");
    }

    const std::string from = model::toStr(cur.data->status);
    if (cur.data->status != model::WaitlistStatus::WAITING &&
        cur.data->status != model::WaitlistStatus::OFFERED) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActLeave, kWaitlistStateInvalid,
                  waitlistId);
        return Result<Empty>::fail(kWaitlistStateInvalid,
                                   "当前候补状态不允许该操作（已是 " +
                                       std::string(model::labelOf(cur.data->status)) + "）");
    }

    // CAS 裁决：并发下只有一个人能改成 CANCELLED
    auto cas = wrepo_.casUpdateStatus(conn, waitlistId, from,
                                      model::toStr(model::WaitlistStatus::CANCELLED),
                                      std::nullopt, std::nullopt);
    if (!cas.ok()) {
        return Result<Empty>::fail(cas.code, cas.msg);
    }
    if (cas.data == 0) {
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActLeave, kWaitlistStateInvalid,
                  waitlistId);
        return Result<Empty>::fail(kWaitlistStateInvalid, "当前候补状态不允许该操作（已被处理）");
    }

    LOG_INFO("rid={} 退出候补 id={} 原状态={}", rid, waitlistId, from);
    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActLeave, kOk, waitlistId);
    return Result<Empty>::make(Empty{});
}

// ============================================================================
// P2 T08：offer 派发 + confirm 转正（ARCH-P2 §7.4 / §7.5）
// ============================================================================

namespace {

/** now + N 分钟的 ISO 字符串 */
std::string isoAfterMin(int minutes) {
    const std::time_t t = rf::TimeUtil::nowEpoch() + static_cast<std::time_t>(minutes) * 60;
    return rf::TimeUtil::format(t);
}

}  // namespace

Result<int> WaitlistService::dispatchOffers(DbConn& conn, int64_t resourceId,
                                            const std::string& slotDate, int fromIndex, int toIndex,
                                            int maxOffers) {
    if (resourceId <= 0 || slotDate.empty() || fromIndex >= toIndex || maxOffers <= 0) {
        return Result<int>::make(0);
    }

    // ⓪ 资源不可约（5 态集合）→ 派发无意义（§7.4 第 0 步）
    auto res = rr_.findById(conn, resourceId);
    if (!res.ok()) {
        return Result<int>::fail(res.code, res.msg);
    }
    if (!res.data.has_value()) {
        return Result<int>::make(0);
    }
    if (model::isUnbookableState(res.data->status)) {
        LOG_DEBUG("资源 {} 状态 {} 不可约，跳过候补派发", resourceId,
                  model::toStr(res.data->status));
        return Result<int>::make(0);
    }

    int                  dispatched = 0;
    std::vector<int64_t> skipped;   // 窗口被占的候选，避免 LIMIT 1 反复选中同一条
    while (dispatched < maxOffers) {
        auto cand = wrepo_.pickFirstEligible(conn, resourceId, slotDate, fromIndex, toIndex, true,
                                             skipped);
        if (!cand.ok()) {
            return Result<int>::fail(cand.code, cand.msg);
        }
        if (!cand.data.has_value()) {
            break;   // 队列里没有窗口完整落在本区间的候选了
        }
        const model::WaitlistItem& w = *cand.data;

        // 可用性复检：只信真实槽表，不信「这次释放了多少」
        std::vector<int> range;
        range.reserve(static_cast<size_t>(w.endIndex - w.startIndex));
        for (int i = w.startIndex; i < w.endIndex; ++i) {
            range.push_back(i);
        }
        auto occ = rrepo_.occupiedIndexes(conn, resourceId, slotDate, range);
        if (!occ.ok()) {
            return Result<int>::fail(occ.code, occ.msg);
        }
        if (!occ.data.empty()) {
            skipped.push_back(w.id);
            continue;
        }
        // 尚未 confirm 的 offer 在槽表里看不到，必须单独查一次，否则会把同一时段
        // 重复发给多个候补（两个人都收到 offer，先 confirm 的赢，后者白等 15 分钟）
        auto overlap = wrepo_.hasOfferedOverlap(conn, resourceId, slotDate, w.startIndex, w.endIndex);
        if (!overlap.ok()) {
            return Result<int>::fail(overlap.code, overlap.msg);
        }
        if (overlap.data) {
            skipped.push_back(w.id);
            continue;
        }

        const std::string expires = isoAfterMin(cfg_.offerTtlMin);
        auto cas = wrepo_.casUpdateStatus(conn, w.id, model::toStr(model::WaitlistStatus::WAITING),
                                          model::toStr(model::WaitlistStatus::OFFERED), expires,
                                          std::nullopt);
        if (!cas.ok()) {
            return Result<int>::fail(cas.code, cas.msg);
        }
        if (cas.data == 1) {
            ++dispatched;
            LOG_INFO("派发候补 offer：waitlist={} 资源={} {} 槽[{},{}) 截止={}", w.id, resourceId,
                     slotDate, w.startIndex, w.endIndex, expires);
        }
        // cas==0：别人抢先改了状态，继续看下一位
    }
    return Result<int>::make(dispatched);
}

Result<int64_t> WaitlistService::createReservationForOffer(DbConn& conn,
                                                          const model::WaitlistItem& w,
                                                          const AuthContext& ctx) {
    // ---- 资源必须仍然可约（5 态集合）
    auto res = rr_.findById(conn, w.resourceId);
    if (!res.ok()) {
        return Result<int64_t>::fail(res.code, res.msg);
    }
    if (!res.data.has_value()) {
        return Result<int64_t>::fail(kResourceNotFound, "资源不存在");
    }
    if (model::isUnbookableState(res.data->status)) {
        return Result<int64_t>::fail(
            kResourceNotBookable,
            "资源当前不可预约（" + std::string(model::labelOf(res.data->status)) + "）");
    }

    // ---- 状态机决定初始状态（免审核分流）
    TransitRequest tq;
    tq.biz          = BizType::RESERVATION;
    tq.fromStatus   = "";
    tq.event        = TransitEvent::CREATE;
    tq.operatorId   = ctx.userId;
    tq.resourceType = res.data->type;
    auto decision   = sm_.plan(tq);
    if (!decision.ok()) {
        return Result<int64_t>::fail(decision.code, decision.msg);
    }

    // ---- 单号（与下单同一条 Lua / 同一个格式，避免两处漂移）
    auto no = ReservationService::generateReservationNo(redis_, compactDate(w.slotDate));
    if (!no.ok()) {
        return Result<int64_t>::fail(no.code, no.msg);
    }

    model::Reservation rec;
    rec.reservationNo = no.data;
    rec.userId        = w.userId;
    rec.resourceId    = w.resourceId;
    rec.startTime     = slotIso(w.slotDate, w.startIndex);
    rec.endTime       = slotIso(w.slotDate, w.endIndex);
    rec.status        = model::reservationStatusFrom(decision.data.toStatus)
                            .value_or(model::ReservationStatus::LOCKED);
    rec.depositAmount = res.data->depositAmount;
    rec.remark        = w.remark.empty() ? std::string("候补转正") : w.remark;

    auto ins = rrepo_.insert(conn, rec);
    if (!ins.ok()) {
        return Result<int64_t>::fail(ins.code, ins.msg);
    }
    const int64_t newId = ins.data;

    std::vector<int> indexes;
    indexes.reserve(static_cast<size_t>(w.endIndex - w.startIndex));
    for (int i = w.startIndex; i < w.endIndex; ++i) {
        indexes.push_back(i);
    }
    auto slots = rrepo_.insertSlots(conn, newId, w.resourceId, w.slotDate, indexes);
    if (!slots.ok()) {
        return Result<int64_t>::fail(slots.code, slots.msg);   // 3005：槽位又被抢
    }

    // ---- 预约单留痕
    repo::StatusLogEntry e;
    e.businessType = "RESERVATION";
    e.businessId   = newId;
    e.fromStatus   = "";
    e.toStatus     = decision.data.toStatus;
    e.operatorId   = ctx.userId;
    e.reason       = "候补名额转正（" + decision.data.reason + "）";
    e.ruleCode     = std::nullopt;
    auto lr        = ar_.insertStatusLog(conn, e);
    if (!lr.ok()) {
        return Result<int64_t>::fail(lr.code, lr.msg);
    }

    // ---- 资源态收敛 + 资源留痕（与 ReservationService::convergeResource 同款 20 行；
    //      那边是私有方法，这里为不改动已验收的 T07 代码而独立实现，口径一致）
    {
        auto actives = rrepo_.listActiveStatuses(conn, w.resourceId, 0);
        if (!actives.ok()) {
            return Result<int64_t>::fail(actives.code, actives.msg);
        }
        const std::string from = model::toStr(res.data->status);
        const std::string to   = model::toStr(sm_.convergeResource(from, actives.data));
        if (from != to) {
            auto upd = rr_.updateStatus(conn, w.resourceId,
                                        model::resourceStateFrom(to).value_or(
                                            model::ResourceState::FREE));
            if (!upd.ok()) {
                return Result<int64_t>::fail(upd.code, upd.msg);
            }
            repo::StatusLogEntry re;
            re.businessType = "RESOURCE";
            re.businessId   = w.resourceId;
            re.fromStatus   = from;
            re.toStatus     = to;
            re.operatorId   = ctx.userId;
            re.reason       = "候补转正，资源状态收敛";
            re.ruleCode     = std::nullopt;
            auto rl         = ar_.insertStatusLog(conn, re);
            if (!rl.ok()) {
                return Result<int64_t>::fail(rl.code, rl.msg);
            }
        }
    }

    // ---- 押金冻结（>0 才写，D-P2-12）
    auto frozen = settlement_.freeze(conn, newId, w.userId, res.data->depositAmount);
    if (!frozen.ok()) {
        return Result<int64_t>::fail(frozen.code, frozen.msg);
    }
    return Result<int64_t>::make(newId);
}

Result<model::WaitlistItem> WaitlistService::confirm(int64_t waitlistId, const AuthContext& ctx) {
    const std::string rid = requestId();
    if (waitlistId <= 0) {
        return Result<model::WaitlistItem>::fail(kWaitlistNotFound, "候补记录不存在");
    }

    // ① 短读定位资源（读完立刻还连接，等锁期间零连接占用）
    int64_t resourceId = 0;
    {
        DbPool::Guard g = pool_.acquire(3000);
        if (!g) {
            return Result<model::WaitlistItem>::fail(kDbError,
                                                     "数据库访问失败：获取数据库连接超时");
        }
        auto cur = wrepo_.findById(*g, waitlistId);
        if (!cur.ok()) {
            return Result<model::WaitlistItem>::fail(cur.code, cur.msg);
        }
        if (!cur.data.has_value()) {
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActConfirm, kWaitlistNotFound,
                      waitlistId);
            return Result<model::WaitlistItem>::fail(kWaitlistNotFound, "候补记录不存在");
        }
        if (ctx.role != model::Role::ADMIN && cur.data->userId != ctx.userId) {
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActConfirm, kReservationForbidden,
                      waitlistId);
            return Result<model::WaitlistItem>::fail(kReservationForbidden, "无权操作该候补记录");
        }
        resourceId = cur.data->resourceId;
    }

    // ② 抢资源锁（零连接占用）
    auto lockRes = lock_.acquireResource(resourceId, lockWaitMs_, lockTtlMs_);
    if (!lockRes.ok()) {
        return Result<model::WaitlistItem>::fail(lockRes.code, lockRes.msg);
    }
    LockHandle lock = std::move(lockRes.data);

    // ③ 取连接 + 事务
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<model::WaitlistItem>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<model::WaitlistItem>::fail(kDbError, "数据库访问失败：事务开启失败");
    }

    // ④ 锁内重读（FOR UPDATE）
    auto cur = wrepo_.findByIdForUpdate(conn, waitlistId);
    if (!cur.ok()) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(cur.code, cur.msg);
    }
    if (!cur.data.has_value()) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(kWaitlistNotFound, "候补记录不存在");
    }
    if (ctx.role != model::Role::ADMIN && cur.data->userId != ctx.userId) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(kReservationForbidden, "无权操作该候补记录");
    }
    if (cur.data->status != model::WaitlistStatus::OFFERED) {
        tx.rollback();
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActConfirm, kWaitlistStateInvalid,
                  waitlistId);
        return Result<model::WaitlistItem>::fail(
            kWaitlistStateInvalid,
            "当前候补状态不允许该操作（当前为 " +
                std::string(model::labelOf(cur.data->status)) + "）");
    }

    // 过期检查：过期是既成事实，先落 EXPIRED 并提交，再返回 3021
    {
        std::time_t expires = 0;
        if (!cur.data->offerExpiresAt.empty() &&
            rf::TimeUtil::parseIso(cur.data->offerExpiresAt, expires) &&
            rf::TimeUtil::nowEpoch() > expires) {
            (void)wrepo_.casUpdateStatus(conn, waitlistId, model::toStr(model::WaitlistStatus::OFFERED),
                                         model::toStr(model::WaitlistStatus::EXPIRED), std::nullopt,
                                         std::nullopt);
            auto committed = tx.commit();
            if (!committed.ok()) {
                return Result<model::WaitlistItem>::fail(committed.code, committed.msg);
            }
            LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActConfirm, kOfferExpired,
                      waitlistId);
            return Result<model::WaitlistItem>::fail(kOfferExpired, "候补名额已过期，请重新排队");
        }
    }

    // ⑤ 用户合规：黑名单 / 信用分
    auto user = ur_.findById(conn, cur.data->userId);
    if (!user.ok()) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(user.code, user.msg);
    }
    if (!user.data.has_value()) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(kInvalidCredential, "账号不存在或已被删除");
    }
    if (user.data->status == model::UserStatus::DISABLED) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(kAccountDisabled, "账号已被禁用，无法确认候补");
    }
    if (user.data->status == model::UserStatus::BLACKLIST) {
        tx.rollback();
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActConfirm, kUserBlacklisted,
                  waitlistId);
        return Result<model::WaitlistItem>::fail(kUserBlacklisted, "账号处于黑名单，无法确认候补");
    }
    if (minCreditScore_ > 0 && user.data->creditScore < minCreditScore_) {
        tx.rollback();
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActConfirm, kCreditTooLow, waitlistId);
        return Result<model::WaitlistItem>::fail(
            kCreditTooLow, "信用分不足，无法确认候补（当前 " +
                               std::to_string(user.data->creditScore) + " 分）");
    }

    // ⑥⑦⑧ 建预约（内部含资源可约复检、plan、插单、插槽、留痕、冻结）
    auto created = createReservationForOffer(conn, *cur.data, ctx);
    if (!created.ok()) {
        tx.rollback();   // 3005 时回滚：候补保持 OFFERED，用户可在过期前重试（§7.5 ⑧）
        LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActConfirm, created.code, waitlistId);
        return Result<model::WaitlistItem>::fail(created.code, created.msg);
    }

    // ⑨ 候补转正（CAS 裁决）
    auto cas = wrepo_.casUpdateStatus(conn, waitlistId, model::toStr(model::WaitlistStatus::OFFERED),
                                     model::toStr(model::WaitlistStatus::CONVERTED), std::nullopt,
                                     created.data);
    if (!cas.ok()) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(cas.code, cas.msg);
    }
    if (cas.data != 1) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(kWaitlistStateInvalid,
                                                 "当前候补状态不允许该操作（已被处理）");
    }

    // ⑪ 提交前查锁
    if (lock.lost()) {
        tx.rollback();
        return Result<model::WaitlistItem>::fail(kLockLost, "资源锁已失效，请重试");
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<model::WaitlistItem>::fail(committed.code, committed.msg);
    }

    auto saved = wrepo_.findById(conn, waitlistId);
    if (!saved.ok()) {
        return Result<model::WaitlistItem>::fail(saved.code, saved.msg);
    }
    if (!saved.data.has_value()) {
        return Result<model::WaitlistItem>::fail(kWaitlistNotFound, "候补记录不存在");
    }
    LOG_INFO("rid={} 候补转正：waitlist={} → 预约 id={}", rid, waitlistId, created.data);
    LOG_AUDIT(rid, ctx.userId, model::toStr(ctx.role), kActConfirm, kOk, created.data);
    return Result<model::WaitlistItem>::make(*saved.data);
}

Result<int> WaitlistService::dispatchDebug(int64_t resourceId, const std::string& slotDate,
                                           int fromIndex, int toIndex, const AuthContext& ctx) {
    if (ctx.role != model::Role::ADMIN) {
        return Result<int>::fail(kForbidden, "权限不足，需要管理员角色");
    }
    DbPool::Guard guard = pool_.acquire(3000);
    if (!guard) {
        return Result<int>::fail(kDbError, "数据库访问失败：获取数据库连接超时");
    }
    DbConn& conn = *guard;

    Transaction tx(conn);
    if (!tx.active()) {
        return Result<int>::fail(kDbError, "数据库访问失败：事务开启失败");
    }
    auto r = dispatchOffers(conn, resourceId, slotDate, fromIndex, toIndex);
    if (!r.ok()) {
        tx.rollback();
        return r;
    }
    auto committed = tx.commit();
    if (!committed.ok()) {
        return Result<int>::fail(committed.code, committed.msg);
    }
    LOG_AUDIT(requestId(), ctx.userId, model::toStr(ctx.role), kActDispatch, kOk, resourceId);
    return r;
}

Result<int64_t> WaitlistService::cancelAllForResource(DbConn& conn, int64_t resourceId,
                                                      const std::string& reason) {
    auto r = wrepo_.cancelAllForResource(conn, resourceId, reason);
    if (!r.ok()) {
        return r;
    }
    if (r.data > 0) {
        LOG_INFO("资源 {} 不可预约，作废候补 {} 条（{}）", resourceId, r.data, reason);
    }
    return r;
}

} // namespace rf
