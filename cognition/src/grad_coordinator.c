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
 * Differential entropy reduction (V3.0 §2.5): only Δ_k and its first-order
 * closure (neighbor_depth layers) are verified; unchanged nodes are treated
 * as proven theorems. Current version uses full-node differential scope
 * (node-level increments can be added later via grad_diff.c).
 */

#include "grad_internal.h"
#include "airy_memory.h"
#include "string_compat.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
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
    if (coord->config.neighbor_depth == 0)
        coord->config.neighbor_depth = AIRY_GRAD_NEIGHBOR_DEPTH;

    coord->rounds = NULL;
    coord->rounds_capacity = 0;
    coord->rounds_count = 0;
    coord->active = 0;
    __builtin_memset(&coord->stats, 0, sizeof(airy_grad_stats_t));

    *out_coord = coord;
    AIRY_LOG_INFO("GRAD: coordinator created (max_iterations=%u neighbor_depth=%u)",
                  (unsigned)coord->config.max_iterations, (unsigned)coord->config.neighbor_depth);
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

    /* Differential scope tracking: round 0 verifies everything; later
     * rounds verify only the previous rejection patch's affected_scope
     * (Δ_k and its first-order closure) for differential entropy
     * reduction. */
    char *delta_scope = NULL;
    uint32_t k = 0;
    for (k = 0; k < coord->config.max_iterations; k++) {
        airy_grad_report_t report;
        __builtin_memset(&report, 0, sizeof(report));

        /* Step 2/4: model C four-way verification (differential entropy
         * reduction: only Δ_k and its first-order closure; first round
         * delta_scope=NULL → full verification) */
        {
            char pb[128];
            snprintf(pb, sizeof(pb), "{\"round\":%u,\"scope\":\"%s\"}", (unsigned)k,
                     delta_scope ? delta_scope : "full");
            grad_progress(coord, 0, "grad_verify_start", pb);
        }
        err = airy_grad_verify_scope(plan, &coord->config.budget,
                                     delta_scope ? (const char *const *)&delta_scope : NULL,
                                     delta_scope ? 1 : 0, &report);
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

        if (delta_scope) {
            AIRY_FREE(delta_scope);
            delta_scope = NULL;
        }

        airy_grad_round_t round;
        __builtin_memset(&round, 0, sizeof(round));
        round.iteration = k;
        round.error = report.error;
        snprintf(round.error_desc, sizeof(round.error_desc), "%s",
                 airy_grad_error_str(report.error));
        snprintf(round.affected_node, sizeof(round.affected_node), "%s", report.affected_node);
        round.verified_scope = plan->task_plan_node_count;
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
                    int e04_fail = 0;
                    double conf = 1.0;
#ifdef AIRY_HAS_CJSON
                    cJSON *root = cJSON_Parse(verdict);
                    if (root) {
                        cJSON *v = cJSON_GetObjectItem(root, "verdict");
                        cJSON *c = cJSON_GetObjectItem(root, "confidence");
                        cJSON *e04 = cJSON_GetObjectItem(root, "e04");
                        /* q8a：confidence 类型容错——number 直取；字符串
                         * （LLM 常见 "0.3"）atof；对象/数组/其他非法类型按
                         * 0.0（不信任，θ_B 保护生效）。此前仅 IsNumber 时
                         * 非法类型静默保持 1.0，低置信 reject 被当作高置信
                         * 触发修复循环（或低置信 accept 放行）。 */
                        if (cJSON_IsNumber(c))
                            conf = c->valuedouble;
                        else if (cJSON_IsString(c) && c->valuestring)
                            conf = atof(c->valuestring);
                        else if (c)
                            conf = 0.0;
                        /* 2.1.1.2 修复（GRAD V3.0 §11）：B 终裁置信度不足
                         * （< θ_B）时放弃终裁权，默认采纳模型 C 的硬逻辑
                         * 判定（保守安全策略）。此前只解析 verdict 丢弃
                         * confidence——低置信度 reject 会误触发修复循环
                         * 浪费迭代，低置信度 accept 可能放行有风险计划。
                         * LLM 未提供 confidence 字段时按 1.0 处理（兼容）。 */
                        if (cJSON_IsString(v) && v->valuestring &&
                            strcmp(v->valuestring, "reject") == 0 &&
                            conf >= AIRY_GRAD_ARBITER_MIN_CONF)
                            reject = 1;
                        /* E-04 purpose drift（语义层守门）：invariant_guard
                         * 节点与目标 G 冲突时，即便 verdict 文本为 accept 也
                         * 是硬拒绝信号（目的漂移 > 语境意见），同受 θ_B 保护。 */
                        if (cJSON_IsString(e04) && e04->valuestring &&
                            strcmp(e04->valuestring, "fail") == 0) {
                            e04_fail = 1;
                            if (conf >= AIRY_GRAD_ARBITER_MIN_CONF)
                                reject = 1;
                        }
                        cJSON_Delete(root);
                    } else {
                        /* q8a：B 仲裁 JSON 解析失败（markdown 围栏/前后缀
                         * 文本）不得静默按"采纳 C"收敛——记录 WARN 与降级
                         * 计数，B 的意见被丢弃需可观测。 */
                        AIRY_LOG_WARN("GRAD-B: arbiter verdict JSON parse failed, "
                                      "deferring to Model C (verdict=%.120s)",
                                      verdict ? verdict : "");
                    }
#else
                    (void)conf;
#endif
                    AIRY_FREE(verdict);
                    if (reject) {
                        /* Contextual rejection: overturn C's "verification
                         * passed", reset the convergence flag, and trigger a
                         * fix via a generic patch (conservative). */
                        coord->stats.converged = 0;
                        char patch[512];
                        snprintf(patch, sizeof(patch),
                                 "{\"patch_id\":\"P_%02u\",\"source\":\"B_Model\","
                                 "\"error_code\":\"%s\",\"affected_scope\":[],"
                                 "\"suggestion\":\"recheck task alignment\"}",
                                 (unsigned)k, e04_fail ? "E-04" : "CTX");
                        round.patch_json = AIRY_STRDUP(patch);
                        coord->stats.rejections++;
                        if (!round.patch_json) {
                            err = AIRY_ENOMEM;
                            loop_err = err;
                            break;
                        }

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

            if (delta_scope)
                AIRY_FREE(delta_scope);
            /* 2.1.1.2 修复：E-03 资源超限的 affected_node 为 "aggregate"
             * （非真实节点 ID），此前被当作下一轮 delta_scope 传入
             * grad_compute_closure → 找不到节点 → closure 空 → 退化全量
             * 验证（丧失差异熵减）。"aggregate" 语义即"全量"，直接置
             * NULL 保持全量验证，不做无意义的 scope 空转。 */
            delta_scope =
                (report.affected_node[0] && strcmp(report.affected_node, "all") != 0 &&
                 strcmp(report.affected_node, "aggregate") != 0) ?
                    AIRY_STRDUP(report.affected_node) :
                    NULL;
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

    if (delta_scope)
        AIRY_FREE(delta_scope);

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
