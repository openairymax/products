// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_llm_s2_plan.c
 * @brief GRAD 模型 A（t2）计划生成域
 *
 * 目标 G + 拒绝补丁 → LLM DAG 计划 JSON → airy_task_plan_t。
 * LLM 不可用时构造保守单节点降级计划（fallback_ 前缀，engine 拒绝采纳）。
 */

#include "grad_internal.h"

/* 模型 A 系统提示（GRAD 计划生成指令） */
static const char GRAD_S2_SYSTEM_PROMPT[] =
    "You are the generator (Model A) in a goal-anchored DAG planning system. "
    "Produce a task plan as a dependency graph (DAG) targeting the confirmed goal.\n"
    "Reply with ONLY a JSON object (no markdown fences):\n"
    "{\n"
    "  \"nodes\": [\n"
    "    {\"id\":\"S_01\",\"goal\":\"...\",\"role\":\"creator\",\"depends\":[],\n"
    "     \"inputs\":[],\"outputs\":[\"artifact_a\"],\n"
    "     \"cost_time_ms\":1000,\"cost_mem_mb\":64,\"invariant_guard\":false}\n"
    "  ],\n"
    "  \"entry\":[\"S_01\"]\n"
    "}\n"
    "Rules: every node's inputs must be produced by its transitive predecessors "
    "(no dangling dependencies); the graph must be acyclic; total cost is bounded; "
    "nodes that must not break the goal's core invariant set invariant_guard=true.";

/**
 * @brief Serialize goal G into compact JSON text (for prompt injection and goal.json persistence).
 *
 * #11 修复：goal 缺失时不再注入字面 "null"——回退到 ctx 记录的原始用户
 * 输入（original_input），保证 GRAD 模型 A/B 永远有真实目标文本可锚定，
 * 避免 LLM 收到无意义目标后输出漂移计划。
 */
char *grad_goal_to_json(const airy_gccp_goal_t *goal, const grad_llm_ctx_t *ctx)
{
    if (goal) {
        char *json = NULL;
        if (airy_gccp_goal_to_json(goal, &json) == AIRY_SUCCESS && json)
            return json;
    }
    if (ctx && ctx->original_input && ctx->original_input_len > 0) {
        size_t cap = ctx->original_input_len + 32;
        char *fb = (char *)AIRY_MALLOC(cap);
        if (fb) {
            size_t take = ctx->original_input_len > 512 ? 512 : ctx->original_input_len;
            snprintf(fb, cap, "{\"fallback_input\":\"%.*s\"}", (int)take, ctx->original_input);
            return fb;
        }
    }
    return AIRY_STRDUP("null");
}

/**
 * @brief Build model A's user prompt (goal G + rejection patch).
 */
static char *grad_s2_build_prompt(const grad_llm_ctx_t *ctx, const airy_gccp_goal_t *goal,
                                  const char *patch_json)
{
    char *goal_json = grad_goal_to_json(goal, ctx);
    if (!goal_json)
        return NULL;

    size_t cap = strlen(goal_json) + 2048;
    if (patch_json)
        cap += strlen(patch_json);
    char *prompt = (char *)AIRY_MALLOC(cap);
    if (!prompt) {
        AIRY_FREE(goal_json);
        return NULL;
    }

    if (patch_json && patch_json[0]) {
        snprintf(prompt, cap,
                 "Confirmed goal (G): %s\n\n"
                 "Rejected patch (apply this fix, do not regress unchanged nodes):\n%s\n\n"
                 "Generate the FULL corrected plan JSON (all nodes, consistent with the patch).",
                 goal_json, patch_json);
    } else {
        snprintf(prompt, cap,
                 "Confirmed goal (G): %s\n\n"
                 "Generate an initial skeleton plan JSON. Keep it minimal but logically sound.",
                 goal_json);
    }
    AIRY_FREE(goal_json);
    return prompt;
}

/**
 * @brief Parse plan JSON into airy_task_plan_t (with GRAD verification metadata).
 */
airy_err_t grad_parse_plan_json(const char *json, const char *plan_id,
                                airy_task_plan_t **out_plan)
{
    if (!json || !out_plan)
        return AIRY_EINVAL;
    *out_plan = NULL;

#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return AIRY_ESERVICE;
    cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
    if (!cJSON_IsArray(nodes)) {
        cJSON_Delete(root);
        return AIRY_ESERVICE;
    }
    size_t n = (size_t)cJSON_GetArraySize(nodes);
    if (n == 0) {
        cJSON_Delete(root);
        return AIRY_ESERVICE;
    }

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!plan) {
        cJSON_Delete(root);
        return AIRY_ENOMEM;
    }
    plan->task_plan_id = plan_id ? AIRY_STRDUP(plan_id) : AIRY_STRDUP("grad_plan");
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(n, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes) {
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        cJSON_Delete(root);
        return AIRY_ENOMEM;
    }

    for (size_t i = 0; i < n; i++) {
        cJSON *node = cJSON_GetArrayItem(nodes, (int)i);
        if (!node)
            continue;
        airy_task_node_t *tn = (airy_task_node_t *)AIRY_CALLOC(1, sizeof(airy_task_node_t));
        if (!tn)
            continue;

        cJSON *id = cJSON_GetObjectItem(node, "id");
        cJSON *goal = cJSON_GetObjectItem(node, "goal");
        cJSON *role = cJSON_GetObjectItem(node, "role");
        tn->task_node_id =
            cJSON_IsString(id) && id->valuestring ? AIRY_STRDUP(id->valuestring) : NULL;
        tn->task_node_goal =
            cJSON_IsString(goal) && goal->valuestring ? AIRY_STRDUP(goal->valuestring) : NULL;
        tn->task_node_agent_role =
            cJSON_IsString(role) && role->valuestring ? AIRY_STRDUP(role->valuestring) : NULL;
        if (tn->task_node_id)
            tn->task_node_id_len = strlen(tn->task_node_id);
        if (tn->task_node_agent_role)
            tn->task_node_role_len = strlen(tn->task_node_agent_role);
        tn->task_node_timeout_ms = 30000;
        tn->task_node_priority = 150;

        /* depends */
        cJSON *depends = cJSON_GetObjectItem(node, "depends");
        if (cJSON_IsArray(depends)) {
            size_t dc = (size_t)cJSON_GetArraySize(depends);
            if (dc > 0) {
                tn->task_node_depends_on = (char **)AIRY_CALLOC(dc, sizeof(char *));
                if (tn->task_node_depends_on) {
                    for (size_t d = 0; d < dc; d++) {
                        cJSON *dep = cJSON_GetArrayItem(depends, (int)d);
                        if (cJSON_IsString(dep) && dep->valuestring) {
                            /* 仅在复制成功时推进计数，避免 NULL 槽位被下游解引用 */
                            char *dup = AIRY_STRDUP(dep->valuestring);
                            if (dup)
                                tn->task_node_depends_on[tn->task_node_depends_count++] = dup;
                        }
                    }
                }
            }
        }

        cJSON *inputs = cJSON_GetObjectItem(node, "inputs");
        if (cJSON_IsArray(inputs)) {
            size_t ic = (size_t)cJSON_GetArraySize(inputs);
            if (ic > 0) {
                tn->task_node_inputs = (char **)AIRY_CALLOC(ic, sizeof(char *));
                if (tn->task_node_inputs) {
                    for (size_t d = 0; d < ic; d++) {
                        cJSON *it = cJSON_GetArrayItem(inputs, (int)d);
                        if (cJSON_IsString(it) && it->valuestring) {
                            char *dup = AIRY_STRDUP(it->valuestring);
                            if (dup)
                                tn->task_node_inputs[tn->task_node_inputs_count++] = dup;
                        }
                    }
                }
            }
        }

        cJSON *outputs = cJSON_GetObjectItem(node, "outputs");
        if (cJSON_IsArray(outputs)) {
            size_t oc = (size_t)cJSON_GetArraySize(outputs);
            if (oc > 0) {
                tn->task_node_outputs = (char **)AIRY_CALLOC(oc, sizeof(char *));
                if (tn->task_node_outputs) {
                    for (size_t d = 0; d < oc; d++) {
                        cJSON *ot = cJSON_GetArrayItem(outputs, (int)d);
                        if (cJSON_IsString(ot) && ot->valuestring) {
                            char *dup = AIRY_STRDUP(ot->valuestring);
                            if (dup)
                                tn->task_node_outputs[tn->task_node_outputs_count++] = dup;
                        }
                    }
                }
            }
        }

        /* cost */
        cJSON *cost_t = cJSON_GetObjectItem(node, "cost_time_ms");
        cJSON *cost_m = cJSON_GetObjectItem(node, "cost_mem_mb");
        if (cJSON_IsNumber(cost_t))
            tn->task_node_cost_time_ms = (int64_t)cost_t->valuedouble;
        if (cJSON_IsNumber(cost_m))
            tn->task_node_cost_mem_mb = (int64_t)cost_m->valuedouble;

        /* invariant_guard */
        cJSON *ig = cJSON_GetObjectItem(node, "invariant_guard");
        if (cJSON_IsBool(ig))
            tn->task_node_invariant_guard = (uint8_t)(ig->valueint != 0);

        plan->task_plan_nodes[plan->task_plan_node_count++] = tn;
    }

    /* entry points */
    cJSON *entry = cJSON_GetObjectItem(root, "entry");
    if (cJSON_IsArray(entry)) {
        size_t ec = (size_t)cJSON_GetArraySize(entry);
        if (ec > 0) {
            plan->task_plan_entry_points = (char **)AIRY_CALLOC(ec, sizeof(char *));
            if (plan->task_plan_entry_points) {
                for (size_t d = 0; d < ec; d++) {
                    cJSON *ep = cJSON_GetArrayItem(entry, (int)d);
                    if (cJSON_IsString(ep) && ep->valuestring) {
                        char *dup = AIRY_STRDUP(ep->valuestring);
                        if (dup)
                            plan->task_plan_entry_points[plan->task_plan_entry_count++] = dup;
                    }
                }
            }
        }
    }

    if (plan->task_plan_entry_count == 0) {
        for (size_t i = 0; i < plan->task_plan_node_count; i++) {
            airy_task_node_t *tn = plan->task_plan_nodes[i];
            if (tn && tn->task_node_depends_count == 0) {
                plan->task_plan_entry_points =
                    (char **)AIRY_REALLOC(plan->task_plan_entry_points,
                                          (plan->task_plan_entry_count + 1) * sizeof(char *));
                if (plan->task_plan_entry_points) {
                    plan->task_plan_entry_points[plan->task_plan_entry_count++] =
                        tn->task_node_id ? AIRY_STRDUP(tn->task_node_id) : NULL;
                }
                break;
            }
        }
    }

    cJSON_Delete(root);
    *out_plan = plan;
    return AIRY_SUCCESS;
#else
    (void)json;
    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!plan)
        return AIRY_ENOMEM;
    plan->task_plan_id = plan_id ? AIRY_STRDUP(plan_id) : AIRY_STRDUP("grad_plan");
    airy_task_node_t *node = (airy_task_node_t *)AIRY_CALLOC(1, sizeof(airy_task_node_t));
    if (!node) {
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        return AIRY_ENOMEM;
    }
    node->task_node_id = AIRY_STRDUP("S_01");
    node->task_node_id_len = 4;
    node->task_node_goal = AIRY_STRDUP("execute confirmed goal");
    node->task_node_agent_role = AIRY_STRDUP("executor");
    node->task_node_role_len = 8;
    node->task_node_timeout_ms = 30000;
    node->task_node_priority = 150;
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes) {
        AIRY_FREE(node->task_node_id);
        AIRY_FREE(node->task_node_goal);
        AIRY_FREE(node->task_node_agent_role);
        AIRY_FREE(node);
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        return AIRY_ENOMEM;
    }
    plan->task_plan_nodes[0] = node;
    plan->task_plan_node_count = 1;
    plan->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (plan->task_plan_entry_points) {
        plan->task_plan_entry_points[0] = AIRY_STRDUP("S_01");
        plan->task_plan_entry_count = 1;
    }
    *out_plan = plan;
    return AIRY_SUCCESS;
#endif
}

/* 保守降级计划：单节点空壳，以 "fallback_" 前缀标记（engine 采纳前拒绝）。
 * P1：LLM 调用失败与 JSON 解析失败均走此降级，保证修复环不中断；
 * 任一 STRDUP/分配失败返回 ENOMEM（空壳计划不得带 NULL 节点流入下游）。 */
static airy_err_t grad_fallback_plan(airy_task_plan_t **out_plan)
{
    airy_task_plan_t *fb = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(*fb));
    if (!fb)
        return AIRY_ENOMEM;
    fb->task_plan_id = AIRY_STRDUP("fallback_grad_plan");
    fb->task_plan_node_count = 1;
    fb->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    if (!fb->task_plan_id || !fb->task_plan_nodes) {
        airy_task_plan_free(fb);
        return AIRY_ENOMEM;
    }
    airy_task_node_t *n = (airy_task_node_t *)AIRY_CALLOC(1, sizeof(*n));
    if (!n) {
        /* 节点分配失败不得返回含 NULL 节点的计划（下游遍历会解引用
         * 崩溃），整体按失败处理。 */
        airy_task_plan_free(fb);
        return AIRY_ENOMEM;
    }
    n->task_node_id = AIRY_STRDUP("fallback");
    n->task_node_goal = AIRY_STRDUP("execute the confirmed goal");
    n->task_node_agent_role = AIRY_STRDUP("agent");
    if (!n->task_node_id || !n->task_node_goal || !n->task_node_agent_role) {
        airy_task_plan_free(fb);
        return AIRY_ENOMEM;
    }
    fb->task_plan_nodes[0] = n;
    *out_plan = fb;
    return AIRY_SUCCESS;
}

airy_err_t grad_llm_s2_plan(const airy_gccp_goal_t *goal, const char *patch_json,
                            airy_task_plan_t **out_plan, void *user_data)
{
    if (!out_plan)
        return AIRY_EINVAL;
    *out_plan = NULL;

    grad_llm_ctx_t *ctx = (grad_llm_ctx_t *)user_data;
    if (!ctx)
        return AIRY_EINVAL;

    char *prompt = grad_s2_build_prompt(ctx, goal, patch_json);
    if (!prompt)
        return AIRY_ENOMEM;

    char *plan_json = NULL;
    size_t plan_json_len = 0;
    airy_err_t err = grad_llm_call(ctx, GRAD_S2_SYSTEM_PROMPT, prompt, ctx->s2_model, &plan_json,
                                   &plan_json_len);
    AIRY_FREE(prompt);

    if (err != AIRY_SUCCESS || !plan_json) {
        AIRY_LOG_WARN("GRAD-A: LLM plan generation failed (err=%d), conservative fallback",
                      (int)err);
        /* 2.1.1.4 修复：LLM 不可用时不再解析 NULL（此前 cJSON 构建下
         * grad_parse_plan_json(NULL) 必返回 ESERVICE，降级名存实亡、GRAD
         * 修复环遇 LLM 故障直接中断）——构造保守单节点计划继续 GRAD 环
         * （C 验证 + B 仲裁），而非让修复循环失败。 */
        airy_err_t ferr = grad_fallback_plan(out_plan);
        if (ferr != AIRY_SUCCESS)
            AIRY_LOG_WARN("GRAD-A: fallback plan construction failed (err=%d)", (int)ferr);
        return ferr;
    }

    if (ctx->workspace_root && ctx->plan_id) {
        char *dir = grad_ws_dir(ctx);
        if (dir) {
            char fname[64];
            /* P13 (2026-08-23)：round 序号递进归一到 S2（每轮修复计划
             * 生成处）。此前进位耦合在 b_arbiter 的文件写入分支——LLM
             * 故障提前 return 或 workspace 不可用时 round_index 停滞，
             * 后续轮次的 plan_round_N 互相覆盖（决策链断档）。现在每轮
             * S2 必递增：plan_round_N 与下一轮 c_verify/b_arbiter 的
             * round_N 精确对应（决策链逐轮可回溯）。 */
            ctx->round_index++;
            snprintf(fname, sizeof(fname), "plan_round_%u.json",
                     (unsigned)(ctx->round_index));
            grad_ws_write_file(dir, "t2", fname, plan_json, plan_json_len);
            AIRY_FREE(dir);
        }

        grad_llm_trace_append(ctx, "s2_plan",
                              plan_json_len > 4000 ? "(plan_json truncated)" : plan_json);
    }

    airy_err_t parse_err = grad_parse_plan_json(plan_json, ctx->plan_id, out_plan);
    if (parse_err != AIRY_SUCCESS) {
        /* q8a：解析失败（非法 JSON / nodes 非数组 / 空节点 / markdown 围栏
         * 未剥）不得中断修复环——与 LLM 调用失败同等对待，走保守 fallback
         * （engine 采纳前按 "fallback_" 前缀拒绝，保持 seed 计划）。此前
         * parse_err 原样返回导致协调器 s2_plan 失败直接 break 整个 GRAD 环。 */
        AIRY_LOG_WARN("GRAD-A: plan JSON parse failed (err=%d), conservative fallback",
                      (int)parse_err);
        airy_err_t ferr = grad_fallback_plan(out_plan);
        if (ferr != AIRY_SUCCESS)
            AIRY_LOG_WARN("GRAD-A: fallback plan construction failed (err=%d)", (int)ferr);
        AIRY_FREE(plan_json);
        return ferr;
    }
    AIRY_FREE(plan_json);
    return AIRY_SUCCESS;
}
