// =============================================================================
// src/RuleRepo.h  —  rule_config 的读取与指纹（无状态，方法首参 DbConn&）
// 对应 ARCH-P3 §2.1 #2 / §1.3 / D-P3-10
//
// 本层只做 SQL 与行 → Rule 的映射；规则表的**写**操作（CRUD）属 T08，
// 届时在本类追加 insert/update/setEnabled/remove，签名沿用现有风格。
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Result.h"
#include "DbPool.h"
#include "RuleTypes.h"

namespace rf::repo {

/** 规则指纹：三项任一变化就重载（§1.3） */
struct RuleFingerprint {
    int64_t     count        = 0;
    std::string maxUpdatedAt;
    int64_t     enabledSum   = 0;

    bool operator==(const RuleFingerprint& o) const {
        return count == o.count && maxUpdatedAt == o.maxUpdatedAt && enabledSum == o.enabledSum;
    }
    bool operator!=(const RuleFingerprint& o) const { return !(*this == o); }
};

class RuleRepo {
public:
    RuleRepo()  = default;
    ~RuleRepo() = default;
    RuleRepo(const RuleRepo&)            = delete;
    RuleRepo& operator=(const RuleRepo&) = delete;

    /**
     * 全量载入规则（按 priority 升序、同优先级按 rule_code 字典序）。
     * 单条 JSON 畸形的行**跳过并记录**（不因一行坏掉整表读不出来）；
     * 跳过的原因通过 skipped 返回，供调用方 WARN。
     */
    Result<std::vector<Rule>> loadAll(DbConn& db, std::vector<std::string>* skipped = nullptr);

    /** 轻量指纹（一条聚合查询，10 行的全表扫描可忽略） */
    Result<RuleFingerprint> fingerprint(DbConn& db);

    Result<std::optional<Rule>> findById(DbConn& db, int64_t id);
    Result<std::optional<Rule>> findByCode(DbConn& db, const std::string& ruleCode);

    // ---- P3 T08：管理端 CRUD ----

    /** 新增规则；rule_code 冲突（uk_rule_code）→ kRuleCodeDup(3025) */
    Result<int64_t> insertRule(DbConn& db, const Rule& r);

    /** 全量更新可改字段（rule_code 不可改）；不存在 → kRuleNotFound(3023) */
    VoidResult updateRule(DbConn& db, int64_t id, const Rule& r);

    /** 只改 enabled / priority（不存在 → 3023） */
    VoidResult setEnabled(DbConn& db, int64_t id, bool enabled);
    VoidResult setPriority(DbConn& db, int64_t id, int priority);

    /** 物理删除（内置保护规则由 Service 层拦）；不存在 → 3023 */
    VoidResult removeRule(DbConn& db, int64_t id);
};

} // namespace rf::repo
