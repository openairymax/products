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
    uint32_t k = 0;
    for (k = 0; k < coord->config.max_iterations; k++) {
        grad_pmode_t pmode = GRAD_PATCH_C;
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
                    } else {
                        coord->stats.converged = 1;
                    }
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

            char *patch = NULL;
            err = airy_grad_build_patch(&report, k, &patch);
            if (err != AIRY_SUCCESS || !patch) {
                AIRY_LOG_WARN("GRAD: build_patch failed (err=%d)", (int)err);
                loop_err = err ? err : AIRY_ESERVICE;
                break;
            }
            round.patch_json = patch;

            grad_scope_free(&delta_scope, &delta_scope_count);
            /* E-03 资源超限的 affected_node 为 "aggregate"（非真实节点
             * ID），"all"/"aggregate" 语义即"全量"：置空 scope 保持全量
             * 验证，不做无意义的 scope 空转。 */
            if (report.affected_node[0] && strcmp(report.affected_node, "all") != 0 &&
                strcmp(report.affected_node, "aggregate") != 0) {
                err = grad_scope_one(report.affected_node, &delta_scope, &delta_scope_count);
                if (err != AIRY_SUCCESS) {
                    AIRY_FREE(round.patch_json);
                    round.patch_json = NULL;
                    loop_err = err;
                    break;
                }
            }
            AIRY_LOG_INFO("GRAD: round %u rejected (%s) scope=%s", (unsigned)k,
                          airy_grad_error_str(report.error),
                          report.affected_node[0] ? report.affected_node : "all");
        }

        err = grad_record_round(coord, &round);
        if (err != AIRY_SUCCESS) {
            if (round.patch_json)
                AIRY_FREE(round.patch_json);
            loop_err = err;
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
        if (loop_err != AIRY_SUCCESS) {
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

    coord->stats.total_rounds = (uint32_t)coord->rounds_count; /* size_t→u32（C4267） */
    coord->stats.total_time_ns = airy_time_monotonic_ns() - start_ns;

    if (out_stats)
        *out_stats = coord->stats;

    coord->active = 0;
    /* Ownership contract: out_plan=NULL when the seed was not modified
     * (caller keeps the seed); only CONVERGED coordinator-owned plans are
     * emitted as OWNER. A non-converged owned plan is discarded here
     * (fail-closed): an unverified plan must never escape the critique
     * gate — the caller sees AIRY_ETIMEDOUT + out_plan=NULL and must
     * route to manual review instead of executing the seed. */
    if (plan_owned && coord->stats.converged) {
        *out_plan = plan;
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

    const char *code = "E-00";
    switch (report->error) {
    case AIRY_GRAD_E01_CAUSAL_BREAK:
        code = "E-01";
        break;
    case AIRY_GRAD_E02_DEADLOCK:
        code = "E-02";
        break;
    case AIRY_GRAD_E03_RESOURCE_OVER:
        code = "E-03";
        break;
    case AIRY_GRAD_E05_VERIFY_INSUFFICIENT:
        code = "E-05";
        break;
    case AIRY_GRAD_E04_GUARD_DRIFT:
        code = "E-04";
        break;
    default:
        break;
    }

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
