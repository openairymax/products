// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_metadata.c
 *   GRAD 缺省元数据推导测试（0.1.20 C4）
 *
 * 验证目标：
 *   1. outputs 缺省由 goal slug 推导，计划内去重；
 *   2. inputs 缺省引用首依赖的首产物，无依赖节点保持无输入；
 *   3. cost 两维独立按角色基线推导，非法角色落 ACT 基线；
 *   4. 显式值绝不覆盖（inferred 归零）。
 */

#include "test_grad_internal.h"

void test_meta_outputs_fill(void)
{
    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    plan->task_plan_nodes =
        (airy_task_node_t **)AIRY_CALLOC(3, sizeof(airy_task_node_t *));
    plan->task_plan_nodes[plan->task_plan_node_count++] =
        make_node("A", "Do It", "plan", NULL, 0, NULL, 0, 0, 0);
    plan->task_plan_nodes[plan->task_plan_node_count++] =
        make_node("B", "do it!", "act", NULL, 0, NULL, 0, 0, 0);
    plan->task_plan_nodes[plan->task_plan_node_count++] =
        make_node("C", "!!!", "verify", NULL, 0, NULL, 0, 0, 0);

    size_t inferred = grad_plan_metadata_fill(plan);
    TEST_CHECK(inferred >= 3, "outputs inference counted");
    TEST_CHECK(plan->task_plan_nodes[0]->task_node_outputs_count == 1 &&
                   strcmp(plan->task_plan_nodes[0]->task_node_outputs[0], "do_it") == 0,
               "slug lowercases and underscores");
    TEST_CHECK(plan->task_plan_nodes[1]->task_node_outputs_count == 1 &&
                   strcmp(plan->task_plan_nodes[1]->task_node_outputs[0], "do_it_2") == 0,
               "duplicate slug gets numeric suffix");
    TEST_CHECK(plan->task_plan_nodes[2]->task_node_outputs_count == 1 &&
                   strcmp(plan->task_plan_nodes[2]->task_node_outputs[0], "out") == 0,
               "empty slug falls back to out");
    TEST_PASS("outputs inferred with plan-wide dedup");
    free_plan(plan);
}

void test_meta_inputs_fill(void)
{
    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    plan->task_plan_nodes =
        (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    airy_task_node_t *a = make_node("A", "produce", "plan", NULL, 0, NULL, 0, 0, 0);
    airy_task_node_t *b = make_node("B", "consume", "act", NULL, 0, NULL, 0, 0, 0);
    add_dep(b, "A");
    plan->task_plan_nodes[plan->task_plan_node_count++] = a;
    plan->task_plan_nodes[plan->task_plan_node_count++] = b;

    grad_plan_metadata_fill(plan);
    TEST_CHECK(a->task_node_inputs_count == 0, "no deps means no inputs");
    TEST_CHECK(b->task_node_inputs_count == 1 &&
                   strcmp(b->task_node_inputs[0], "A.produce") == 0,
               "input references first dep's first output");
    TEST_PASS("inputs inferred from dependency chain");
    free_plan(plan);
}

void test_meta_cost_fill(void)
{
    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    plan->task_plan_nodes =
        (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    plan->task_plan_nodes[plan->task_plan_node_count++] =
        make_node("A", "g", "reason", NULL, 0, NULL, 0, 0, 0);
    /* 非法角色（schema 外形态，如非 cJSON fallback 的历史 role）落 ACT 基线 */
    plan->task_plan_nodes[plan->task_plan_node_count++] =
        make_node("B", "g", "executor", NULL, 0, NULL, 0, 0, 0);

    grad_plan_metadata_fill(plan);
    TEST_CHECK(plan->task_plan_nodes[0]->task_node_cost_time_ms == 5000 &&
                   plan->task_plan_nodes[0]->task_node_cost_mem_mb == 256,
               "reason baseline applied");
    TEST_CHECK(plan->task_plan_nodes[1]->task_node_cost_time_ms == 10000 &&
                   plan->task_plan_nodes[1]->task_node_cost_mem_mb == 128,
               "unknown role falls back to act baseline");
    TEST_PASS("cost inferred per role baseline");
    free_plan(plan);
}

void test_meta_explicit_kept(void)
{
    const char *outs[] = {"keep"};
    const char *ins[] = {"lit:x"};
    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    plan->task_plan_nodes =
        (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    airy_task_node_t *a =
        make_node("A", "explicit", "act", ins, 1, outs, 1, 12345, 0);
    a->task_node_cost_mem_mb = 77;
    plan->task_plan_nodes[plan->task_plan_node_count++] = a;

    size_t inferred = grad_plan_metadata_fill(plan);
    TEST_CHECK(inferred == 0, "fully explicit plan infers nothing");
    TEST_CHECK(a->task_node_cost_time_ms == 12345 && a->task_node_cost_mem_mb == 77,
               "explicit costs untouched");
    TEST_CHECK(a->task_node_outputs_count == 1 &&
                   strcmp(a->task_node_outputs[0], "keep") == 0,
               "explicit outputs untouched");
    TEST_CHECK(a->task_node_inputs_count == 1 &&
                   strcmp(a->task_node_inputs[0], "lit:x") == 0,
               "explicit inputs untouched");
    TEST_PASS("explicit values never overwritten");
    free_plan(plan);
}
