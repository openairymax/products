// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file reactive.c
 * @brief Reactive planning strategy: quickly generate a dynamic plan from intent.
 *
 * Fixed: hardcoded single node → dynamically generates a multi-node plan
 * from the intent content. Supports LLM calls (when available) + rule
 * degradation (when unavailable). Rules match EN + zh-CN keyword sets;
 * the no-match fallback path emits generative actions with a writable
 * executor (never a verify-only DAG).
 *
 * 0.1.19 M5-4 §271 自 atoms/coreloopthree src/cognition/think/planner/
 * 迁入 products/cognition（族内归格 src/planner/）。
 */

#include "cognition.h"
#include "plan_strategy.h"
#include "llm_client.h"
#include "agent_vocab.h"
#include "airy_memory.h"
#include "airy_plan_nodes.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

typedef struct reactive_data {
    airy_llm_service_t *llm;
    char *model_name;
    airy_mtx_t *lock;
    uint64_t plan_counter;
} reactive_data_t;

typedef struct {
    const char *const *keywords;
    const char *role;
    const char *action;
    int priority;
    int timeout_ms;
} reactive_rule_t;

/* 每条规则一组触发词（英文 + 中文同义词），NULL 结尾；UTF-8 子串匹配。 */
static const char *const KW_QUERY[] = {"query", "查询", "检索", "查一下", NULL};
static const char *const KW_SEARCH[] = {"search", "搜索", "搜一下", "查找", NULL};
static const char *const KW_FIND[] = {"find", "找出", "找到", "定位", NULL};
static const char *const KW_CREATE[] = {"create", "创建", "新建", "建立", "创作", NULL};
static const char *const KW_GENERATE[] = {"generate", "生成", "产出", NULL};
static const char *const KW_WRITE[] = {"write", "写", "编写", "撰写", "拟定", NULL};
static const char *const KW_ANALYZE[] = {"analyze", "分析", "剖析", NULL};
static const char *const KW_COMPARE[] = {"compare", "对比", "比较", "相比", NULL};
static const char *const KW_EVALUATE[] = {"evaluate", "评估", "评价", "评审", NULL};
static const char *const KW_EXECUTE[] = {"execute", "执行", NULL};
static const char *const KW_RUN[] = {"run", "运行", "跑一下", "启动", NULL};
static const char *const KW_DELETE[] = {"delete", "删除", "删掉", "移除", NULL};
static const char *const KW_UPDATE[] = {"update", "更新", "修改", "改动", NULL};
static const char *const KW_TRANSLATE[] = {"translate", "翻译", NULL};
static const char *const KW_SUMMARIZE[] = {"summarize", "总结", "摘要", "归纳", "概括", NULL};
static const char *const KW_EXPLAIN[] = {"explain", "解释", "说明", "讲解", "解析", NULL};

static const reactive_rule_t REACTIVE_RULES[] = {
    {KW_QUERY, "retriever", "retrieve_and_answer", 200, 15000},
    {KW_SEARCH, "retriever", "search_and_rank", 200, 20000},
    {KW_FIND, "retriever", "find_and_present", 200, 15000},
    {KW_CREATE, "creator", "create_artifact", 180, 30000},
    {KW_GENERATE, "creator", "generate_content", 180, 30000},
    {KW_WRITE, "creator", "write_output", 180, 25000},
    {KW_ANALYZE, AGENT_VOCAB_ROLE_ANALYST, "analyze_and_report", 190, 25000},
    {KW_COMPARE, AGENT_VOCAB_ROLE_ANALYST, "compare_and_contrast", 190, 20000},
    {KW_EVALUATE, AGENT_VOCAB_ROLE_ANALYST, "evaluate_and_score", 190, 20000},
    {KW_EXECUTE, "executor", "execute_action", 220, 15000},
    {KW_RUN, "executor", "run_command", 220, 10000},
    {KW_DELETE, "executor", "delete_resource", 230, 10000},
    {KW_UPDATE, "executor", "update_resource", 220, 15000},
    {KW_TRANSLATE, "translator", "translate_content", 170, 20000},
    {KW_SUMMARIZE, "summarizer", "summarize_content", 170, 15000},
    {KW_EXPLAIN, "explainer", "explain_concept", 170, 20000},
};

/* UTF-8 安全截断长度：把 max_bytes 回退到最近的字符边界，避免切断
 * 多字节序列产生非法 UTF-8（%.*s 按字节截断，中文场景会把字符切成
 * 半个，goal 乱码导致下游执行体行为异常、工具不调用、响应为空）。 */
static size_t utf8_safe_len(const char *s, size_t max_bytes)
{
    size_t n = 0;
    if (!s)
        return 0;
    while (n < max_bytes && s[n])
        n++;
    /* 未截断，或截断点下一位不是 UTF-8 连续字节（0x80-0xBF）→ 恰好
     * 落在字符边界，无需回退。 */
    if (n < max_bytes || ((unsigned char)s[n] & 0xC0) != 0x80)
        return n;
    /* 截断点在多字节序列中间：从当前位开始前移到引导字节之前。
     * 注意检查 s[n]（当前候选位）而非 s[n-1]：s[max] 是连续字节说明
     * 字符跨越边界，引导字节（0xC0-0xFD）自身即视为边界位置。 */
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

#define REACTIVE_RULE_COUNT (sizeof(REACTIVE_RULES) / sizeof(REACTIVE_RULES[0]))

static int rule_hit(const char *goal, const reactive_rule_t *rule)
{
    for (size_t k = 0; rule->keywords[k]; k++) {
        if (strstr(goal, rule->keywords[k]) != NULL)
            return 1;
    }
    return 0;
}

static int match_rules(const char *goal, size_t goal_len, int *out_indices, int max_matches)
{
    if (!goal || goal_len == 0)
        return 0;
    int count = 0;
    for (size_t r = 0; r < REACTIVE_RULE_COUNT && count < max_matches; r++) {
        if (rule_hit(goal, &REACTIVE_RULES[r]))
            out_indices[count++] = (int)r;
    }
    return count;
}

/* 取 LLM 行动计划中第 idx 行（0 起）；无该行返回 NULL（借用指针，不分配）。 */
static const char *reactive_line_at(const char *plan, size_t idx)
{
    if (!plan)
        return NULL;
    const char *p = plan;
    for (size_t i = 0; i < idx; i++) {
        const char *nl = strchr(p, '\n');
        if (!nl)
            return NULL;
        p = nl + 1;
    }
    if (!*p)
        return NULL;
    return p;
}

static void reactive_destroy(airy_plan_strategy_t *strategy)
{
    if (!strategy)
        return;
    reactive_data_t *data = (reactive_data_t *)strategy->data;
    if (data) {
        if (data->model_name)
            AIRY_FREE(data->model_name);
        if (data->lock)
            airy_mtx_free(data->lock);
        AIRY_FREE(data);
    }
    AIRY_FREE(strategy);
}

static airy_err_t reactive_plan(const airy_intent_t *intent, void *context,
                                airy_task_plan_t **out_plan)
{

    reactive_data_t *data = (reactive_data_t *)context;
    if (!intent || !out_plan)
        AIRY_RET_ERR(AIRY_EINVAL);

    const char *goal = intent->intent_goal ? (const char *)intent->intent_goal : "";
    size_t goal_len = intent->intent_goal_len;
    int complexity = (int)(intent->intent_flags & 0x07);

    /* CPR 汇总认知决策消费（2026-08-16）：决策 JSON 由 engine 注入 intent
     * （intent_cog_decision）。clarify_needed → 计划从简（避免对歧义任务
     * 过度分解）；risk_count>0 → 追加验证节点。LLM 规划路径同时把决策
     * 拼入提示词，要求拆分真正独立的工作、禁止重复同一动作。 */
    int cog_clarify = 0, cog_risk = 0;
    if (intent->intent_cog_decision && intent->intent_cog_decision[0]) {
#ifdef AIRY_HAS_CJSON
        cJSON *dec = cJSON_Parse(intent->intent_cog_decision);
        if (dec) {
            cJSON *c = cJSON_GetObjectItem(dec, "clarify_needed");
            if (cJSON_IsNumber(c) && c->valueint != 0)
                cog_clarify = 1;
            cJSON *r = cJSON_GetObjectItem(dec, "risk_count");
            if (cJSON_IsNumber(r) && r->valueint > 0)
                cog_risk = (int)r->valueint;
            cJSON_Delete(dec);
        }
#endif
    }

    int matched[8];
    int match_count = match_rules(goal, goal_len, matched, 8);

    char *llm_plan = NULL;
    if (data && data->llm && airy_llm_service_is_available(data->llm)) {
        char prompt[1152];
        int plen = snprintf(prompt, sizeof(prompt),
                            "Generate a brief action plan (max 5 steps) for: %s\n"
                            "Format: one action per line, no numbering.",
                            goal_len > 500 ? "(long input)" : goal);
        if (cog_clarify || cog_risk > 0) {
            size_t used = (size_t)plen < sizeof(prompt) ? (size_t)plen : sizeof(prompt);
            snprintf(prompt + used, sizeof(prompt) - used,
                     "\nIndependent cognition review: clarify_needed=%d risk_count=%d. "
                     "Split genuinely distinct work into separate steps; never duplicate "
                     "the same step.",
                     cog_clarify, cog_risk);
        }
        airy_err_t llm_err = airy_llm_service_call(data->llm, prompt, &llm_plan);
        if (llm_err != AIRY_SUCCESS || !llm_plan) {
            llm_plan = NULL;
        }
    }

    size_t node_count = 0;
    if (llm_plan) {
        for (size_t i = 0; llm_plan[i]; i++) {
            if (llm_plan[i] == '\n')
                node_count++;
        }
        if (node_count == 0 && strlen(llm_plan) > 0)
            node_count = 1;
        if (node_count > 8)
            node_count = 8;
    }

    if (node_count == 0) {
        if (match_count > 0) {
            node_count = (size_t)match_count;
            if (complexity >= 4)
                node_count += 1;
        } else {
            node_count = (complexity >= 3) ? 3 : 1;
        }
    }

    /* 认知决策的启发式消费（确定性，不依赖 LLM）：歧义任务收敛计划，
     * 风险标记追加验证节点（上限 8 保持既有护栏）。 */
    if (cog_clarify && node_count > 2)
        node_count = 2;
    /* #9 修复：intent_flags 0x10（GCCP AMBIGUOUS）此前只写不读，是死标志。
     * 歧义确认后的目标同样应收敛计划（与 cog_clarify 同构），避免对
     * 未澄清意图过度分解——LLM 规划路径保留多步能力，由 GCCP 交互澄清。 */
    if ((intent->intent_flags & 0x10) && node_count > 2)
        node_count = 2;
    int add_validator = 0;
    if (cog_risk > 0 && node_count > 0 && node_count < 8) {
        add_validator = 1;
        node_count += 1;
    }

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!plan) {
        if (llm_plan)
            AIRY_FREE(llm_plan);
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    uint64_t counter = 0;
    if (data) {
        airy_mtx_lock(data->lock);
        data->plan_counter++;
        counter = data->plan_counter;
        airy_mtx_unlock(data->lock);
    }

    char plan_id[64];
    snprintf(plan_id, sizeof(plan_id), "reactive_%llu", (unsigned long long)counter);
    plan->task_plan_id = AIRY_STRDUP(plan_id);

    plan->task_plan_nodes =
        (airy_task_node_t **)AIRY_CALLOC(node_count, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes && node_count > 0) {
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        if (llm_plan)
            AIRY_FREE(llm_plan);
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    for (size_t i = 0; i < node_count; i++) {
        airy_task_node_t *node = (airy_task_node_t *)AIRY_CALLOC(1, sizeof(airy_task_node_t));
        if (!node)
            goto cleanup;

        /* 认知决策追加的验证节点（语义位于计划末尾） */
        int is_validator = (add_validator && i == node_count - 1);

        char nid[128];
        snprintf(nid, sizeof(nid), "%s_step%zu", plan_id, i + 1);
        node->task_node_id = AIRY_STRDUP(nid);

        if (is_validator) {
            node->task_node_agent_role = AIRY_STRDUP("validator");
            node->task_node_timeout_ms = 15000;
            node->task_node_priority = 140;
        } else if (llm_plan && i < node_count) {
            node->task_node_agent_role = AIRY_STRDUP("reactive-agent");
            node->task_node_timeout_ms = 20000;
            node->task_node_priority = 180 - (int)i * 10;
        } else if (i < (size_t)match_count) {
            const reactive_rule_t *rule = &REACTIVE_RULES[matched[i]];
            node->task_node_agent_role = AIRY_STRDUP(rule->role);
            node->task_node_timeout_ms = rule->timeout_ms;
            node->task_node_priority = rule->priority;
        } else {
            /* 兜底执行体 = 角色词汇表 fallback（coding，可写）：生成型
             * 兜底动作需要可写执行体；旧 {processor,validator,formatter}
             * 中 validator 归一化为只读 tester，会卡死产物产出。 */
            node->task_node_agent_role = AIRY_STRDUP(AGENT_VOCAB_FALLBACK);
            node->task_node_timeout_ms = 15000;
            node->task_node_priority = 150 - (int)i * 10;
        }

        /* Per-node goal (user-visible): feeds the DAG node name, the work
         * hall board and the cognition review sub-agents. The full request
         * text still travels via the agent invoke input.
         *
         * 节点目标必须彼此 DISTINCT（2026-08-16 修复）：此前多节点一律
         * 使用整段原始 goal（"子目标 x/N：<goal>"），导致节点目标雷同、
         * 认知审查子 agent 误判"重复未分解"。现在按语义来源取差异化动作：
         * 验证节点 / LLM 逐行动作 / 规则动作 / 兜底角色动作。
         *
         * 2026-08-19 根因修复：节点级 goal 只描述本节点职责，不再把完整
         * 用户指令拼进每个节点。此前 goal = "<动作>：<完整指令>"，验证节点
         * （validator→tester_v1，只读 ACL）看到指令中的写动作词（fs_write
         * 等）会越界尝试执行，被 ACL 拒绝导致整个任务误判失败；同时完整
         * 指令重复注入各节点造成执行体上下文污染。完整任务文本仍经 agent
         * invoke input 传递给执行体，节点 goal 保留动作 + 60 字符内摘要。 */
        {
            char goal_buf[192];
            char line_buf[128];
            /* 确认目标段（"\n[确认目标] <endpoint>"）必须完整保留：
             * GCCP 收敛结果（engine_process.c 注入 intent_goal）是比原始
             * 指令更精确的规划依据。整体按 60 字节截断时，长指令的截断点
             * 落在原始指令内部，[确认目标] 段被整个切掉 → 规划器拿不到
             * 确认结果（q8a 修复：优先截短原始指令部分，确认段恒保留）。 */
            const char *goal_src = goal;
            size_t glen = goal_len < 60 ? goal_len : 60;
            const char *confirm_marker = strstr(goal, "\n[确认目标] ");
            size_t confirm_off = 0, confirm_len = 0;
            if (confirm_marker) {
                confirm_off = (size_t)(confirm_marker - goal);
                confirm_len = goal_len - confirm_off;
                if (confirm_len < glen)
                    glen -= confirm_len;
                else
                    confirm_len = 0; /* 确认段本身超预算：退回整体截断 */
            }
            char cut_goal[192];
            size_t head_len = utf8_safe_len(goal_src, glen);
            if (confirm_len > 0 && head_len + confirm_len < sizeof(cut_goal) - 1) {
                __builtin_memcpy(cut_goal, goal_src, head_len);
                __builtin_memcpy(cut_goal + head_len, goal_src + confirm_off, confirm_len);
                cut_goal[head_len + confirm_len] = '\0';
                goal_src = cut_goal;
                glen = head_len + confirm_len;
            } else {
                glen = head_len;
            }
            const char *action = NULL;
            if (is_validator) {
                /* 验证节点：剥离用户指令动作词，只保留验证职责。 */
                action = "验证最终结果";
            } else if (llm_plan) {
                /* LLM 逐行动作：截断到行尾（去尾随换行），作为该节点目标 */
                const char *ln = reactive_line_at(llm_plan, i);
                if (ln) {
                    size_t ln_len = 0;
                    while (ln[ln_len] && ln[ln_len] != '\n' && ln_len < sizeof(line_buf) - 1)
                        ln_len++;
                    __builtin_memcpy(line_buf, ln, ln_len);
                    line_buf[ln_len] = '\0';
                    action = line_buf;
                }
            } else if (i < (size_t)match_count) {
                action = REACTIVE_RULES[matched[i]].action;
            }
            if (is_validator) {
                snprintf(goal_buf, sizeof(goal_buf), "验证最终结果");
            } else if (glen == 0) {
                snprintf(goal_buf, sizeof(goal_buf), "完成阶段 %zu/%zu（%s）", i + 1, node_count,
                         node->task_node_agent_role ? node->task_node_agent_role : "task");
            } else if (action && action[0]) {
                snprintf(goal_buf, sizeof(goal_buf), "%s：%.*s", action, (int)glen, goal_src);
            } else if (i >= (size_t)match_count) {
                /* 生成型兜底动作：规则 miss + LLM 不可用时不得静默退化为
                 * 校验型序列（R2-A），首节点必须携带产出语义。 */
                static const char *const FALLBACK_ACTIONS[] = {"生成初始方案", "产出阶段成果",
                                                               "汇总并交付结果"};
                snprintf(goal_buf, sizeof(goal_buf), "%s：%.*s", FALLBACK_ACTIONS[i % 3],
                         (int)glen, goal_src);
            } else if (node_count == 1) {
                snprintf(goal_buf, sizeof(goal_buf), "%.*s", (int)glen, goal_src);
            } else {
                snprintf(goal_buf, sizeof(goal_buf), "子目标 %zu/%zu（%s）：%.*s", i + 1, node_count,
                         node->task_node_agent_role ? node->task_node_agent_role : "task",
                         (int)glen, goal_src);
            }
            node->task_node_goal = AIRY_STRDUP(goal_buf);
            if (!node->task_node_goal)
                goto cleanup;
        }

        if (i > 0) {
            node->task_node_depends_on = (char **)AIRY_MALLOC(sizeof(char *));
            if (node->task_node_depends_on) {
                node->task_node_depends_count = 1;
                node->task_node_depends_on[0] =
                    AIRY_STRDUP(plan->task_plan_nodes[i - 1]->task_node_id);
            }
        }

        /* GRAD verification metadata (E-01 causality / E-03 resources /
         * E-04 invariants): each node produces an artifact signature;
         * from the second node on, inputs take the predecessor's output;
         * the last node is protected by the goal invariant. On
         * inputs/outputs allocation failure, keep NULL (conservative skip). */
        char out_sig[192];
        snprintf(out_sig, sizeof(out_sig), "artifact:%s", nid);
        node->task_node_outputs = (char **)AIRY_CALLOC(1, sizeof(char *));
        if (node->task_node_outputs) {
            node->task_node_outputs[0] = AIRY_STRDUP(out_sig);
            if (node->task_node_outputs[0])
                node->task_node_outputs_count = 1;
        }
        if (i > 0 && plan->task_plan_nodes[i - 1] &&
            plan->task_plan_nodes[i - 1]->task_node_outputs_count > 0 &&
            plan->task_plan_nodes[i - 1]->task_node_outputs[0]) {
            node->task_node_inputs = (char **)AIRY_CALLOC(1, sizeof(char *));
            if (node->task_node_inputs) {
                node->task_node_inputs[0] =
                    AIRY_STRDUP(plan->task_plan_nodes[i - 1]->task_node_outputs[0]);
                if (node->task_node_inputs[0])
                    node->task_node_inputs_count = 1;
                else
                    node->task_node_inputs_count = 0;
            }
        }
        node->task_node_cost_time_ms = node->task_node_timeout_ms * 4 / 10;
        node->task_node_cost_mem_mb = 64;
        node->task_node_invariant_guard = (i == node_count - 1) ? 1 : 0;

        plan->task_plan_nodes[i] = node;
        plan->task_plan_node_count++;
    }

    plan->task_plan_entry_points = (char **)AIRY_MALLOC(sizeof(char *));
    if (plan->task_plan_entry_points && plan->task_plan_node_count > 0) {
        plan->task_plan_entry_count = 1;
        plan->task_plan_entry_points[0] = AIRY_STRDUP(plan->task_plan_nodes[0]->task_node_id);
    }

    if (llm_plan)
        AIRY_FREE(llm_plan);
    *out_plan = plan;
    return AIRY_SUCCESS;

cleanup:
    plan_nodes_reclaim(plan);
    if (llm_plan)
        AIRY_FREE(llm_plan);
    AIRY_RET_ERR(AIRY_ENOMEM);
}

airy_plan_strategy_t *airy_plan_reactive_create(airy_llm_service_t *llm)
{
    airy_plan_strategy_t *strat =
        (airy_plan_strategy_t *)AIRY_CALLOC(1, sizeof(airy_plan_strategy_t));
    if (!strat)
        return NULL;

    reactive_data_t *rdata = (reactive_data_t *)AIRY_CALLOC(1, sizeof(reactive_data_t));
    if (!rdata) {
        AIRY_FREE(strat);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    rdata->llm = llm;
    rdata->lock = airy_mtx_create();
    if (!rdata->lock) {
        AIRY_FREE(rdata);
        AIRY_FREE(strat);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    strat->plan = reactive_plan;
    strat->destroy = reactive_destroy;
    strat->data = rdata;

    return strat;
}
