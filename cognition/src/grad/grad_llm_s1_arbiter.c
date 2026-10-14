// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_llm_s1_arbiter.c
 * @brief GRAD 模型 B（t1-f）情境仲裁域
 *
 * 目标 G + 模型 C 验证报告 → LLM 最终裁决（accept/reject + E-04 目标漂移
 * 评估）。LLM 不可用时 defer to C（确定性硬逻辑，保守策略）。
 */

#include "grad_internal.h"

static const char GRAD_S1_ARBITER_SYSTEM_PROMPT[] =
    "You are the contextual arbiter (Model B) in a goal-anchored planning system. "
    "A deterministic verifier (Model C) has inspected a task plan DAG. "
    "Review the plan against the confirmed goal and the verification report.\n"
    "Reply with ONLY a JSON object (no markdown fences):\n"
    "{\"verdict\":\"accept|reject\",\"confidence\":0.0-1.0,\"opinion\":\"...\","
    "\"e04\":\"pass|fail\"}\n"
    "Use 'accept' only if the plan aligns with the goal despite any minor critique. "
    "Use 'reject' if the plan cannot reach the goal.\n"
    "E-04 purpose drift: when the plan lists invariant-guard nodes, evaluate "
    "whether each guard node's goal aligns with the confirmed goal G; set "
    "\"e04\":\"fail\" when any guard node risks breaking G's core invariant. "
    "When there are no guard nodes, set \"e04\":\"pass\".";

/**
 * @brief Collect invariant-guard node scope (E-04 purpose-drift evaluation).
 *
 * Builds "id (goal), id (goal)" from all nodes with task_node_invariant_guard
 * set, so model B can evaluate whether each guard node aligns with goal G.
 * Returns NULL when the plan has no guard nodes (E-04 trivially passes).
 */
char *grad_collect_guards(const airy_task_plan_t *plan)
{
    if (!plan || plan->task_plan_node_count == 0)
        return NULL;
    size_t cap = 256;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return NULL;
    buf[0] = '\0';
    size_t off = 0;
    size_t count = 0;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        airy_task_node_t *n = plan->task_plan_nodes[i];
        if (!n || !n->task_node_invariant_guard)
            continue;
        size_t need = (n->task_node_id ? strlen(n->task_node_id) : 0) +
                      (n->task_node_goal ? strlen(n->task_node_goal) : 0) + 32;
        if (off + need >= cap) {
            size_t new_cap = cap * 2 + need;
            char *nb = (char *)AIRY_REALLOC(buf, new_cap);
            if (!nb) {
                AIRY_FREE(buf);
                return NULL;
            }
            buf = nb;
            cap = new_cap;
        }
        int w = snprintf(buf + off, cap - off, "%s%s (%s)", count ? ", " : "",
                         n->task_node_id ? n->task_node_id : "?",
                         n->task_node_goal ? n->task_node_goal : "");
        if (w <= 0 || off + (size_t)w >= cap)
            break;
        off += (size_t)w;
        count++;
    }
    if (count == 0) {
        AIRY_FREE(buf);
        return NULL;
    }
    return buf;
}

airy_err_t grad_llm_s1_arbiter(const airy_gccp_goal_t *goal, const airy_grad_report_t *report,
                               const airy_task_plan_t *plan, char **out_verdict, void *user_data)
{
    if (!out_verdict)
        return AIRY_EINVAL;
    *out_verdict = NULL;

    grad_llm_ctx_t *ctx = (grad_llm_ctx_t *)user_data;
    if (!ctx)
        return AIRY_EINVAL;
    if (!report || !plan)
        return AIRY_EINVAL;

    char *goal_json = grad_goal_to_json(goal, ctx);
    char *report_json = NULL;
    int report_json_owned = 0;
    if (airy_grad_report_to_json(report, &report_json) != AIRY_SUCCESS || !report_json) {
        /* 序列化失败（OOM）时回退静态空对象：report_json 仅用于构建 prompt
         * 与落盘，空对象不影响 B 仲裁正确性。此前 STRDUP("{}") 失败会留下
         * NULL，后续 strlen(NULL) 崩溃。 */
        report_json = (char *)"{}";
    } else {
        report_json_owned = 1;
    }

    /* Persist model C's verification report (c_verify/round_N.json, same
     * round as b_arbiter, keeping the full decision chain: verify →
     * arbitrate → fix traceability) */
    if (ctx->workspace_root && ctx->plan_id) {
        char *c_dir = grad_ws_dir(ctx);
        if (c_dir) {
            char c_fname[64];
            snprintf(c_fname, sizeof(c_fname), "round_%u.json", (unsigned)ctx->round_index);
            grad_ws_write_file(c_dir, "c_verify", c_fname, report_json, strlen(report_json));
            grad_llm_trace_append(ctx, "c_verify", report_json);
            AIRY_FREE(c_dir);
        }
    }

    /* E-04 purpose-drift scope: guard node list (NULL when none) */
    char *guards = grad_collect_guards(plan);

    size_t cap = (goal_json ? strlen(goal_json) : 16) + strlen(report_json) + 512;
    if (guards)
        cap += strlen(guards) + 64;
    char *prompt = (char *)AIRY_MALLOC(cap);
    if (!prompt) {
        if (goal_json)
            AIRY_FREE(goal_json);
        if (report_json_owned)
            AIRY_FREE(report_json);
        if (guards)
            AIRY_FREE(guards);
        return AIRY_ENOMEM;
    }
    if (guards) {
        snprintf(prompt, cap,
                 "Confirmed goal (G): %s\n\n"
                 "Verification report: %s\n\n"
                 "Plan has %zu nodes.\n"
                 "Invariant-guard nodes (E-04 purpose-drift scope): %s\n\n"
                 "Render your contextual verdict, explicitly including e04 pass|fail.",
                 goal_json ? goal_json : "null", report_json, plan->task_plan_node_count,
                 guards);
        AIRY_FREE(guards);
    } else {
        snprintf(prompt, cap,
                 "Confirmed goal (G): %s\n\n"
                 "Verification report: %s\n\n"
                 "Plan has %zu nodes. Render your contextual verdict.",
                 goal_json ? goal_json : "null", report_json, plan->task_plan_node_count);
    }
    if (goal_json)
        AIRY_FREE(goal_json);
    if (report_json_owned)
        AIRY_FREE(report_json);

    char *verdict = NULL;
    size_t verdict_len = 0;
    airy_err_t err = grad_llm_call(ctx, GRAD_S1_ARBITER_SYSTEM_PROMPT, prompt, ctx->s1_verify_model,
                                   &verdict, &verdict_len);
    AIRY_FREE(prompt);

    if (err != AIRY_SUCCESS || !verdict) {
        AIRY_LOG_WARN("GRAD-B: arbiter LLM unavailable (err=%d), deferring to Model C", (int)err);
        return AIRY_SUCCESS;
    }

    if (ctx->workspace_root && ctx->plan_id) {
        grad_llm_trace_append(ctx, "b_arbiter", verdict);
        char *dir = grad_ws_dir(ctx);
        if (dir) {
            char fname[64];
            /* P13：与 c_verify 共用当前 round_index（round 号由 S2 推进），
             * 移除此处的自增——verify → arbitrate 同轮文件号一致。 */
            snprintf(fname, sizeof(fname), "round_%u.json", (unsigned)ctx->round_index);
            grad_ws_write_file(dir, "b_arbiter", fname, verdict, verdict_len);
            AIRY_FREE(dir);
        }
    }

    *out_verdict = verdict;
    return AIRY_SUCCESS;
}
