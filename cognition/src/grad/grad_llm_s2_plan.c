// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_llm_s2_plan.c
 * @brief GRAD 模型 A（t2）计划生成域
 *
 * 目标 G + 拒绝补丁 → LLM DAG 计划 JSON → airy_task_plan_t。
 * 生成/解析失败默认立即 ESERVICE 并记录字段级失败详情（C-8 做法①）；
 * 仅 grad.fallback_as_seed=1 的回退实验路径构造保守单节点降级计划。
 */

#include "grad_internal.h"

/* 模型 A 系统提示（GRAD 计划生成指令）。C-9 做法②：内嵌两个必须通过
 * 校验器的最小样例（lit: 输入 / producer 链），并显式带 schema 版本号；
 * 样例字面量来自 grad_internal.h（与样例自检共用，漂移即判红）。 */
static const char GRAD_S2_SYSTEM_PROMPT[] =
    "You are the generator (Model A) in a goal-anchored DAG planning system. "
    "Produce a task plan as a dependency graph (DAG) targeting the confirmed goal.\n"
    "Reply with ONLY a JSON object (no markdown fences).\n"
    "Schema version: " GRAD_PLAN_SCHEMA_VERSION "\n"
    "Minimal valid sample 1 (literal input):\n"
    GRAD_S2_SAMPLE_LIT_JSON "\n"
    "Minimal valid sample 2 (producer chain):\n"
    GRAD_S2_SAMPLE_CHAIN_JSON "\n"
    "Rules: every inputs entry MUST be either \"lit:<name>\" or "
    "\"<producer_id>.<artifact>\", where producer_id is a node in this node's "
    "transitive depends closure and artifact is one of its outputs; "
    "role MUST be one of plan, retrieve, reason, act, verify; "
    "every node's inputs must be produced by its transitive predecessors "
    "(no dangling dependencies); the graph must be acyclic; "
    "outputs, inputs, cost_time_ms and cost_mem_mb may be omitted "
    "(they are inferred automatically, but explicit values are preferred); "
    "executable nodes MUST declare \"handler\" (a tool name, e.g. shell_run), "
    "\"params\" (JSON object with that handler's arguments, e.g. "
    "{\"command\":\"rpm -q tree\"}) and \"kind\":\"exec\"; nodes that only "
    "verify or summarize MUST declare \"kind\":\"verify\" or "
    "\"kind\":\"report\" and carry no handler. When handler is omitted the "
    "role decides: act -> shell_run/exec, verify -> verify, "
    "plan/retrieve/reason -> report. A node with kind=exec and no handler is "
    "a defective plan, not a no-op.\n"
    "nodes that must not break the goal's core invariant set invariant_guard=true.\n"
    "Structure first (T3): keep every text field terse and verifiable — "
    "concrete metadata, dependencies and acceptance-worthy outputs carry the "
    "plan; do not write long prose in goal/outputs fields.";

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
 * @brief 字段级违规的合法形态提示（C-9 做法③：回喂不止位置，还给可照抄的形态）。
 *
 * @ownership return: NONE（静态串）
 */
static const char *grad_field_hint(const char *field)
{
    if (!field)
        return NULL;
    if (strcmp(field, "inputs") == 0)
        return "each entry must be \"lit:<name>\" or \"<producer_id>.<artifact>\", "
               "where producer_id is in this node's transitive depends closure and "
               "lists <artifact> in its outputs";
    if (strcmp(field, "role") == 0)
        return "one of plan, retrieve, reason, act, verify";
    if (strcmp(field, "outputs") == 0)
        return "unique non-empty artifact names ([A-Za-z0-9_.-]); no other node may "
               "declare the same artifact";
    if (strcmp(field, "depends") == 0)
        return "existing node ids only; self references are forbidden";
    if (strcmp(field, "id") == 0)
        return "unique node id matching [A-Za-z0-9_-]{1,48}";
    if (strcmp(field, "handler") == 0)
        return "a tool name available at runtime (e.g. shell_run); required "
               "when kind is exec";
    if (strcmp(field, "params") == 0)
        return "a JSON object holding the handler's arguments, e.g. "
               "{\"command\":\"rpm -q tree\"}";
    if (strcmp(field, "kind") == 0)
        return "one of exec, verify, report; role act defaults to exec";
    if (strcmp(field, "goal") == 0)
        return "a non-empty string of at most 512 characters";
    return NULL;
}

/**
 * @brief Build model A's user prompt (goal G + rejection patch + round history).
 *
 * C13 决策③：patch 分支注入 "Previous rounds" 段（≤3 条，来自
 * round_note_cb 写入的 ctx->a_hist），A 因此知道此前哪些缺陷已被 C 拒绝、
 * 修补范围多大，避免重复提交同一计划。
 */
static char *grad_s2_build_prompt(const grad_llm_ctx_t *ctx, const airy_gccp_goal_t *goal,
                                  const char *patch_json)
{
    char *goal_json = grad_goal_to_json(goal, ctx);
    if (!goal_json)
        return NULL;

    size_t hist_len = 0;
    for (uint32_t i = 0; i < ctx->a_hist_count; i++)
        hist_len += strlen(ctx->a_hist[i]) + 4;

    size_t cap = strlen(goal_json) + 2048 + hist_len;
    if (patch_json)
        cap += strlen(patch_json);
    if (ctx->last_detail_set)
        cap += 1024;
    char *prompt = (char *)AIRY_MALLOC(cap);
    if (!prompt) {
        AIRY_FREE(goal_json);
        return NULL;
    }

    if (patch_json && patch_json[0]) {
        size_t off = (size_t)snprintf(prompt, cap,
                                      "Confirmed goal (G): %s\n\n"
                                      "Rejected patch (apply this fix, do not regress "
                                      "unchanged nodes):\n%s\n\n",
                                      goal_json, patch_json);
        if (off >= cap)
            off = cap - 1;
        if (ctx->a_hist_count) {
            off += (size_t)snprintf(prompt + off, cap - off,
                                    "Previous rounds (most recent last, max %d):\n",
                                    GRAD_ARBITER_MAX_HISTORY);
            if (off >= cap)
                off = cap - 1;
            for (uint32_t i = 0; i < ctx->a_hist_count && off < cap - 1; i++)
                off += (size_t)snprintf(prompt + off, cap - off, "  %s\n", ctx->a_hist[i]);
            if (off >= cap)
                off = cap - 1;
            off += (size_t)snprintf(prompt + off, cap - off,
                                    "Do NOT reproduce the same defect. If the same defect "
                                    "recurs, state why the plan cannot avoid it.\n\n");
            if (off >= cap)
                off = cap - 1;
        }
        /* C-8 做法④ / C-9 做法③：修补轮消费上一轮字段级错误文本——解析/
         * schema 失败的 {index, field, reason} 必须显式回喂，并附该字段
         * 的合法形态（只给位置不给形态，模型仍只能猜）。同一字段连续两轮
         * 违例时追加格式强化段（判据：同一 field 不得连续两轮违例）。 */
        if (ctx->last_detail_set) {
            if (ctx->last_detail.index >= 0)
                off += (size_t)snprintf(prompt + off, cap - off,
                                        "The previous plan was rejected by deterministic "
                                        "validation at node index %d (field \"%s\", reason "
                                        "\"%s\"). Correct exactly that field; keep the rest of "
                                        "the graph unchanged.\n\n",
                                        ctx->last_detail.index, ctx->last_detail.field,
                                        ctx->last_detail.reason);
            else
                off += (size_t)snprintf(prompt + off, cap - off,
                                        "The previous plan was rejected by deterministic "
                                        "validation at document level (field \"%s\", reason "
                                        "\"%s\"). Fix the document structure; keep the rest of "
                                        "the graph unchanged.\n\n",
                                        ctx->last_detail.field, ctx->last_detail.reason);
            if (off >= cap)
                off = cap - 1;
            const char *hint = grad_field_hint(ctx->last_detail.field);
            if (hint) {
                off += (size_t)snprintf(prompt + off, cap - off,
                                        "Legal form for field \"%s\": %s.\n\n",
                                        ctx->last_detail.field, hint);
                if (off >= cap)
                    off = cap - 1;
            }
            if (ctx->detail_repeat) {
                off += (size_t)snprintf(prompt + off, cap - off,
                                        "This field has violated in two consecutive rounds. "
                                        "Re-read the schema samples above and emit exactly "
                                        "the legal form; do not vary the shape.\n\n");
                if (off >= cap)
                    off = cap - 1;
            }
        }
        if (ctx->incremental_patch) {
            /* T3（0.1.20）：Δ 渲染契约——只回受影响节点子集（含新增生产
             * 者），未变节点禁止重复。输出 tokens 较全量重绘下降 ≥50%
             * 的 G61 主手段；服务端结构等价性校验由协调器 merge 承接
             * （越权/frozen 篡改回滚并强制整份重绘）。 */
            snprintf(prompt + off, cap - off,
                     "Output ONLY a patch plan JSON: {\"nodes\":[...],\"entry\":[...]} "
                     "containing just the nodes listed in affected_scope (plus any NEW "
                     "producer nodes they depend on), restating each affected node's "
                     "corrected definition with all required fields (id, goal, role, "
                     "depends, handler, params, kind). Do NOT repeat unchanged nodes. "
                     "Keep node ids stable.");
        } else {
            snprintf(prompt + off, cap - off,
                     "Generate the FULL corrected plan JSON (all nodes, consistent "
                     "with the patch).");
        }
    } else {
        snprintf(prompt, cap,
                 "Confirmed goal (G): %s\n\n"
                 "Generate the initial plan JSON. Decompose the goal into concrete "
                 "nodes (typically 3-5) with real dependencies and verifiable outputs.",
                 goal_json);
    }
    AIRY_FREE(goal_json);
    return prompt;
}

/**
 * @brief Parse plan JSON into airy_task_plan_t (with GRAD verification metadata).
 *
 * C-8：detail 非 NULL 时导出字段级失败详情（index=-1 表示整份结构错误）。
 */
static void grad_detail_set(grad_detail_t *detail, int index, const char *field,
                            const char *reason)
{
    if (!detail)
        return;
    detail->index = index;
    snprintf(detail->field, sizeof(detail->field), "%s", field ? field : "");
    snprintf(detail->reason, sizeof(detail->reason), "%s", reason ? reason : "");
}

/* C-12：执行角色缺省绑定的工具名。GRAD 的 act 节点即「执行一步真实
 * 动作」，与 tool_d 的 shell_run 一一对应；映射只在此处定义一次。 */
#define GRAD_ACT_HANDLER "shell_run"

static uint8_t s2_kind_of(const char *s)
{
    if (!s)
        return AIRY_TASK_NODE_AUTO;
    if (strcmp(s, "exec") == 0)
        return AIRY_TASK_NODE_EXEC;
    if (strcmp(s, "verify") == 0)
        return AIRY_TASK_NODE_VERIFY;
    if (strcmp(s, "report") == 0)
        return AIRY_TASK_NODE_REPORT;
    return AIRY_TASK_NODE_AUTO;
}

/* C-12：角色映射补全（意图图与执行图同一张图）。模型未声明 kind 时按
 * 角色定类别：有 handler 即可执行，act 缺省绑 shell_run，verify 只验证，
 * plan/retrieve/reason 只汇报。kind=exec 而角色非 act 且无 handler 时
 * 保留空 handler，交由 dispatch 报 AIRY_EINVAL（契约违约不得静默成功）。 */
static void s2_kind_fill(airy_task_node_t *tn)
{
    const char *role = tn->task_node_agent_role;

    if (tn->task_node_kind == AIRY_TASK_NODE_AUTO) {
        if (tn->task_node_handler_name)
            tn->task_node_kind = AIRY_TASK_NODE_EXEC;
        else if (role && strcmp(role, "act") == 0)
            tn->task_node_kind = AIRY_TASK_NODE_EXEC;
        else if (role && strcmp(role, "verify") == 0)
            tn->task_node_kind = AIRY_TASK_NODE_VERIFY;
        else
            tn->task_node_kind = AIRY_TASK_NODE_REPORT;
    }
    if (tn->task_node_kind == AIRY_TASK_NODE_EXEC && !tn->task_node_handler_name &&
        role && strcmp(role, "act") == 0)
        tn->task_node_handler_name = AIRY_STRDUP(GRAD_ACT_HANDLER);
}

#ifdef AIRY_HAS_CJSON

/* C-12：params 允许字符串（已是 JSON 文本）或内联对象/数组；内联形式
 * 序列化为未格式化文本。cJSON 打印缓冲用 cJSON_free 释放，转出串走
 * AIRY_FREE 域，避免跨分配器释放。 */
static char *s2_params_dup(cJSON *nj)
{
    cJSON *f = cJSON_GetObjectItem(nj, "params");
    char *printed = NULL;
    char *out = NULL;

    if (!f)
        return NULL;
    if (cJSON_IsString(f) && f->valuestring)
        return AIRY_STRDUP(f->valuestring);
    if (!cJSON_IsObject(f) && !cJSON_IsArray(f))
        return NULL;
    printed = cJSON_PrintUnformatted(f);
    if (!printed)
        return NULL;
    out = AIRY_STRDUP(printed);
    cJSON_free(printed);
    return out;
}

#endif /* AIRY_HAS_CJSON */

airy_err_t grad_parse_plan_json_ex(const char *json, const char *plan_id,
                                   airy_task_plan_t **out_plan, grad_detail_t *detail)
{
    grad_detail_set(detail, -1, "", "");
    if (!json || !out_plan) {
        grad_detail_set(detail, -1, "json", "invalid argument");
        return AIRY_EINVAL;
    }
    *out_plan = NULL;

#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        grad_detail_set(detail, -1, "json", "syntax");
        return AIRY_ESERVICE;
    }

    /* C1：schema 校验前置——违例整份拒绝（EINVAL），不静默修复。
     * C-8 起 s2_plan 侧默认把该失败直接上报（ESERVICE），不再降级 fallback；
     * 缺省元数据（outputs/inputs/cost）由 C4 在构造后推导补全。
     * plan_id 缺省值必须先于校验归一：否则 schema 以 "missing" 拒绝整份
     * 计划，下方的缺省构造形同虚设，launch 未带 plan_id 的合法计划会被
     * 系统性地降级为 fallback 空壳（终态 fail-closed）。 */
    const char *eff_id = plan_id ? plan_id : GRAD_PLAN_ID_DEFAULT;
    grad_schema_err_t serr;
    if (grad_plan_schema_validate(root, eff_id, &serr) != AIRY_SUCCESS) {
        AIRY_LOG_WARN("GRAD-A: plan schema violation (node=%d field=%s reason=%s)",
                      serr.index, serr.field, serr.reason);
        grad_detail_set(detail, serr.index, serr.field, serr.reason);
        cJSON_Delete(root);
        return AIRY_EINVAL;
    }

    cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
    if (!cJSON_IsArray(nodes)) {
        grad_detail_set(detail, -1, "nodes", "not_array");
        cJSON_Delete(root);
        return AIRY_ESERVICE;
    }
    size_t n = (size_t)cJSON_GetArraySize(nodes);
    if (n == 0) {
        grad_detail_set(detail, -1, "nodes", "empty");
        cJSON_Delete(root);
        return AIRY_ESERVICE;
    }

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!plan) {
        grad_detail_set(detail, -1, "plan", "alloc");
        cJSON_Delete(root);
        return AIRY_ENOMEM;
    }
    plan->task_plan_id = AIRY_STRDUP(eff_id);
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(n, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes) {
        grad_detail_set(detail, -1, "plan", "alloc");
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

        /* C-12：把「意图-产物」节点映射到执行层的「handler + 参数」调用；
         * 角色映射只在 s2_kind_fill 内定义一次（SSoT）。 */
        cJSON *handler = cJSON_GetObjectItem(node, "handler");
        cJSON *kind = cJSON_GetObjectItem(node, "kind");
        tn->task_node_handler_name =
            cJSON_IsString(handler) && handler->valuestring
                ? AIRY_STRDUP(handler->valuestring)
                : NULL;
        tn->task_node_params = s2_params_dup(node);
        tn->task_node_kind = s2_kind_of(cJSON_IsString(kind) ? kind->valuestring : NULL);
        s2_kind_fill(tn);
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

    /* C4：缺省元数据（outputs/inputs/cost）推导补全，只补缺省不覆盖显式值；
     * 推导占比过高时内部告警，不改变成功路径结果。 */
    grad_plan_metadata_fill(plan);

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
    node->task_node_agent_role = AIRY_STRDUP("act");
    node->task_node_role_len = 3;
    s2_kind_fill(node);
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
    grad_plan_metadata_fill(plan);
    plan->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (plan->task_plan_entry_points) {
        plan->task_plan_entry_points[0] = AIRY_STRDUP("S_01");
        plan->task_plan_entry_count = 1;
    }
    *out_plan = plan;
    return AIRY_SUCCESS;
#endif
}

airy_err_t grad_parse_plan_json(const char *json, const char *plan_id,
                                airy_task_plan_t **out_plan)
{
    return grad_parse_plan_json_ex(json, plan_id, out_plan, NULL);
}

/* 保守降级计划：单节点空壳，以 "fallback_" 前缀标记（engine 采纳前拒绝）。
 * C-8 做法②：fallback 只允许在修补轮内部占位，且必须携带最小可验元数据
 * （1 个 output、1 个 cost 估计、`lit:` 输入），否则 C 的四路验证对
 * 空壳无能为力、B 只能盲判。任一 STRDUP/分配失败返回 ENOMEM（空壳计划
 * 不得带 NULL 节点流入下游）。 */
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
    n->task_node_agent_role = AIRY_STRDUP("act");
    n->task_node_role_len = 3;
    s2_kind_fill(n);
    n->task_node_inputs = (char **)AIRY_CALLOC(1, sizeof(char *));
    n->task_node_outputs = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (n->task_node_inputs)
        n->task_node_inputs[0] = AIRY_STRDUP("lit:seed");
    if (n->task_node_outputs)
        n->task_node_outputs[0] = AIRY_STRDUP("fallback_result");
    if (n->task_node_inputs && n->task_node_inputs[0])
        n->task_node_inputs_count = 1;
    if (n->task_node_outputs && n->task_node_outputs[0])
        n->task_node_outputs_count = 1;
    n->task_node_cost_time_ms = 1000;
    n->task_node_cost_mem_mb = 64;
    if (!n->task_node_id || !n->task_node_goal || !n->task_node_agent_role ||
        n->task_node_inputs_count == 0 || n->task_node_outputs_count == 0) {
        airy_task_plan_free(fb);
        return AIRY_ENOMEM;
    }
    fb->task_plan_nodes[0] = n;
    *out_plan = fb;
    return AIRY_SUCCESS;
}

/* C-8：把本轮失败落进 ctx 的字段级记忆，下一轮修补 prompt 回喂。
 * C-9：同一 field 连续两轮违例时置 detail_repeat（下一轮 prompt 强化）。 */
static void grad_rec_detail(grad_llm_ctx_t *ctx, int index, const char *field,
                            const char *reason)
{
    const char *f = field ? field : "";
    ctx->detail_repeat =
        (ctx->last_detail_set && f[0] && strcmp(ctx->last_detail.field, f) == 0) ? 1 : 0;
    ctx->last_detail.index = index;
    snprintf(ctx->last_detail.field, sizeof(ctx->last_detail.field), "%s", f);
    snprintf(ctx->last_detail.reason, sizeof(ctx->last_detail.reason), "%s",
             reason ? reason : "");
    ctx->last_detail_set = 1;
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
    airy_err_t err = grad_llm_call(ctx, GRAD_S2_SYSTEM_PROMPT, prompt, ctx->s2_model,
                                   AIRY_GRAD_ROLE_A, &plan_json, &plan_json_len);
    AIRY_FREE(prompt);

    if (err != AIRY_SUCCESS || !plan_json) {
        grad_rec_detail(ctx, -1, "llm",
                        err != AIRY_SUCCESS ? "call_failed" : "empty_response");
        AIRY_LOG_WARN("GRAD-A: LLM plan generation failed (err=%d)", (int)err);
        AIRY_FREE(plan_json);
        return AIRY_ESERVICE;
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

    grad_detail_t detail;
    airy_err_t parse_err = grad_parse_plan_json_ex(plan_json, ctx->plan_id, out_plan, &detail);
    if (parse_err != AIRY_SUCCESS) {
        grad_rec_detail(ctx, detail.index, detail.field, detail.reason);
        AIRY_LOG_WARN("GRAD-A: plan JSON parse failed (err=%d index=%d field=%s reason=%s)",
                      (int)parse_err, detail.index, detail.field, detail.reason);
        AIRY_FREE(plan_json);
        /* C-8 做法①：默认路径任何生成/解析失败都不得降级为 fallback 空壳，
         * 否则同一字段违例会反复空转（判据禁止 rounds=3 converged=0）。
         * 仅在 grad.fallback_as_seed=1 的回退实验路径保留保守 fallback。 */
        if (!ctx->fallback_as_seed)
            return AIRY_ESERVICE;
        airy_err_t ferr = grad_fallback_plan(out_plan);
        if (ferr != AIRY_SUCCESS)
            AIRY_LOG_WARN("GRAD-A: fallback plan construction failed (err=%d)", (int)ferr);
        return ferr;
    }
    /* 本轮计划可解析：清掉上一轮的错误记忆，避免旧字段反复回喂。 */
    ctx->last_detail_set = 0;
    ctx->detail_repeat = 0;
    AIRY_FREE(plan_json);
    return AIRY_SUCCESS;
}
