// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_verifiers.c
 *   GRAD 四验器测试域：E-02 死锁检测、E-01 因果充分性、E-03 资源守恒、
 *   验证范围（full/delta scope）与 E-05 验证不足拒绝。
 */

#include "test_grad_internal.h"

void test_e02_deadlock(void)
{

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    plan->task_plan_id = AIRY_STRDUP("cycle_plan");
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));

    airy_task_node_t *a = make_node("A", "step a", "executor", NULL, 0, NULL, 0, 10, 0);
    airy_task_node_t *b = make_node("B", "step b", "executor", NULL, 0, NULL, 0, 10, 0);
    add_dep(a, "B");
    add_dep(b, "A");
    plan->task_plan_nodes[plan->task_plan_node_count++] = a;
    plan->task_plan_nodes[plan->task_plan_node_count++] = b;

    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan(plan, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E02_DEADLOCK) {
        TEST_PASS("E-02 deadlock detected (A<->B cycle)");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected E02, got err=%d error=%d", (int)err,
                 (int)report.error);
        TEST_FAIL("E-02 deadlock", msg);
    }

    airy_task_plan_t *ok = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    ok->task_plan_id = AIRY_STRDUP("ok_plan");
    ok->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    airy_task_node_t *a2 = make_node("A", "step a", "executor", NULL, 0, NULL, 0, 10, 0);
    airy_task_node_t *b2 = make_node("B", "step b", "executor", NULL, 0, NULL, 0, 10, 0);
    add_dep(b2, "A");
    ok->task_plan_nodes[ok->task_plan_node_count++] = a2;
    ok->task_plan_nodes[ok->task_plan_node_count++] = b2;
    __builtin_memset(&report, 0, sizeof(report));
    /* 传 budget：无 inputs/outputs 的节点由 E-03 统计 cost → checked_nodes>0，
     * 避免 E-05（验证不足）误判——本用例仅验证拓扑序（acyclic）。 */
    airy_grad_budget_t budget = {600000, 1024};
    err = airy_grad_verify_plan(ok, &budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK) {
        TEST_PASS("E-02 acyclic plan passes");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d", (int)err, (int)report.error);
        TEST_FAIL("E-02 acyclic", msg);
    }

    free_plan(ok);
    free_plan(plan);
}

void test_e01_causal(void)
{

    const char *a_out[] = {"data"};
    airy_task_plan_t *ok = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    ok->task_plan_id = AIRY_STRDUP("causal_ok");
    ok->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    airy_task_node_t *a = make_node("A", "produce", "executor", NULL, 0, a_out, 1, 10, 0);
    const char *b_in[] = {"data"};
    airy_task_node_t *b = make_node("B", "consume", "executor", b_in, 1, NULL, 0, 10, 0);
    add_dep(b, "A");
    ok->task_plan_nodes[ok->task_plan_node_count++] = a;
    ok->task_plan_nodes[ok->task_plan_node_count++] = b;

    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan(ok, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK) {
        TEST_PASS("E-01 valid producer chain passes");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d", (int)err, (int)report.error);
        TEST_FAIL("E-01 valid chain", msg);
    }
    free_plan(ok);

    const char *c_in[] = {"missing"};
    airy_task_plan_t *bad = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    bad->task_plan_id = AIRY_STRDUP("causal_bad");
    bad->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    airy_task_node_t *c = make_node("C", "consume missing", "executor", c_in, 1, NULL, 0, 10, 0);
    bad->task_plan_nodes[bad->task_plan_node_count++] = c;

    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(bad, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E01_CAUSAL_BREAK &&
        strcmp(report.missing_artifact, "missing") == 0) {
        TEST_PASS("E-01 dangling input detected (missing artifact)");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected E01, got err=%d error=%d missing=%s", (int)err,
                 (int)report.error, report.missing_artifact);
        TEST_FAIL("E-01 dangling input", msg);
    }
    free_plan(bad);
}

void test_e03_resource(void)
{

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    plan->task_plan_id = AIRY_STRDUP("resource_over");
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(3, sizeof(airy_task_node_t *));
    for (int i = 0; i < 3; i++) {
        char id[8];
        snprintf(id, sizeof(id), "N%d", i);
        plan->task_plan_nodes[plan->task_plan_node_count++] =
            make_node(id, id, "executor", NULL, 0, NULL, 0, 40, 0);
    }

    airy_grad_budget_t budget = {.time_ms = 100, .mem_mb = 0};
    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan(plan, &budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E03_RESOURCE_OVER &&
        report.cost_sum_ms == 120) {
        TEST_PASS("E-03 budget overrun detected (120ms > 100ms)");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected E03 sum=120, got err=%d error=%d sum=%lld", (int)err,
                 (int)report.error, (long long)report.cost_sum_ms);
        TEST_FAIL("E-03 overrun", msg);
    }

    airy_grad_budget_t budget_ok = {.time_ms = 200, .mem_mb = 0};
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(plan, &budget_ok, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK) {
        TEST_PASS("E-03 within budget passes");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d", (int)err, (int)report.error);
        TEST_FAIL("E-03 within budget", msg);
    }
    free_plan(plan);
}

void test_verify_scope(void)
{

    const char *a_out[] = {"x"};
    const char *b_in[] = {"x"};
    const char *b_out[] = {"y"};
    const char *c_in[] = {"y"};

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    plan->task_plan_id = AIRY_STRDUP("scope_plan");
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(3, sizeof(airy_task_node_t *));
    airy_task_node_t *a = make_node("A", "produce x", "executor", NULL, 0, a_out, 1, 10, 0);
    airy_task_node_t *b = make_node("B", "x to y", "executor", b_in, 1, b_out, 1, 10, 0);
    airy_task_node_t *c = make_node("C", "consume y", "executor", c_in, 1, NULL, 0, 10, 0);
    add_dep(b, "A");
    add_dep(c, "B");
    plan->task_plan_nodes[plan->task_plan_node_count++] = a;
    plan->task_plan_nodes[plan->task_plan_node_count++] = b;
    plan->task_plan_nodes[plan->task_plan_node_count++] = c;

    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_scope(plan, NULL, NULL, 0, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK && report.checked_nodes == 2) {
        TEST_PASS("full-scope verify checks 2 nodes");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK checked=2, got err=%d error=%d checked=%u",
                 (int)err, (int)report.error, (unsigned)report.checked_nodes);
        TEST_FAIL("full-scope", msg);
    }

    const char *scope[] = {"B"};
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_scope(plan, NULL, scope, 1, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK) {
        TEST_PASS("delta scope (B) passes");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d", (int)err, (int)report.error);
        TEST_FAIL("delta scope (B)", msg);
    }

    const char *bad_scope[] = {"NONEXIST"};
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_scope(plan, NULL, bad_scope, 1, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK) {
        TEST_PASS("delta scope with unknown node degrades safely");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK degrade, got err=%d error=%d", (int)err,
                 (int)report.error);
        TEST_FAIL("delta scope unknown node", msg);
    }

    free_plan(plan);
}

void test_e05_verify_insufficient(void)
{
    /* 空壳计划：2 个节点均无 inputs/outputs，cost=0 → 无任何可检元数据 */
    airy_task_plan_t *shell = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    shell->task_plan_id = AIRY_STRDUP("shell_plan");
    shell->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    shell->task_plan_nodes[shell->task_plan_node_count++] =
        make_node("S1", "a", "executor", NULL, 0, NULL, 0, 0, 0);
    shell->task_plan_nodes[shell->task_plan_node_count++] =
        make_node("S2", "b", "executor", NULL, 0, NULL, 0, 0, 0);

    airy_grad_budget_t budget = {600000, 1024};
    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan(shell, &budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E05_VERIFY_INSUFFICIENT &&
        report.checked_nodes == 0) {
        TEST_PASS("empty-shell plan rejected (E-05, no false convergence)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected E-05, got err=%d error=%d checked=%u skipped=%u",
                 (int)err, (int)report.error, (unsigned)report.checked_nodes,
                 (unsigned)report.skipped_nodes);
        TEST_FAIL("E-05 empty-shell", msg);
    }
    free_plan(shell);

    /* 对照：节点带 cost（cost>0 计入 E-03）→ checked_nodes>0，E-05 不触发 */
    airy_task_plan_t *costed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    costed->task_plan_id = AIRY_STRDUP("costed_plan");
    costed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    costed->task_plan_nodes[costed->task_plan_node_count++] =
        make_node("C1", "c", "executor", NULL, 0, NULL, 0, 10, 0);
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(costed, &budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK && report.checked_nodes >= 1) {
        TEST_PASS("cost-carrying plan verified (E-05 not triggered)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d checked=%u",
                 (int)err, (int)report.error, (unsigned)report.checked_nodes);
        TEST_FAIL("E-05 costed control", msg);
    }
    free_plan(costed);
}
