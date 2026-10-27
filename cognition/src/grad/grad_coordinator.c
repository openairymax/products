// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_coordinator.c
 * @brief GRAD coordinator implementation — plan-level critique loop
 *        (differential entropy reduction + incremental patches).
 *
 * Loop timing (GRAD V3.0 §4):
 *   Step 0 goal injection → Step 1 A skeleton generation → Step 2 C four-way
 *   verification → Step 3 A incremental expansion Δ_k → Step 4 C incremental
 *   verification (differential scope) → Step 5 B contextual arbitration →
 *   Step 6 A patch fix → Step 7 convergence stop
 *
 * Differential entropy reduction (V3.0 §2.5): later rounds verify only the
 * Δ_k patch scope and its first-order closure; unchanged nodes are treated
 * as proven theorems. B verdict defects drive the incremental repair scope
 * (C6, 0.1.20 §5.2b.3); closure overrun escalates to a single full redraw
 * tagged repair_mode=full.
 */

#include "grad_internal.h"
#include "airy_memory.h"
#include "string_compat.h"
#include "logging.h"

#include <stdio.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

struct airy_grad_coordinator {
    airy_grad_config_t config;
    airy_grad_stats_t stats;
    airy_grad_round_t *rounds;
    size_t rounds_capacity;
    size_t rounds_count;
    int active;
};

static airy_err_t grad_record_round(airy_grad_coordinator_t *coord, const airy_grad_round_t *round)
{
    if (coord->rounds_count >= coord->rounds_capacity) {
        size_t new_cap = coord->rounds_capacity ? coord->rounds_capacity * 2 : 16;
        airy_grad_round_t *new_arr =
            (airy_grad_round_t *)AIRY_REALLOC(coord->rounds, new_cap * sizeof(airy_grad_round_t));
        if (!new_arr)
            return AIRY_ENOMEM;
        coord->rounds = new_arr;
        coord->rounds_capacity = new_cap;
    }
    airy_grad_round_t *slot = &coord->rounds[coord->rounds_count];
    *slot = *round;
    slot->patch_json = NULL;
    if (round->patch_json) {
        slot->patch_json = AIRY_STRDUP(round->patch_json);
        if (!slot->patch_json)
            return AIRY_ENOMEM;
    }
    coord->rounds_count++;
    return AIRY_SUCCESS;
}

static void grad_free_rounds(airy_grad_coordinator_t *coord)
{
    if (!coord || !coord->rounds)
        return;
    for (size_t i = 0; i < coord->rounds_count; i++) {
        if (coord->rounds[i].patch_json) {
            AIRY_FREE(coord->rounds[i].patch_json);
            coord->rounds[i].patch_json = NULL;
        }
    }
    AIRY_FREE(coord->rounds);
    coord->rounds = NULL;
    coord->rounds_capacity = 0;
    coord->rounds_count = 0;
}

/* 2.3.14 GRAD 决策链可见性：阶段进度转发（config.progress_cb 为空时无开销） */
static void grad_progress(airy_grad_coordinator_t *coord, int level, const char *event,
                          const char *data)
{
    if (coord && coord->config.progress_cb) {
        coord->config.progress_cb(level, event, data, coord->config.progress_user_data);
    }
}

/* E1（0.1.20）：终态计划指纹——FNV-1a 64 over 节点 id/goal 串联（域
 * 分隔字节防拼接歧义）。同计划同指纹，审计回放可比对"放行的到底是
 * 哪份计划"；无计划返回 0。节点 id 经 schema 约束为安全字符集。 */
static uint64_t grad_plan_digest(const airy_task_plan_t *plan)
{
    if (!plan)
        return 0;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *n = plan->task_plan_nodes[i];
        if (!n)
            continue;
        const char *fields[2] = { n->task_node_id, n->task_node_goal };
        for (int f = 0; f < 2; f++) {
            const char *s = fields[f];
            h ^= 0xff;
            h *= 1099511628211ULL;
            while (s && *s) {
                h ^= (unsigned char)*s++;
                h *= 1099511628211ULL;
            }
        }
    }
    return h;
}

/* E1-E5（0.1.20）：驳回影响面事件——B 驳回与 C 失败两条路径共用，
 * 携带缺陷码、scope 规模与前 8 个节点 id（schema 约束安全字符集，
 * 无需转义）。审计可回放"驳回的是哪些节点、修补对准了没有"。 */
static void grad_emit_reject_scope(airy_grad_coordinator_t *coord, uint32_t round,
                                   const char *code, char *const *scope, size_t scope_count)
{
    char buf[384];
    size_t off = (size_t)snprintf(buf, sizeof(buf),
                                  "{\"round\":%u,\"code\":\"%s\",\"scope\":%zu,\"nodes\":[",
                                  (unsigned)round, code ? code : "-", scope_count);
    if (off >= sizeof(buf))
        return;
    size_t shown = scope_count > 8 ? 8 : scope_count;
    for (size_t i = 0; i < shown && off < sizeof(buf); i++) {
        int w = snprintf(buf + off, sizeof(buf) - off, "%s\"%s\"",
                         i ? "," : "", scope[i] ? scope[i] : "");
        if (w <= 0 || off + (size_t)w >= sizeof(buf))
            break;
        off += (size_t)w;
    }
    if (off < sizeof(buf) - 1)
        snprintf(buf + off, sizeof(buf) - off, "]");
    grad_progress(coord, 1, "grad_reject_scope", buf);
}

/* 本轮 patch 来源，决定 s2_plan 产物如何落回计划 */
typedef enum {
    GRAD_PATCH_C = 0, /**< C 判据 patch：整份替换（允许增删节点） */
    GRAD_PATCH_INCR,  /**< B verdict Δk：scope 内合并 + frozen 逐字段校验 */
    GRAD_PATCH_FULL   /**< B 升级整份重绘：整份替换（唯一 full 路径） */
} grad_pmode_t;

/* Δk 修补 scope 所有权：顶层数组 + 元素（可重复调用） */
static void grad_scope_free(char ***scope, size_t *count)
{
    if (!scope || !*scope) {
        if (count)
            *count = 0;
        return;
    }
    for (size_t i = 0; i < *count; i++)
        AIRY_FREE((*scope)[i]);
    AIRY_FREE(*scope);
    *scope = NULL;
    *count = 0;
}

/* 单节点 scope（C 判据 patch 的复验入口） */
static airy_err_t grad_scope_one(const char *id, char ***out_scope, size_t *out_count)
{
    *out_scope = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (!*out_scope)
        return AIRY_ENOMEM;
    (*out_scope)[0] = AIRY_STRDUP(id);
    if (!(*out_scope)[0]) {
        AIRY_FREE(*out_scope);
        *out_scope = NULL;
        return AIRY_ENOMEM;
    }
    *out_count = 1;
    return AIRY_SUCCESS;
}

/* 失败码 → 契约缺陷码串（C 判据 patch 与 A 历史缺陷序列共用，避免
 * 同一映射在两处漂移）。E-00 仅用于枚举外的内部错误兜底。 */
static const char *grad_error_code(airy_grad_error_t e)
{
    switch (e) {
    case AIRY_GRAD_E01_CAUSAL_BREAK:
        return "E-01";
    case AIRY_GRAD_E02_DEADLOCK:
        return "E-02";
    case AIRY_GRAD_E03_RESOURCE_OVER:
        return "E-03";
    case AIRY_GRAD_E05_VERIFY_INSUFFICIENT:
        return "E-05";
    case AIRY_GRAD_E04_GUARD_DRIFT:
        return "E-04";
    default:
        return "E-00";
    }
}

/* 失败路径仲裁来源（A 侧历史 arbiter 字段与统计计数共用） */
typedef enum {
    GRAD_ARB_TAG_NONE = 0,    /**< B 未参与（策略关闭/节点不可定位） */
    GRAD_ARB_TAG_ACCEPT,      /**< B 认可 C 的否定，沿用 C 模板 patch */
    GRAD_ARB_TAG_REJECT,      /**< B 定向驳回，patch 由 B 缺陷派生 */
    GRAD_ARB_TAG_SKIP_BUDGET  /**< 预算不足跳过仲裁 */
} grad_arb_tag_t;

/** @brief 失败路径分流产物（patch/scope 为 OWNER，整体移交调用方）。 */
typedef struct grad_fail_out {
    char *patch;
    char **scope;
    size_t scope_count;
    grad_arb_tag_t tag;
    char arb[32]; /**< A 侧历史 arbiter 字段（accept/reject(...)/skipped_budget） */
} grad_fail_out_t;

static void grad_fo_free(grad_fail_out_t *o)
{
    if (!o)
        return;
    if (o->patch) {
        AIRY_FREE(o->patch);
        o->patch = NULL;
    }
    grad_scope_free(&o->scope, &o->scope_count);
    o->tag = GRAD_ARB_TAG_NONE;
    o->arb[0] = '\0';
}

/* 计划内按 id 查节点（失败节点全文注入的前提；查不到即 B 盲判，须跳过） */
static const airy_task_node_t *grad_node_find(const airy_task_plan_t *plan, const char *id)
{
    if (!plan || !id || !id[0])
        return NULL;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *n = plan->task_plan_nodes[i];
        if (n && n->task_node_id && strcmp(n->task_node_id, id) == 0)
            return n;
    }
    return NULL;
}

/** @brief A/B 共享的轮次历史环（保留最近 GRAD_ARBITER_MAX_HISTORY 条）。 */
typedef struct grad_hist_ring {
    grad_round_hist_t e[GRAD_ARBITER_MAX_HISTORY];
    size_t n;
} grad_hist_ring_t;

static void grad_hist_clr(grad_round_hist_t *h)
{
    if (!h)
        return;
    grad_scope_free(&h->scope, &h->scope_count);
    h->round = 0;
    h->codes[0] = '\0';
}

static void grad_hist_free(grad_hist_ring_t *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->n; i++)
        grad_hist_clr(&r->e[i]);
    r->n = 0;
}

/**
 * @brief 追加一轮 C 失败记录（codes 为 C 缺陷码，scope 为本轮修补范围）。
 *
 * scope 深拷贝：调用方的 delta_scope 仍属下一轮验证，不得移交所有权。
 */
static airy_err_t grad_hist_push(grad_hist_ring_t *r, uint32_t round, airy_grad_error_t code,
                                 const char *const *scope, size_t scope_count)
{
    if (r->n >= GRAD_ARBITER_MAX_HISTORY) {
        grad_hist_clr(&r->e[0]);
        for (size_t i = 1; i < r->n; i++)
            r->e[i - 1] = r->e[i];
        r->n--;
    }
    grad_round_hist_t *slot = &r->e[r->n];
    __builtin_memset(slot, 0, sizeof(*slot));
    slot->round = round;
    snprintf(slot->codes, sizeof(slot->codes), "%s", grad_error_code(code));
    if (scope_count) {
        slot->scope = (char **)AIRY_CALLOC(scope_count, sizeof(char *));
        if (!slot->scope)
            return AIRY_ENOMEM;
        for (size_t i = 0; i < scope_count; i++) {
            slot->scope[i] = AIRY_STRDUP(scope[i] ? scope[i] : "");
            if (!slot->scope[i]) {
                grad_hist_clr(slot);
                return AIRY_ENOMEM;
            }
            slot->scope_count++;
        }
    }
    r->n++;
    return AIRY_SUCCESS;
}

/* 两轮"同一缺陷 + 同一范围"（集合语义，顺序无关） */
static int grad_hist_same(const grad_round_hist_t *a, const grad_round_hist_t *b)
{
    if (strcmp(a->codes, b->codes) != 0)
        return 0;
    if (a->scope_count != b->scope_count)
        return 0;
    for (size_t i = 0; i < a->scope_count; i++) {
        int hit = 0;
        for (size_t j = 0; j < b->scope_count; j++) {
            if (strcmp(a->scope[i], b->scope[j]) == 0) {
                hit = 1;
                break;
            }
        }
        if (!hit)
            return 0;
    }
    return 1;
}

/** @brief J-16 无进展：连续两轮缺陷码序列与修补范围完全相同。 */
static int grad_hist_stall(const grad_hist_ring_t *r)
{
    return r && r->n >= 2 && grad_hist_same(&r->e[r->n - 2], &r->e[r->n - 1]);
}

/*
 * 失败路径 B 定向仲裁（§5.2b.11）：装配权威七字段 → 预算判定 → 调用 B
 * → 契约解析 → 缺陷派生 → patch 分流（B 产物为 OWNER）。
 *
 * 返回 AIRY_SUCCESS 且 out->patch==NULL 表示"按 C 模板 patch 继续"：B 未
 * 参与、B 认可 C 的否定、或 B 自身故障（记 arbiter_internal_error）。
 * 失败路径上 B 只细化"怎么修"，不把 C 的否定翻成成功（fail-closed）。
 */
static airy_err_t grad_fail_arb(airy_grad_coordinator_t *coord, const airy_gccp_goal_t *goal,
                                const airy_task_plan_t *plan, uint32_t k,
                                const airy_grad_report_t *report, grad_hist_ring_t *hist,
                                uint64_t round_start_ns, grad_fail_out_t *out)
{
    const airy_task_node_t *fnode = grad_node_find(plan, report->affected_node);
    if (!fnode)
        return AIRY_SUCCESS; /* 节点不可定位 → 不给 B 盲判的机会 */

    int64_t used_ms = (int64_t)((airy_time_monotonic_ns() - round_start_ns) / 1000000ULL);
    if (coord->config.budget.time_ms > 0 && used_ms >= coord->config.budget.time_ms) {
        coord->stats.arbiter_skipped_budget++;
        out->tag = GRAD_ARB_TAG_SKIP_BUDGET;
        snprintf(out->arb, sizeof(out->arb), "skipped_budget");
        grad_progress(coord, 0, "grad_arbiter_failure_skip", "{\"reason\":\"budget\"}");
        return AIRY_SUCCESS;
    }

    char *goal_json = grad_goal_to_json(goal, NULL);
    char *c_report = NULL;
    char *fnode_json = NULL;
    char *nbr_json = NULL;
    char *hist_json = NULL;
    airy_err_t rc = goal_json ? AIRY_SUCCESS : AIRY_ENOMEM;
    if (rc == AIRY_SUCCESS)
        rc = grad_c_report_json(report, &c_report);
    if (rc == AIRY_SUCCESS)
        rc = grad_node_to_json(fnode, &fnode_json);
    if (rc == AIRY_SUCCESS)
        rc = grad_neighbors_to_json(plan, report->affected_node, &nbr_json);
    if (rc == AIRY_SUCCESS)
        rc = grad_hist_to_json(hist->e, hist->n, &hist_json);
    if (rc != AIRY_SUCCESS) {
        AIRY_LOG_WARN("GRAD-B: failure prompt assembly failed (err=%d)", (int)rc);
        AIRY_FREE(goal_json);
        AIRY_FREE(c_report);
        AIRY_FREE(fnode_json);
        AIRY_FREE(nbr_json);
        AIRY_FREE(hist_json);
        return rc;
    }

    grad_arb_fail_in_t in;
    __builtin_memset(&in, 0, sizeof(in));
    in.goal_json = goal_json;
    in.c_report_json = c_report;
    in.failed_node_json = fnode_json;
    in.neighbors_json = nbr_json;
    in.round = k;
    in.round_history_json = hist_json;
    /* §5.2b.9：token 近似统一走 C12 est（ceil(utf8_bytes/3)）；
     * 真实预算上界由 n_ctx_slot 派生，此处仅供 A 历史与统计。 */
    in.token_budget = (int64_t)(grad_ctxb_est(goal_json) + grad_ctxb_est(c_report) +
                                grad_ctxb_est(fnode_json) + grad_ctxb_est(nbr_json) +
                                grad_ctxb_est(hist_json));

    char *verdict = NULL;
    coord->stats.arbiter_failure_calls++;
    airy_err_t aerr = coord->config.s1_failure(&in, &verdict, coord->config.s1_user_data);
    AIRY_FREE(goal_json);
    AIRY_FREE(c_report);
    AIRY_FREE(fnode_json);
    AIRY_FREE(nbr_json);
    AIRY_FREE(hist_json);

    grad_arb_fail_t f;
    __builtin_memset(&f, 0, sizeof(f));
    int parsed = verdict && aerr == AIRY_SUCCESS &&
                 grad_arb_fail_parse(verdict, plan, &f) == AIRY_SUCCESS;
    AIRY_FREE(verdict);
    if (!parsed) {
        /* B 超时/非 JSON/契约违例：回退 C 模板 patch，不阻塞修复环 */
        coord->stats.arbiter_internal_error++;
        AIRY_LOG_WARN("GRAD-B: failure arbitration unusable (err=%d), C patch kept", (int)aerr);
        grad_progress(coord, 0, "grad_arbiter_failure_error", "{\"fallback\":\"c_patch\"}");
        return AIRY_SUCCESS;
    }

    if (f.kind == GRAD_VD_ACCEPT) {
        snprintf(out->arb, sizeof(out->arb), "accept");
        grad_arb_fail_free(&f);
        return AIRY_SUCCESS;
    }

    out->tag = GRAD_ARB_TAG_REJECT;
    snprintf(out->arb, sizeof(out->arb), "%s",
             strcmp(f.defect_class, "SEM") == 0 ? "reject(semantic)" : "reject(structural)");

    grad_defect_t *defects = NULL;
    size_t dcount = 0;
    char **scope = NULL;
    size_t scount = 0;
    size_t closure = 0;

    int salvage = f.salvageable;
    if (salvage) {
        rc = grad_arb_fail_defects(&f, &defects, &dcount);
        if (rc == AIRY_SUCCESS)
            rc = grad_patch_scope(plan, defects, dcount, &scope, &scount, &closure);
        if (rc == AIRY_SUCCESS)
            rc = grad_patch_build(k, defects, dcount, (const char *const *)scope, scount,
                                  &out->patch);
    } else {
        rc = AIRY_EINVAL; /* 不可增量：直接整份重绘 */
    }
    grad_arb_fail_free(&f);
    AIRY_FREE(defects);

    if (rc == AIRY_SUCCESS) {
        out->scope = scope;
        out->scope_count = scount;
        coord->stats.arbiter_failure_override++;
        AIRY_LOG_INFO("GRAD-B: failure repair localized (scope=%zu closure=%zu)", scount, closure);
        return AIRY_SUCCESS;
    }

    grad_scope_free(&scope, &scount);
    if (!salvage) {
        AIRY_LOG_WARN("GRAD-B: failure repair not salvageable, full redraw");
    } else if (rc != AIRY_E2BIG) {
        /* salvageable 但 scope 不可定位（含空 scope）：P0-7 负向用例③，
         * 判 patch_invalid 并回退全量重绘，不做无落点的重试。E2BIG 是
         * 闭包超界的正常升级路径，不属 patch_invalid。 */
        AIRY_LOG_WARN("GRAD-B: failure patch_invalid (err=%d), full redraw", (int)rc);
        grad_progress(coord, 0, "grad_patch_invalid", "{\"action\":\"full_redraw\"}");
    }
    rc = grad_patch_full(plan, k, NULL, 0, &out->patch);
    if (rc != AIRY_SUCCESS)
        return rc;
    coord->stats.arbiter_failure_override++;
    return AIRY_SUCCESS;
}

/**
 * @brief C 失败分流：B 定向仲裁（可跳过）→ 回退 C 判据模板 patch。
 *
 * out->patch==NULL 仅在 B 未参与/认可 C/自身故障时出现，此时本函数补齐
 * C 模板 patch 与单节点复验 scope；B 产物路径的 scope 由 B 缺陷闭包决定。
 */
static airy_err_t grad_fail_stage(airy_grad_coordinator_t *coord, const airy_gccp_goal_t *goal,
                                  const airy_task_plan_t *plan, uint32_t k,
                                  const airy_grad_report_t *report, grad_hist_ring_t *hist,
                                  uint64_t round_start_ns, grad_fail_out_t *out)
{
    __builtin_memset(out, 0, sizeof(*out));
    out->tag = GRAD_ARB_TAG_NONE;
    snprintf(out->arb, sizeof(out->arb), "accept");

    if (coord->config.s1_failure &&
        grad_arb_fail_wanted(coord->config.arbiter_on_failure, report->error, plan,
                             report->affected_node)) {
        airy_err_t rc = grad_fail_arb(coord, goal, plan, k, report, hist, round_start_ns, out);
        if (rc != AIRY_SUCCESS)
            return rc;
        if (out->patch)
            return AIRY_SUCCESS;
    }

    char *patch = NULL;
    airy_err_t rc = airy_grad_build_patch(report, k, &patch);
    if (rc != AIRY_SUCCESS || !patch)
        return rc != AIRY_SUCCESS ? rc : AIRY_ESERVICE;
    out->patch = patch;
    /* E-03 的 affected_node 为 "aggregate"（非真实节点 ID），"all"/"aggregate"
     * 语义即"全量"：置空 scope 保持全量验证，不做无意义的 scope 空转。 */
    if (report->affected_node[0] && strcmp(report->affected_node, "all") != 0 &&
        strcmp(report->affected_node, "aggregate") != 0) {
        rc = grad_scope_one(report->affected_node, &out->scope, &out->scope_count);
        if (rc != AIRY_SUCCESS) {
            grad_fo_free(out);
            return rc;
        }
    }
    return AIRY_SUCCESS;
}

/*
 * B 终裁阶段：严格解析裁决契约（§5.2b.4）→ θ_B 折让 → Δk 修补分流。
 *
 * 契约违例（arbiter_parse_error）、reject 但 defects 缺失、defect 节点
 * 不可定位（G55 patch_invalid）一律降级"采纳 C 判定"，不得把非法输出当
 * accept；闭包超界（E2BIG）与 force_full 走整份重绘（唯一 full 路径）。
 *
 * @return AIRY_SUCCESS：裁定完成（*out_reject=1 时 patch/scope 为 OWNER）；
 *         AIRY_ENOMEM / AIRY_EUNKNOWN：本轮中止
 */
static airy_err_t grad_arb_stage(airy_grad_coordinator_t *coord, const airy_task_plan_t *plan,
                                 uint32_t k, const char *verdict_json, int force_full,
                                 int *out_reject, char **out_patch, char ***out_scope,
                                 size_t *out_scope_count)
{
    *out_reject = 0;
    *out_patch = NULL;
    *out_scope = NULL;
    *out_scope_count = 0;

    grad_verdict_t v;
    __builtin_memset(&v, 0, sizeof(v));
    airy_err_t rc = grad_verdict_parse(verdict_json, plan, &v);
    if (rc != AIRY_SUCCESS) {
        AIRY_LOG_WARN("GRAD-B: arbiter_parse_error (err=%d), deferring to Model C", (int)rc);
        grad_progress(coord, 0, "grad_arbiter_parse_error", "{\"fallback\":\"accept_c_verdict\"}");
        return AIRY_SUCCESS;
    }

    int objection = (v.kind == GRAD_VD_REJECT || v.e04 == GRAD_E04_FAIL);
    if (!objection || v.confidence < AIRY_GRAD_ARBITER_MIN_CONF) {
        if (objection)
            AIRY_LOG_INFO("GRAD-B: low-confidence objection ignored (conf=%.2f)", v.confidence);
        grad_verdict_free(&v);
        return AIRY_SUCCESS;
    }

    /* 决策④（C13）：opinion 不得丢弃——缺陷条目缺 suggestion 时以
     * opinion 补齐，否则 A 侧只拿到空建议（等于丢失 B 的定位结论）。 */
    for (size_t i = 0; i < v.defect_count; i++) {
        if (!v.defects[i].suggestion[0])
            grad_copy_fld(v.defects[i].suggestion, sizeof(v.defects[i].suggestion), v.opinion);
    }

    int full = force_full;
    char **scope = NULL;
    size_t scope_count = 0;
    if (!full) {
        size_t closure = 0;
        rc = grad_patch_scope(plan, v.defects, v.defect_count, &scope, &scope_count, &closure);
        if (rc == AIRY_E2BIG) {
            full = 1;
        } else if (rc != AIRY_SUCCESS) {
            AIRY_LOG_WARN("GRAD-B: patch_invalid (err=%d defects=%zu), accepting C verdict",
                          (int)rc, v.defect_count);
            grad_progress(coord, 0, "grad_patch_invalid", "{\"fallback\":\"accept_c_verdict\"}");
            grad_verdict_free(&v);
            return AIRY_SUCCESS;
        }
    }

    if (full) {
        AIRY_LOG_WARN("GRAD-B: repair escalated to full redraw (reason=%s)",
                      force_full ? "patch_inconsistent" : "closure_too_large");
        rc = grad_patch_full(plan, k, v.defects, v.defect_count, out_patch);
    } else {
        rc = grad_patch_build(k, v.defects, v.defect_count, (const char *const *)scope,
                              scope_count, out_patch);
    }
    grad_verdict_free(&v);

    if (rc != AIRY_SUCCESS) {
        grad_scope_free(&scope, &scope_count);
        return rc;
    }
    *out_reject = 1;
    *out_scope = scope;
    *out_scope_count = scope_count;
    return AIRY_SUCCESS;
}

airy_err_t airy_grad_coordinator_create(const airy_grad_config_t *config,
                                        airy_grad_coordinator_t **out_coord)
{
    if (!out_coord)
        return AIRY_EINVAL;
    *out_coord = NULL;

    if (!config || !config->s2_plan) {
        AIRY_LOG_ERROR("GRAD: create with missing s2_plan callback");
        return AIRY_EINVAL;
    }

    airy_grad_coordinator_t *coord =
        (airy_grad_coordinator_t *)AIRY_CALLOC(1, sizeof(airy_grad_coordinator_t));
    if (!coord)
        return AIRY_ENOMEM;

    coord->config = *config;
    if (coord->config.max_iterations == 0)
        coord->config.max_iterations = AIRY_GRAD_MAX_ITERATIONS;

    coord->rounds = NULL;
    coord->rounds_capacity = 0;
    coord->rounds_count = 0;
    coord->active = 0;
    __builtin_memset(&coord->stats, 0, sizeof(airy_grad_stats_t));

    *out_coord = coord;
    AIRY_LOG_INFO("GRAD: coordinator created (max_iterations=%u)",
                  (unsigned)coord->config.max_iterations);
    return AIRY_SUCCESS;
}

void airy_grad_coordinator_destroy(airy_grad_coordinator_t *coord)
{
    if (!coord)
        return;
    grad_free_rounds(coord);
    AIRY_FREE(coord);
}

airy_err_t airy_grad_coordinator_execute(airy_grad_coordinator_t *coord,
                                         const airy_gccp_goal_t *goal,
                                         const airy_task_plan_t *seed_plan,
                                         airy_task_plan_t **out_plan, airy_grad_stats_t *out_stats)
{
    if (!coord || !out_plan)
        return AIRY_EINVAL;
    *out_plan = NULL;

    if (coord->active) {
        AIRY_LOG_ERROR("GRAD: coordinator already active (reentrancy not allowed)");
        return AIRY_EBUSY;
    }
    coord->active = 1;
    uint64_t start_ns = airy_time_monotonic_ns();

    grad_free_rounds(coord);
    __builtin_memset(&coord->stats, 0, sizeof(airy_grad_stats_t));

    /* Round 0 plan: prefer the caller-provided seed_plan (BORROW, not
     * freed); when absent, let model A generate a skeleton. */
    airy_task_plan_t *plan = NULL;
    int plan_owned = 0;
    airy_err_t err = AIRY_SUCCESS;
    /* q8a：记录循环提前 break 的根因（服务错误/OOM/JSON 解析 vs 迭代耗尽）。
     * 此前 build_patch/s2_plan 失败统一归因为 ETIMEDOUT，调用方无法区分
     * "真未收敛"与"服务故障"，只能一律转人工评审而无法重试/降级。 */
    airy_err_t loop_err = AIRY_SUCCESS;
    if (seed_plan) {
        plan = (airy_task_plan_t *)seed_plan; /* BORROW */
    } else {

        err = coord->config.s2_plan(goal, NULL, &plan, coord->config.s2_user_data);
        if (err != AIRY_SUCCESS || !plan) {
            AIRY_LOG_ERROR("GRAD: S2 skeleton generation failed (err=%d)", (int)err);
            coord->active = 0;
            return err ? err : AIRY_ESERVICE;
        }
        plan_owned = 1;
    }

    AIRY_LOG_INFO("GRAD: initial plan ready (nodes=%zu owned=%d)",
                  plan ? plan->task_plan_node_count : 0, plan_owned);

    /* 2.3.14 决策链可见性：计划就绪（seed 或 S2 骨架生成完成） */
    {
        char pb[96];
        snprintf(pb, sizeof(pb), "{\"nodes\":%zu,\"owned\":%d}",
                 plan ? plan->task_plan_node_count : 0, plan_owned);
        grad_progress(coord, 0, "grad_s2_done", pb);
    }

    /* Δ_k differential scope: round 0 verifies the whole plan; later rounds
     * verify only the previous rejection patch's affected_scope (Δ_k) plus
     * its first-order closure, resolved inside the verifier (F8/F9). */
    char **delta_scope = NULL;
    size_t delta_scope_count = 0;
    /* patch_inconsistent 后的下一轮：跳过增量 scope，强制整份重绘 */
    int force_full = 0;
    /* C13 决策③：A/B 共享的失败轮次环（仅 C 失败轮入环，J-16 依据末两条） */
    grad_hist_ring_t hist;
    __builtin_memset(&hist, 0, sizeof(hist));
    uint32_t k = 0;
    for (k = 0; k < coord->config.max_iterations; k++) {
        grad_pmode_t pmode = GRAD_PATCH_C;
        /* 本轮起始时刻：失败路径 B 预算判定（arbiter_skipped_budget）基准 */
        uint64_t round_start_ns = airy_time_monotonic_ns();
        int no_prog = 0;
        airy_grad_report_t report;
        __builtin_memset(&report, 0, sizeof(report));

        /* Step 2/4: model C four-way verification (differential entropy
         * reduction: only Δ_k and its first-order closure; first round
         * delta_scope_count=0 → full verification) */
        {
            char pb[128];
            snprintf(pb, sizeof(pb), "{\"round\":%u,\"scope_nodes\":%zu}", (unsigned)k,
                     delta_scope_count);
            grad_progress(coord, 0, "grad_verify_start", pb);
        }
        /* C5（0.1.20）：透传 goal，启用确定性 E-04 purpose-drift 守卫
         * （guard 节点 outputs 与 goal 关键词实体交集判定）。 */
        err = airy_grad_verify_scope_g(plan, &coord->config.budget, goal,
                                       (const char *const *)delta_scope, delta_scope_count,
                                       &report);
        if (err != AIRY_SUCCESS && err != AIRY_EINVAL) {
            AIRY_LOG_WARN("GRAD: verify failed (err=%d), aborting loop", (int)err);
            loop_err = err;
            break;
        }
        {
            char pb[128];
            snprintf(pb, sizeof(pb), "{\"round\":%u,\"error\":\"%s\",\"node\":\"%s\"}",
                     (unsigned)k, airy_grad_error_str(report.error),
                     report.affected_node[0] ? report.affected_node : "-");
            grad_progress(coord, 0, "grad_verify_done", pb);
        }

        /* scope 复用窗口关闭：本轮验证已消费，下一轮 scope 由本轮裁定重填 */
        grad_scope_free(&delta_scope, &delta_scope_count);

        airy_grad_round_t round;
        __builtin_memset(&round, 0, sizeof(round));
        round.iteration = k;
        round.error = report.error;
        snprintf(round.error_desc, sizeof(round.error_desc), "%s",
                 airy_grad_error_str(report.error));
        snprintf(round.affected_node, sizeof(round.affected_node), "%s", report.affected_node);
        round.verified_scope = report.causal_checked;
        round.total_nodes = plan->task_plan_node_count;
        round.patch_json = NULL;

        switch (report.error) {
        case AIRY_GRAD_OK:
            coord->stats.converged = 1;
            break;
        case AIRY_GRAD_E01_CAUSAL_BREAK:
            coord->stats.e01_count++;
            coord->stats.rejections++;
            break;
        case AIRY_GRAD_E02_DEADLOCK:
            coord->stats.e02_count++;
            coord->stats.rejections++;
            break;
        case AIRY_GRAD_E03_RESOURCE_OVER:
            coord->stats.e03_count++;
            coord->stats.rejections++;
            break;
        case AIRY_GRAD_E05_VERIFY_INSUFFICIENT:
            coord->stats.e05_count++;
            coord->stats.rejections++;
            break;
        case AIRY_GRAD_E04_GUARD_DRIFT:
            coord->stats.e04_count++;
            coord->stats.rejections++;
            break;
        default:
            break;
        }

        if (report.error == AIRY_GRAD_OK) {

            if (coord->config.s1_arbiter) {
                char *verdict = NULL;
                grad_progress(coord, 0, "grad_arbiter_start", "{\"round\":\"final\"}");
                airy_err_t arb_err = coord->config.s1_arbiter(goal, &report, plan, &verdict,
                                                              coord->config.s1_user_data);
                {
                    char pb[96];
                    snprintf(pb, sizeof(pb), "{\"verdict\":\"%s\"}",
                             (arb_err == AIRY_SUCCESS && verdict) ? "reported" : "accepted");
                    grad_progress(coord, 0, "grad_arbiter_done", pb);
                }
                if (arb_err == AIRY_SUCCESS && verdict) {
                    int reject = 0;
                    /* 契约强校验 + θ_B 折让 + Δk 分流（§5.2b.3/§5.2b.4） */
                    err = grad_arb_stage(coord, plan, k, verdict, force_full, &reject,
                                         &round.patch_json, &delta_scope, &delta_scope_count);
                    AIRY_FREE(verdict);
                    if (err != AIRY_SUCCESS) {
                        loop_err = err;
                        break;
                    }
                    force_full = 0;
                    if (reject) {
                        /* Contextual rejection: overturn C's "verification
                         * passed", reset convergence, repair Δk or full. */
                        coord->stats.converged = 0;
                        coord->stats.rejections++;
                        pmode = delta_scope_count ? GRAD_PATCH_INCR : GRAD_PATCH_FULL;
                        grad_emit_reject_scope(coord, k, "B-REJECT", delta_scope,
                                               delta_scope_count);
                    } else {
                        coord->stats.converged = 1;
                    }
                } else {
                    /* C7 R4：成功路径 B 缺席/故障（ERANGE 预算弃权、ENOMEM、
                     * 服务故障）显式降级。C 的确定性判定仍然成立（B 语义本就
                     * 是上下文复核），但此前静默吞掉——B 故障在事件流不可归因。 */
                    coord->stats.arbiter_unavailable++;
                    char ab[96];
                    snprintf(ab, sizeof(ab), "{\"round\":%u,\"err\":%d}", (unsigned)k,
                             (int)arb_err);
                    grad_progress(coord, 1, "grad_arbiter_unavailable", ab);
                }
            }
            if (coord->stats.converged) {
                /* Record this round (grad_record_round already copied a
                 * patch_json copy), then free the original patch_json to
                 * avoid a leak (B final-reject scenario). */
                grad_record_round(coord, &round);
                if (round.patch_json) {
                    AIRY_FREE(round.patch_json);
                    round.patch_json = NULL;
                }
                break;
            }
            if (!round.patch_json) {

                coord->stats.converged = 1;
                grad_record_round(coord, &round);
                break;
            }
        } else {
            /* C 失败路径（C13）：B 定向仲裁优先，B 未参与/认可 C/自身故障
             * 时回退 C 判据模板 patch；B 的 Δk 产物为 OWNER。 */
            grad_fail_out_t fo;
            err = grad_fail_stage(coord, goal, plan, k, &report, &hist, round_start_ns, &fo);
            if (err != AIRY_SUCCESS) {
                AIRY_LOG_WARN("GRAD: failure stage failed (err=%d)", (int)err);
                loop_err = err;
                break;
            }
            round.patch_json = fo.patch;
            fo.patch = NULL;
            grad_scope_free(&delta_scope, &delta_scope_count);
            delta_scope = fo.scope;
            delta_scope_count = fo.scope_count;
            fo.scope = NULL;
            fo.scope_count = 0;
            pmode = (fo.tag == GRAD_ARB_TAG_REJECT)
                        ? (delta_scope_count ? GRAD_PATCH_INCR : GRAD_PATCH_FULL)
                        : GRAD_PATCH_C;
            if (grad_hist_push(&hist, k, report.error, (const char *const *)delta_scope,
                               delta_scope_count) == AIRY_SUCCESS) {
                if (coord->config.round_note_cb) {
                    grad_round_note_t note;
                    __builtin_memset(&note, 0, sizeof(note));
                    note.round = k;
                    note.codes = grad_error_code(report.error);
                    note.scope = (const char *const *)delta_scope;
                    note.scope_count = delta_scope_count;
                    note.arbiter = fo.arb;
                    (void)coord->config.round_note_cb(coord->config.round_note_user_data, &note);
                }
                no_prog = grad_hist_stall(&hist);
            }
            AIRY_LOG_INFO("GRAD: round %u rejected (%s) scope=%zu repair=%s", (unsigned)k,
                          airy_grad_error_str(report.error), delta_scope_count, fo.arb);
            grad_emit_reject_scope(coord, k, grad_error_code(report.error), delta_scope,
                                   delta_scope_count);
            grad_fo_free(&fo);
            /* 本轮已整份替换计划（C 模板或全量重绘），force_full 已被消费 */
            force_full = 0;
        }

        err = grad_record_round(coord, &round);
        if (err != AIRY_SUCCESS) {
            if (round.patch_json)
                AIRY_FREE(round.patch_json);
            loop_err = err;
            break;
        }

        if (no_prog) {
            /* J-16：连续两轮缺陷码序列与修补范围完全相同 → 修复环已无信息
             * 增益，fail-closed 停止（不发第 3 轮），交上层人工/策略处置。 */
            coord->stats.stopped = 1;
            AIRY_LOG_WARN("GRAD: no progress between consecutive rounds, stop (J-16)");
            grad_progress(coord, 0, "grad_no_progress", "{\"action\":\"stop\"}");
            if (round.patch_json) {
                AIRY_FREE(round.patch_json);
                round.patch_json = NULL;
            }
            break;
        }

        airy_task_plan_t *new_plan = NULL;
        err = coord->config.s2_plan(goal, round.patch_json, &new_plan, coord->config.s2_user_data);
        if (err != AIRY_SUCCESS || !new_plan) {
            AIRY_LOG_WARN("GRAD: S2 patch application failed (err=%d)", (int)err);

            if (round.patch_json)
                AIRY_FREE(round.patch_json);
            loop_err = err ? err : AIRY_ESERVICE;
            break;
        }
        if (round.patch_json) {
            AIRY_FREE(round.patch_json);
            round.patch_json = NULL;
        }

        if (pmode == GRAD_PATCH_INCR) {
            /* Δk 增量：仅 scope 内整节点替换，frozen 逐字段校验。修补产物
             * 越权/frozen 篡改 → 本轮作废（保留旧计划），下一轮强制整份
             * 重绘（§5.2b.3 唯一 full 升级路径）。 */
            airy_task_plan_t *merged = NULL;
            err = grad_plan_merge(plan, new_plan, (const char *const *)delta_scope,
                                  delta_scope_count, &merged);
            airy_task_plan_free(new_plan);
            if (err != AIRY_SUCCESS) {
                AIRY_LOG_WARN("GRAD: patch_inconsistent (err=%d) at round %u, "
                              "rolling back and forcing full redraw",
                              (int)err, (unsigned)k);
                grad_progress(coord, 0, "grad_patch_inconsistent",
                              "{\"action\":\"rollback_full_redraw\"}");
                grad_scope_free(&delta_scope, &delta_scope_count);
                force_full = 1;
                continue;
            }
            new_plan = merged;
        }

        if (plan_owned)
            airy_task_plan_free(plan);
        plan = new_plan;
        plan_owned = 1;
    }

    if (!coord->stats.converged) {
        if (coord->stats.stopped) {
            AIRY_LOG_ERROR("GRAD: stopped early (no progress, J-16), plan rejected");
        } else if (loop_err != AIRY_SUCCESS) {
            /* 服务错误/OOM 中止（区别于迭代耗尽）：调用方可据此重试/降级，
             * 而非一律当作"未收敛"转人工评审。 */
            AIRY_LOG_ERROR("GRAD: loop aborted by service error (err=%d), plan rejected",
                           (int)loop_err);
        } else {
            AIRY_LOG_ERROR("GRAD: max iterations (%u) reached without convergence, "
                           "plan rejected (manual review required)",
                           (unsigned)coord->config.max_iterations);
        }
    }

    grad_scope_free(&delta_scope, &delta_scope_count);
    grad_hist_free(&hist);

    coord->stats.total_rounds = (uint32_t)coord->rounds_count; /* size_t→u32（C4267） */
    coord->stats.total_time_ns = airy_time_monotonic_ns() - start_ns;
    /* E1（0.1.20）：终态计划指纹（含未收敛轮的最后状态，审计可回放） */
    coord->stats.plan_digest = grad_plan_digest(plan);

    if (out_stats)
        *out_stats = coord->stats;

    coord->active = 0;
    /* Ownership contract (G53, 0.1.20): SUCCESS implies *out_plan != NULL.
     * A converged coordinator-owned plan is emitted as OWNER; a converged
     * untouched seed (first-round pass, never BORROWed out) is emitted as
     * a deep copy so callers face a single success shape and can never
     * alias the seed. A non-converged owned plan is discarded here
     * (fail-closed): an unverified plan must never escape the critique
     * gate — the caller sees a non-SUCCESS code + out_plan=NULL and must
     * route to manual review instead of executing the seed. */
    if (plan_owned && coord->stats.converged) {
        *out_plan = plan;
    } else if (coord->stats.converged && seed_plan) {
        airy_err_t cl_err = airy_task_plan_clone(seed_plan, out_plan);
        if (cl_err != AIRY_SUCCESS) {
            /* Clone failure must not report SUCCESS with a NULL plan —
             * degrade to ENOMEM (manual review path) instead. Stats are
             * re-synced: the earlier snapshot still said converged. */
            AIRY_LOG_ERROR("GRAD: seed pass-through clone failed (err=%d)", (int)cl_err);
            coord->stats.converged = 0;
            if (out_stats)
                *out_stats = coord->stats;
            *out_plan = NULL;
            return cl_err;
        }
    } else {
        if (plan_owned) {
            AIRY_LOG_WARN("GRAD: owned plan not converged, discarded (no plan emitted)");
            airy_task_plan_free(plan);
            plan = NULL;
        }
        *out_plan = NULL;
    }
    AIRY_LOG_INFO("GRAD: execute complete (rounds=%u converged=%d rejections=%u "
                  "time=%llu ms)",
                  (unsigned)coord->stats.total_rounds, coord->stats.converged,
                  (unsigned)coord->stats.rejections,
                  (unsigned long long)(coord->stats.total_time_ns / 1000000ULL));
    {
        char pb[160];
        snprintf(pb, sizeof(pb),
                 "{\"rounds\":%u,\"converged\":%d,\"rejections\":%u,\"review_required\":%s}",
                 (unsigned)coord->stats.total_rounds, coord->stats.converged,
                 (unsigned)coord->stats.rejections,
                 coord->stats.converged ? "false" : "true");
        grad_progress(coord, 0, "grad_done", pb);
    }

    return coord->stats.converged
               ? AIRY_SUCCESS
               : (loop_err != AIRY_SUCCESS ? loop_err : AIRY_ETIMEDOUT);
}

airy_err_t airy_grad_coordinator_get_stats(const airy_grad_coordinator_t *coord,
                                           airy_grad_stats_t *out_stats)
{
    if (!coord || !out_stats)
        return AIRY_EINVAL;
    *out_stats = coord->stats;
    return AIRY_SUCCESS;
}

airy_err_t airy_grad_round_to_json(const airy_grad_round_t *round, char **out_json)
{
    if (!round || !out_json)
        return AIRY_EINVAL;
    *out_json = NULL;

#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return AIRY_ENOMEM;
    cJSON_AddNumberToObject(root, "iteration", (double)round->iteration);
    cJSON_AddNumberToObject(root, "error", (double)round->error);
    cJSON_AddStringToObject(root, "error_desc", round->error_desc);
    cJSON_AddStringToObject(root, "affected_node", round->affected_node);
    cJSON_AddNumberToObject(root, "verified_scope", (double)round->verified_scope);
    cJSON_AddNumberToObject(root, "total_nodes", (double)round->total_nodes);
    if (round->patch_json)
        cJSON_AddStringToObject(root, "patch", round->patch_json);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return AIRY_ENOMEM;
    *out_json = json;
    return AIRY_SUCCESS;
#else
    size_t cap = 512 + (round->patch_json ? strlen(round->patch_json) : 0);
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return AIRY_ENOMEM;
    int n = snprintf(buf, cap,
                     "{\"iteration\":%u,\"error\":%d,\"error_desc\":\"%s\","
                     "\"affected_node\":\"%s\",\"verified_scope\":%u,"
                     "\"total_nodes\":%u%s%s}",
                     (unsigned)round->iteration, (int)round->error, round->error_desc,
                     round->affected_node, (unsigned)round->verified_scope,
                     (unsigned)round->total_nodes, round->patch_json ? ",\"patch\":" : "",
                     round->patch_json ? round->patch_json : "");
    if (n <= 0 || (size_t)n >= cap) {
        AIRY_FREE(buf);
        return AIRY_EUNKNOWN;
    }
    *out_json = buf;
    return AIRY_SUCCESS;
#endif
}

airy_err_t airy_grad_build_patch(const airy_grad_report_t *report, uint32_t iteration,
                                 char **out_patch)
{
    if (!report || !out_patch)
        return AIRY_EINVAL;
    *out_patch = NULL;

    const char *code = grad_error_code(report->error);

    const char *suggestion = "review and fix the plan";
    switch (report->error) {
    case AIRY_GRAD_E01_CAUSAL_BREAK:
        suggestion = "insert a producer node or fix the input signature";
        break;
    case AIRY_GRAD_E02_DEADLOCK:
        suggestion = "break the dependency cycle / reorder steps";
        break;
    case AIRY_GRAD_E03_RESOURCE_OVER:
        suggestion = "reduce cost or raise budget";
        break;
    case AIRY_GRAD_E05_VERIFY_INSUFFICIENT:
        suggestion = "fill inputs/outputs/cost metadata for every node";
        break;
    case AIRY_GRAD_E04_GUARD_DRIFT:
        suggestion = "realign guard node outputs with the goal entities";
        break;
    default:
        break;
    }

    size_t cap = 768;
    char *patch = (char *)AIRY_MALLOC(cap);
    if (!patch)
        return AIRY_ENOMEM;

    int n = snprintf(patch, cap,
                     "{\"patch_id\":\"P_%02u\",\"source\":\"C_Model\","
                     "\"error_code\":\"%s\",\"affected_scope\":[\"%s\"],"
                     "\"missing_artifact\":\"%s\",\"suggestion\":\"%s\"}",
                     (unsigned)iteration, code,
                     report->affected_node[0] ? report->affected_node : "all",
                     report->missing_artifact[0] ? report->missing_artifact : "", suggestion);
    if (n <= 0 || (size_t)n >= cap) {
        AIRY_FREE(patch);
        return AIRY_EUNKNOWN;
    }

    *out_patch = patch;
    return AIRY_SUCCESS;
}
