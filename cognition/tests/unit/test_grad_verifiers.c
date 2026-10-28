// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_verifiers.c
 *   GRAD 五验器测试域（0.1.20 C5）：E-02 死锁检测、E-01 因果充分性
 *   （三形态输入 + 自引用判裂 + 复合闭包约束）、E-03 资源守恒、
 *   E-04 purpose-drift 确定性守卫（关键词实体交集）、E-05 元数据覆盖
 *   （any 语义：参与集内任一节点元数据不全即拒）、验证范围
 *   （full/delta scope）与新计数器（causal_checked/cost_checked/
 *   metadata_ok/guard_state/errors 位图）。
 *
 * 元数据完备判据：outputs 非空 ∧ 有成本 ∧ 依赖/输入一致。
 * 断言 E-01/E-03 的用例必须为全部参与节点补全元数据，否则 E-05
 * （优先级高于 E-01/E-03）会遮蔽目标错误。
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
    /* 复合形态 "A.done" 拆为 {pid="A", name="done"}：pid 与 producer id
     * 精确匹配（含大小写），outputs 须精确含 name，故 A 产出 "done"
     * 而非整体串。 */
    const char *a2_out[] = {"done"};
    const char *b2_in[] = {"A.done"};
    const char *b2_out[] = {"b.done"};
    airy_task_node_t *a2 = make_node("A", "step a", "executor", NULL, 0, a2_out, 1, 10, 0);
    /* 元数据完备判据：声明依赖的节点必须声明其消费的产物（依赖图与
     * 数据流图一致），否则 E-05 正确拒收。 */
    airy_task_node_t *b2 = make_node("B", "step b", "executor", b2_in, 1, b2_out, 1, 10, 0);
    add_dep(b2, "A");
    ok->task_plan_nodes[ok->task_plan_node_count++] = a2;
    ok->task_plan_nodes[ok->task_plan_node_count++] = b2;
    __builtin_memset(&report, 0, sizeof(report));
    /* C5 any 语义：全节点元数据完备（outputs+cost）→ E-05 不触发；
     * 本用例仅验证拓扑序（acyclic）与参与集全量受检。 */
    airy_grad_budget_t budget = {600000, 1024};
    err = airy_grad_verify_plan(ok, &budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK && report.causal_checked == 2 &&
        report.metadata_ok == 2) {
        TEST_PASS("E-02 acyclic plan passes");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d checked=%u meta=%u",
                 (int)err, (int)report.error, (unsigned)report.causal_checked,
                 (unsigned)report.metadata_ok);
        TEST_FAIL("E-02 acyclic", msg);
    }

    free_plan(ok);
    free_plan(plan);
}

void test_e01_causal(void)
{

    const char *a_out[] = {"data"};
    const char *b_out[] = {"result"};
    airy_task_plan_t *ok = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    ok->task_plan_id = AIRY_STRDUP("causal_ok");
    ok->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    airy_task_node_t *a = make_node("A", "produce", "executor", NULL, 0, a_out, 1, 10, 0);
    const char *b_in[] = {"data"};
    airy_task_node_t *b = make_node("B", "consume", "executor", b_in, 1, b_out, 1, 10, 0);
    add_dep(b, "A");
    ok->task_plan_nodes[ok->task_plan_node_count++] = a;
    ok->task_plan_nodes[ok->task_plan_node_count++] = b;

    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan(ok, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK && report.causal_checked == 2 &&
        report.metadata_ok == 2) {
        TEST_PASS("E-01 valid producer chain passes");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d checked=%u meta=%u",
                 (int)err, (int)report.error, (unsigned)report.causal_checked,
                 (unsigned)report.metadata_ok);
        TEST_FAIL("E-01 valid chain", msg);
    }
    free_plan(ok);

    /* 自引用判裂（C5 先检后并）：节点消费自己的产物、不声明依赖 —
     * 复合形态要求 producer 在 depends 传递闭包内，自身被排除。 */
    const char *d_in[] = {"D.art"};
    const char *d_out[] = {"art"};
    airy_task_plan_t *selfref = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    selfref->task_plan_id = AIRY_STRDUP("causal_selfref");
    selfref->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    airy_task_node_t *d = make_node("D", "self consume", "executor", d_in, 1, d_out, 1, 10, 0);
    selfref->task_plan_nodes[selfref->task_plan_node_count++] = d;
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(selfref, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E01_CAUSAL_BREAK &&
        strcmp(report.missing_artifact, "D.art") == 0) {
        TEST_PASS("E-01 self-reference rejected (check-before-merge)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected E01 selfref, got err=%d error=%d missing=%s",
                 (int)err, (int)report.error, report.missing_artifact);
        TEST_FAIL("E-01 self-reference", msg);
    }
    free_plan(selfref);

    /* 复合形态闭包约束：Q 消费 P.art 但未声明对 P 的依赖 → 判裂；
     * 补上依赖后同图通过（声明依赖图与数据流图必须一致）。 */
    const char *p_out[] = {"art"};
    const char *q_in[] = {"P.art"};
    const char *q_out[] = {"q.done"};
    airy_task_plan_t *comp = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    comp->task_plan_id = AIRY_STRDUP("causal_composite");
    comp->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    airy_task_node_t *p = make_node("P", "produce art", "executor", NULL, 0, p_out, 1, 10, 0);
    airy_task_node_t *q = make_node("Q", "consume art", "executor", q_in, 1, q_out, 1, 10, 0);
    comp->task_plan_nodes[comp->task_plan_node_count++] = p;
    comp->task_plan_nodes[comp->task_plan_node_count++] = q;
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(comp, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E01_CAUSAL_BREAK &&
        strcmp(report.affected_node, "Q") == 0) {
        TEST_PASS("E-01 composite input without declared dep rejected");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected E01 node=Q, got err=%d error=%d node=%s", (int)err,
                 (int)report.error, report.affected_node);
        TEST_FAIL("E-01 composite no-dep", msg);
    }
    add_dep(q, "P");
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(comp, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK) {
        TEST_PASS("E-01 composite input with declared dep passes");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d", (int)err, (int)report.error);
        TEST_FAIL("E-01 composite with-dep", msg);
    }
    free_plan(comp);

    const char *c_in[] = {"missing"};
    const char *c_out[] = {"c.done"};
    airy_task_plan_t *bad = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    bad->task_plan_id = AIRY_STRDUP("causal_bad");
    bad->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    airy_task_node_t *c = make_node("C", "consume missing", "executor", c_in, 1, c_out, 1, 10, 0);
    bad->task_plan_nodes[bad->task_plan_node_count++] = c;

    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(bad, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E01_CAUSAL_BREAK &&
        strcmp(report.missing_artifact, "missing") == 0 &&
        (report.errors & AIRY_GRAD_ERR_BIT(AIRY_GRAD_E01_CAUSAL_BREAK))) {
        TEST_PASS("E-01 dangling input detected (missing artifact)");
    } else {
        char msg[160];
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
        char out[16];
        snprintf(id, sizeof(id), "N%d", i);
        snprintf(out, sizeof(out), "n%d.done", i);
        const char *outs[] = {out};
        /* C5 any 语义：节点须元数据完备，否则 E-05（优先级高于 E-03）遮蔽。 */
        plan->task_plan_nodes[plan->task_plan_node_count++] =
            make_node(id, id, "executor", NULL, 0, outs, 1, 40, 0);
    }

    airy_grad_budget_t budget = {.time_ms = 100, .mem_mb = 0};
    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan(plan, &budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E03_RESOURCE_OVER &&
        report.cost_sum_ms == 120 && report.cost_checked == 3) {
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
    const char *c_out[] = {"z"};

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    plan->task_plan_id = AIRY_STRDUP("scope_plan");
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(3, sizeof(airy_task_node_t *));
    airy_task_node_t *a = make_node("A", "produce x", "executor", NULL, 0, a_out, 1, 10, 0);
    airy_task_node_t *b = make_node("B", "x to y", "executor", b_in, 1, b_out, 1, 10, 0);
    airy_task_node_t *c = make_node("C", "consume y", "executor", c_in, 1, c_out, 1, 10, 0);
    add_dep(b, "A");
    add_dep(c, "B");
    plan->task_plan_nodes[plan->task_plan_node_count++] = a;
    plan->task_plan_nodes[plan->task_plan_node_count++] = b;
    plan->task_plan_nodes[plan->task_plan_node_count++] = c;

    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_scope(plan, NULL, NULL, 0, &report);
    /* C5 any 语义：全量模式参与集 = 全体非 NULL 节点，三节点元数据完备
     * → causal/metadata 计数均为 3（checked_nodes 为兼容别名）。 */
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK && report.checked_nodes == 3 &&
        report.causal_checked == 3 && report.metadata_ok == 3) {
        TEST_PASS("full-scope verify checks 3 nodes");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected OK checked=3, got err=%d error=%d checked=%u",
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
    /* C5 any 语义：参与集 2 节点全受检（因果平凡通过亦计），元数据覆盖
     * 为零 → 拒收，杜绝空壳计划的假收敛。 */
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E05_VERIFY_INSUFFICIENT &&
        report.causal_checked == 2 && report.metadata_ok == 0) {
        TEST_PASS("empty-shell plan rejected (E-05, no false convergence)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected E-05, got err=%d error=%d causal=%u meta=%u",
                 (int)err, (int)report.error, (unsigned)report.causal_checked,
                 (unsigned)report.metadata_ok);
        TEST_FAIL("E-05 empty-shell", msg);
    }
    free_plan(shell);

    /* 对照（C5 any 语义收紧）：仅带 cost 无 outputs → 元数据仍不完备
     * → E-05 依旧拒收（旧判据 cost>0 即放行，已封堵）。 */
    airy_task_plan_t *costed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    costed->task_plan_id = AIRY_STRDUP("costed_plan");
    costed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    costed->task_plan_nodes[costed->task_plan_node_count++] =
        make_node("C1", "c", "executor", NULL, 0, NULL, 0, 10, 0);
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(costed, &budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E05_VERIFY_INSUFFICIENT &&
        report.causal_checked == 1 && report.metadata_ok == 0) {
        TEST_PASS("cost-only plan still rejected (E-05 metadata gate)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected E-05, got err=%d error=%d causal=%u meta=%u",
                 (int)err, (int)report.error, (unsigned)report.causal_checked,
                 (unsigned)report.metadata_ok);
        TEST_FAIL("E-05 cost-only", msg);
    }
    free_plan(costed);

    /* 对照：元数据完备（outputs + cost）→ 通过且计数齐全。 */
    const char *full_out[] = {"full.done"};
    airy_task_plan_t *full = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    full->task_plan_id = AIRY_STRDUP("full_plan");
    full->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    full->task_plan_nodes[full->task_plan_node_count++] =
        make_node("F1", "f", "executor", NULL, 0, full_out, 1, 10, 0);
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(full, &budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK && report.causal_checked == 1 &&
        report.metadata_ok == 1 && report.checked_nodes == 1) {
        TEST_PASS("metadata-complete plan verified (E-05 not triggered)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d causal=%u meta=%u",
                 (int)err, (int)report.error, (unsigned)report.causal_checked,
                 (unsigned)report.metadata_ok);
        TEST_FAIL("E-05 complete control", msg);
    }
    free_plan(full);
}

void test_e04_guard_drift(void)
{
    /* PASS：guard 节点 outputs（"deploy.plan"）与 goal 关键词（deploy/
     * the/web/service，仅 >2 字符入集）有实体交集 → 判过。 */
    airy_gccp_goal_t goal;
    __builtin_memset(&goal, 0, sizeof(goal));
    goal.goal_endpoint = (char *)"deploy the web service";

    const char *g_out[] = {"deploy.plan"};
    airy_task_plan_t *aligned = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    aligned->task_plan_id = AIRY_STRDUP("guard_pass");
    aligned->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    airy_task_node_t *g1 = make_node("G1", "plan deploy", "guard", NULL, 0, g_out, 1, 10, 1);
    aligned->task_plan_nodes[aligned->task_plan_node_count++] = g1;
    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan_g(aligned, NULL, &goal, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK &&
        report.guard_state == AIRY_GRAD_GUARD_PASS && report.invariant_guards == 1 &&
        report.metadata_ok == 1) {
        TEST_PASS("E-04 guard outputs overlap goal entities (PASS)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected OK/PASS, got err=%d error=%d guard=%u", (int)err,
                 (int)report.error, (unsigned)report.guard_state);
        TEST_FAIL("E-04 pass", msg);
    }
    free_plan(aligned);

    /* FAIL：guard outputs 与 goal 零交集 → purpose drift；节点须元数据
     * 完备，否则更高优先级的 E-05 会遮蔽目标错误。 */
    const char *f_out[] = {"quantum.flux"};
    airy_task_plan_t *drift = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    drift->task_plan_id = AIRY_STRDUP("guard_fail");
    drift->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    airy_task_node_t *g2 = make_node("G2", "guard entity", "guard", NULL, 0, f_out, 1, 10, 1);
    drift->task_plan_nodes[drift->task_plan_node_count++] = g2;
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan_g(drift, NULL, &goal, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_E04_GUARD_DRIFT &&
        report.guard_state == AIRY_GRAD_GUARD_FAIL &&
        strcmp(report.affected_node, "G2") == 0 &&
        (report.errors & AIRY_GRAD_ERR_BIT(AIRY_GRAD_E04_GUARD_DRIFT))) {
        TEST_PASS("E-04 purpose drift detected (zero entity overlap)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected E04, got err=%d error=%d guard=%u node=%s",
                 (int)err, (int)report.error, (unsigned)report.guard_state,
                 report.affected_node);
        TEST_FAIL("E-04 drift", msg);
    }
    free_plan(drift);

    /* NONE：goal 缺席 → 不判定（交语义层 B 仲裁），仅计 guard 数。 */
    airy_task_plan_t *nogoal = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    nogoal->task_plan_id = AIRY_STRDUP("guard_nogoal");
    nogoal->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    airy_task_node_t *g3 = make_node("G3", "guard entity", "guard", NULL, 0, g_out, 1, 10, 1);
    nogoal->task_plan_nodes[nogoal->task_plan_node_count++] = g3;
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan_g(nogoal, NULL, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK &&
        report.guard_state == AIRY_GRAD_GUARD_NONE && report.invariant_guards == 1) {
        TEST_PASS("E-04 without goal degrades to guard counting (NONE)");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected OK/NONE, got err=%d error=%d guard=%u", (int)err,
                 (int)report.error, (unsigned)report.guard_state);
        TEST_FAIL("E-04 none", msg);
    }
    free_plan(nogoal);
}

/* Gate 32：位图 → 单值裁决的优先级梯。梯序为 E-02 > E-05 > E-01 >
 * E-03 > E-04，故任意两位同置时，下标较小者胜。10 组两两冲突 +
 * 5 个单位 + 全位置位 + 零位，全部由同一个纯函数判定。 */
void test_err_priority(void)
{
    const airy_grad_error_t order[] = {AIRY_GRAD_E02_DEADLOCK, AIRY_GRAD_E05_VERIFY_INSUFFICIENT,
                                       AIRY_GRAD_E01_CAUSAL_BREAK, AIRY_GRAD_E03_RESOURCE_OVER,
                                       AIRY_GRAD_E04_GUARD_DRIFT};
    const char *names[] = {"E-02", "E-05", "E-01", "E-03", "E-04"};
    char msg[192];
    int bad = 0;
    uint32_t all = 0;

    for (int i = 0; i < 5; i++) {
        all |= AIRY_GRAD_ERR_BIT(order[i]);
        if (grad_err_rank(AIRY_GRAD_ERR_BIT(order[i])) != order[i]) {
            snprintf(msg, sizeof(msg), "single %s lost", names[i]);
            bad++;
        }
        for (int j = i + 1; j < 5; j++) {
            uint32_t pair = AIRY_GRAD_ERR_BIT(order[i]) | AIRY_GRAD_ERR_BIT(order[j]);
            if (grad_err_rank(pair) != order[i]) {
                snprintf(msg, sizeof(msg), "%s vs %s: lower rank won", names[i], names[j]);
                bad++;
            }
        }
    }
    if (grad_err_rank(all) != AIRY_GRAD_E02_DEADLOCK) {
        snprintf(msg, sizeof(msg), "all-five: top of ladder not returned");
        bad++;
    }
    if (grad_err_rank(0) != AIRY_GRAD_OK) {
        snprintf(msg, sizeof(msg), "empty bitmap: expected OK");
        bad++;
    }

    if (bad == 0)
        TEST_PASS("priority ladder E-02 > E-05 > E-01 > E-03 > E-04 (10 pairs + edges)");
    else
        TEST_FAIL("err priority ladder", msg);
}
