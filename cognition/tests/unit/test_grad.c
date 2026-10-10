// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad.c
 *   GRAD 协议（Airymax 化）计划级批判循环自动化验证测试——主文件
 *
 * 验证目标:
 *   1. 四验器 E-02 死锁检测（Kahn 拓扑排序环检测）
 *   2. 四验器 E-01 因果充分性（inputs ⊆ 前驱 outputs）
 *   3. 四验器 E-03 资源守恒（cost 求和 vs 预算）
 *   4. GRAD 协调器：seed 计划四验通过 → 收敛（不修改 seed）
 *   5. GRAD 协调器：四验被拒 → 模型 A 生成替换计划 → 收敛
 *   6. 驳回补丁构造（GRAD §6.2 格式）
 *   7. 决策链轮次 JSON 序列化
 *   8. 空计划/边界条件健壮性
 *   9. GRAD fail-closed：未收敛时 owned 计划必须被丢弃（out_plan=NULL）、
 *      返回 AIRY_ETIMEDOUT——缺陷计划严禁流出批判门静默执行（99.99% 守门）
 *
 * 主文件承载：SPDX 头、共享断言宏与计数（经 test_grad_internal.h）、
 * 共享辅助函数与 mock（make_node/add_dep/free_plan/grad_mock_*）与
 * int main()。测试函数按功能域拆分（见 test_grad_internal.h）。
 */

#include "test_grad_internal.h"

int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

airy_task_node_t *make_node(const char *id, const char *goal, const char *role,
                            const char **inputs, size_t input_count, const char **outputs,
                            size_t output_count, int64_t cost_ms, uint8_t invariant_guard)
{
    airy_task_node_t *n = (airy_task_node_t *)AIRY_CALLOC(1, sizeof(airy_task_node_t));
    if (!n)
        return NULL;
    n->task_node_id = AIRY_STRDUP(id);
    n->task_node_id_len = strlen(id);
    n->task_node_goal = AIRY_STRDUP(goal ? goal : id);
    n->task_node_agent_role = AIRY_STRDUP(role ? role : "executor");
    n->task_node_role_len = n->task_node_agent_role ? strlen(n->task_node_agent_role) : 0;
    n->task_node_timeout_ms = 30000;
    n->task_node_priority = 150;
    n->task_node_cost_time_ms = cost_ms;
    n->task_node_invariant_guard = invariant_guard;

    if (inputs && input_count > 0) {
        n->task_node_inputs = (char **)AIRY_CALLOC(input_count, sizeof(char *));
        if (n->task_node_inputs) {
            for (size_t i = 0; i < input_count; i++) {
                if (inputs[i]) {
                    n->task_node_inputs[n->task_node_inputs_count++] = AIRY_STRDUP(inputs[i]);
                }
            }
        }
    }
    if (outputs && output_count > 0) {
        n->task_node_outputs = (char **)AIRY_CALLOC(output_count, sizeof(char *));
        if (n->task_node_outputs) {
            for (size_t i = 0; i < output_count; i++) {
                if (outputs[i]) {
                    n->task_node_outputs[n->task_node_outputs_count++] = AIRY_STRDUP(outputs[i]);
                }
            }
        }
    }
    return n;
}

void add_dep(airy_task_node_t *n, const char *dep_id)
{
    if (!n || !dep_id)
        return;
    n->task_node_depends_on =
        (char **)AIRY_REALLOC(n->task_node_depends_on,
                              (n->task_node_depends_count + 1) * sizeof(char *));
    if (n->task_node_depends_on) {
        n->task_node_depends_on[n->task_node_depends_count++] = AIRY_STRDUP(dep_id);
    }
}

void free_plan(airy_task_plan_t *plan)
{
    if (!plan)
        return;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        airy_task_node_t *n = plan->task_plan_nodes[i];
        if (!n)
            continue;
        AIRY_FREE(n->task_node_id);
        AIRY_FREE(n->task_node_goal);
        AIRY_FREE(n->task_node_agent_role);
        if (n->task_node_depends_on) {
            for (size_t j = 0; j < n->task_node_depends_count; j++)
                AIRY_FREE(n->task_node_depends_on[j]);
            AIRY_FREE(n->task_node_depends_on);
        }
        if (n->task_node_inputs) {
            for (size_t j = 0; j < n->task_node_inputs_count; j++)
                AIRY_FREE(n->task_node_inputs[j]);
            AIRY_FREE(n->task_node_inputs);
        }
        if (n->task_node_outputs) {
            for (size_t j = 0; j < n->task_node_outputs_count; j++)
                AIRY_FREE(n->task_node_outputs[j]);
            AIRY_FREE(n->task_node_outputs);
        }
        AIRY_FREE(n);
    }
    AIRY_FREE(plan->task_plan_nodes);
    AIRY_FREE(plan->task_plan_id);
    if (plan->task_plan_entry_points) {
        for (size_t e = 0; e < plan->task_plan_entry_count; e++)
            AIRY_FREE(plan->task_plan_entry_points[e]);
        AIRY_FREE(plan->task_plan_entry_points);
    }
    AIRY_FREE(plan);
}

airy_err_t grad_mock_s2(const airy_gccp_goal_t *goal, const char *patch_json,
                        airy_task_plan_t **out_plan, void *user_data)
{
    grad_mock_ctx_t *mc = (grad_mock_ctx_t *)user_data;
    (void)goal;
    if (mc)
        mc->s2_call_count++;

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!plan)
        return AIRY_ENOMEM;
    plan->task_plan_id = AIRY_STRDUP("mock_plan");
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes) {
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        return AIRY_ENOMEM;
    }
    (void)patch_json;
    plan->task_plan_nodes[plan->task_plan_node_count++] =
        make_node("S_01", "execute", "executor", NULL, 0, NULL, 0, 10, 0);
    plan->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (plan->task_plan_entry_points) {
        plan->task_plan_entry_points[0] = AIRY_STRDUP("S_01");
        plan->task_plan_entry_count = 1;
    }
    *out_plan = plan;
    return AIRY_SUCCESS;
}

airy_err_t grad_mock_s2_bad(const airy_gccp_goal_t *goal, const char *patch_json,
                            airy_task_plan_t **out_plan, void *user_data)
{
    /* 恒生成 E-02 死锁环计划 → GRAD 永远无法收敛 */
    (void)goal;
    (void)patch_json;
    (void)user_data;
    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!plan)
        return AIRY_ENOMEM;
    plan->task_plan_id = AIRY_STRDUP("bad_loop");
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes) {
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        return AIRY_ENOMEM;
    }
    airy_task_node_t *a = make_node("A", "a", "executor", NULL, 0, NULL, 0, 10, 0);
    airy_task_node_t *b = make_node("B", "b", "executor", NULL, 0, NULL, 0, 10, 0);
    add_dep(a, "B");
    add_dep(b, "A");
    plan->task_plan_nodes[plan->task_plan_node_count++] = a;
    plan->task_plan_nodes[plan->task_plan_node_count++] = b;
    *out_plan = plan;
    return AIRY_SUCCESS;
}

airy_err_t grad_mock_arbiter(const airy_gccp_goal_t *goal, const airy_grad_report_t *report,
                             const airy_task_plan_t *plan, char **out_verdict,
                             void *user_data)
{
    grad_arb_ctx_t *mc = (grad_arb_ctx_t *)user_data;
    (void)goal;
    (void)report;
    (void)plan;
    if (!mc)
        return AIRY_SUCCESS;
    const char *v = mc->verdict_json;
    if (mc->call_count > 0 && mc->verdict_json_2)
        v = mc->verdict_json_2;
    mc->call_count++;
    if (!v)
        return AIRY_SUCCESS; /* NULL 输出 → 协调器采纳 C */
    *out_verdict = AIRY_STRDUP(v);
    return AIRY_SUCCESS;
}

/* ==================== main ==================== */
int main(void)
{
    printf("\n=== GRAD Protocol Tests ===\n");
    RUN_TEST(test_e02_deadlock);
    RUN_TEST(test_e01_causal);
    RUN_TEST(test_e03_resource);
    RUN_TEST(test_coordinator_seed_converge);
    RUN_TEST(test_coordinator_reject_regenerate);
    RUN_TEST(test_arbiter_conf_degrade);
    RUN_TEST(test_arbiter_e04_purpose_drift);
    RUN_TEST(test_coordinator_failclosed_nonconverged);
    RUN_TEST(test_build_patch);
    RUN_TEST(test_edge_cases);
    RUN_TEST(test_verify_scope);
    RUN_TEST(test_progress_events);
    RUN_TEST(test_e05_verify_insufficient);
    RUN_TEST(test_arbiter_malformed_verdicts);

    printf("\n=== Results: %d run, %d passed, %d failed ===\n", tests_run, tests_passed,
           tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
