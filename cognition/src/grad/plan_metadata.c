// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file plan_metadata.c
 * @brief GRAD 计划缺省元数据推导（0.1.20 C4）。
 *
 * 三遍扫描，只补缺省、绝不覆盖显式值：
 *   pass1 outputs —— 缺省节点由 goal slug 推导单个产物名，计划内去重；
 *   pass2 inputs  —— 缺省且带依赖的节点引用首依赖节点的首产物
 *                    （pass1 保证产物名恒非空），无依赖节点保持无输入；
 *   pass3 cost    —— 时间/内存两维独立按角色基线 + goal 规模推导。
 *
 * 推导占比超过 GRAD_INFERRED_MAX_RATIO 时告警不拒绝——元数据全为
 * 推导值说明上游计划器未给出任何资源判断，值得人工关注但拓扑语义
 * 已由 C1 保证，仍可交付执行。
 */

#include "grad_internal.h"
#include "logging.h"

#include <stdio.h>

/* 角色基线表：fixed + per_kchar * len(goal)/1000，时间/内存两维独立 */
typedef struct {
    int64_t fixed_ms;
    int64_t per_kchar_ms;
    int64_t fixed_mb;
    int64_t per_kchar_mb;
} grad_role_baseline_t;

static const grad_role_baseline_t GRAD_ROLE_BASELINE[GRAD_PLAN_ROLE_COUNT] = {
    /* plan */ {2000, 100, 64, 2},
    /* retrieve */ {3000, 50, 128, 4},
    /* reason */ {5000, 200, 256, 8},
    /* act */ {10000, 300, 128, 4},
    /* verify */ {2000, 100, 64, 2},
};

/* goal → 小写 slug（非 [a-z0-9] 归 '_'，截尾 '_'，空则 "out"），至多 24 字符 */
static void grad_slug(const char *goal, char *out)
{
    size_t n = 0;
    for (const char *p = goal; p && *p && n < 24; p++) {
        char c = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
        out[n++] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    while (n > 0 && out[n - 1] == '_')
        n--;
    if (n == 0) {
        out[0] = 'o';
        out[1] = 'u';
        out[2] = 't';
        n = 3;
    }
    out[n] = '\0';
}

static bool grad_out_taken(const airy_task_plan_t *plan, const char *name)
{
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *tn = plan->task_plan_nodes[i];
        if (!tn)
            continue;
        for (size_t j = 0; j < tn->task_node_outputs_count; j++) {
            if (tn->task_node_outputs[j] && strcmp(tn->task_node_outputs[j], name) == 0)
                return true;
        }
    }
    return false;
}

static bool grad_push_str(char ***arr, size_t *count, const char *s)
{
    char **na = (char **)AIRY_REALLOC(*arr, (*count + 1) * sizeof(char *));
    if (!na)
        return false;
    *arr = na;
    char *d = AIRY_STRDUP(s);
    if (!d)
        return false;
    (*arr)[(*count)++] = d;
    return true;
}

static void grad_fill_outputs(airy_task_plan_t *plan, size_t *inferred)
{
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        airy_task_node_t *tn = plan->task_plan_nodes[i];
        if (!tn || tn->task_node_outputs_count > 0)
            continue;
        char base[25];
        grad_slug(tn->task_node_goal, base);
        char name[32];
        snprintf(name, sizeof(name), "%s", base);
        for (unsigned k = 2; grad_out_taken(plan, name); k++)
            snprintf(name, sizeof(name), "%s_%u", base, k);
        if (grad_push_str(&tn->task_node_outputs, &tn->task_node_outputs_count, name))
            (*inferred)++;
    }
}

static airy_task_node_t *grad_node_of(const airy_task_plan_t *plan, const char *id)
{
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        airy_task_node_t *tn = plan->task_plan_nodes[i];
        if (tn && tn->task_node_id && strcmp(tn->task_node_id, id) == 0)
            return tn;
    }
    return NULL;
}

static void grad_fill_inputs(const airy_task_plan_t *plan, size_t *inferred)
{
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        airy_task_node_t *tn = plan->task_plan_nodes[i];
        if (!tn || tn->task_node_inputs_count > 0 || tn->task_node_depends_count == 0)
            continue;
        const airy_task_node_t *dep =
            grad_node_of(plan, tn->task_node_depends_on[0]);
        if (!dep || dep->task_node_outputs_count == 0 || !dep->task_node_outputs[0])
            continue;
        char ref[GRAD_INPUT_MAX];
        snprintf(ref, sizeof(ref), "%s.%s", dep->task_node_id, dep->task_node_outputs[0]);
        if (grad_push_str(&tn->task_node_inputs, &tn->task_node_inputs_count, ref))
            (*inferred)++;
    }
}

static void grad_fill_cost(airy_task_node_t *tn, size_t *inferred)
{
    grad_plan_role_e r = grad_role_of(tn->task_node_agent_role);
    if (r == GRAD_PLAN_ROLE_COUNT)
        r = GRAD_PLAN_ROLE_ACT;
    const grad_role_baseline_t *bl = &GRAD_ROLE_BASELINE[r];
    int64_t kchars = (int64_t)(tn->task_node_goal ? strlen(tn->task_node_goal) : 0) / 1000;
    if (tn->task_node_cost_time_ms <= 0) {
        tn->task_node_cost_time_ms = bl->fixed_ms + bl->per_kchar_ms * kchars;
        (*inferred)++;
    }
    if (tn->task_node_cost_mem_mb <= 0) {
        tn->task_node_cost_mem_mb = bl->fixed_mb + bl->per_kchar_mb * kchars;
        (*inferred)++;
    }
}

size_t grad_plan_metadata_fill(airy_task_plan_t *plan)
{
    if (!plan || plan->task_plan_node_count == 0)
        return 0;
    size_t inferred = 0;
    grad_fill_outputs(plan, &inferred);
    grad_fill_inputs(plan, &inferred);
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        if (plan->task_plan_nodes[i])
            grad_fill_cost(plan->task_plan_nodes[i], &inferred);
    }
    size_t total = (size_t)plan->task_plan_node_count * 3;
    if ((double)inferred / (double)total > GRAD_INFERRED_MAX_RATIO)
        AIRY_LOG_WARN("GRAD-A: metadata_inferred_high (%zu/%zu slots inferred)", inferred,
                      total);
    return inferred;
}
