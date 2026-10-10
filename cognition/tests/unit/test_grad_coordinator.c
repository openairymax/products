// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_coordinator.c
 *   GRAD 协调器测试域：seed 计划收敛、四验被拒后模型 A 再生成收敛、
 *   未收敛 fail-closed（owned 计划丢弃 + AIRY_ETIMEDOUT）。
 */

#include "test_grad_internal.h"

void test_coordinator_seed_converge(void)
{

    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    seed->task_plan_id = AIRY_STRDUP("seed");
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    seed->task_plan_nodes[seed->task_plan_node_count++] =
        make_node("S_01", "execute", "executor", NULL, 0, NULL, 0, 10, 0);
    seed->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (seed->task_plan_entry_points) {
        seed->task_plan_entry_points[0] = AIRY_STRDUP("S_01");
        seed->task_plan_entry_count = 1;
    }

    grad_mock_ctx_t mc;
    __builtin_memset(&mc, 0, sizeof(mc));

    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.s2_plan = grad_mock_s2;
    cfg.s2_user_data = &mc;

    airy_grad_coordinator_t *coord = NULL;
    airy_err_t err = airy_grad_coordinator_create(&cfg, &coord);
    if (err != AIRY_SUCCESS || !coord) {
        TEST_FAIL("coordinator create", "create failed");
        free_plan(seed);
        return;
    }

    airy_task_plan_t *final = NULL;
    airy_grad_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    err = airy_grad_coordinator_execute(coord, NULL, seed, &final, &stats);
    if (err == AIRY_SUCCESS && stats.converged && final == NULL && mc.s2_call_count == 0) {
        TEST_PASS("seed converges without regeneration");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "expected converged+NULL+no-s2, got err=%d conv=%d s2=%d final=%p", (int)err,
                 stats.converged, mc.s2_call_count, (void *) final);
        TEST_FAIL("seed converge", msg);
    }
    airy_grad_coordinator_destroy(coord);
    free_plan(seed);
}

void test_coordinator_reject_regenerate(void)
{

    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    seed->task_plan_id = AIRY_STRDUP("bad_seed");
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    airy_task_node_t *a = make_node("A", "a", "executor", NULL, 0, NULL, 0, 10, 0);
    airy_task_node_t *b = make_node("B", "b", "executor", NULL, 0, NULL, 0, 10, 0);
    add_dep(a, "B");
    add_dep(b, "A");
    seed->task_plan_nodes[seed->task_plan_node_count++] = a;
    seed->task_plan_nodes[seed->task_plan_node_count++] = b;

    grad_mock_ctx_t mc;
    __builtin_memset(&mc, 0, sizeof(mc));

    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.max_iterations = 2;
    cfg.s2_plan = grad_mock_s2;
    cfg.s2_user_data = &mc;

    airy_grad_coordinator_t *coord = NULL;
    airy_err_t err = airy_grad_coordinator_create(&cfg, &coord);
    if (err != AIRY_SUCCESS || !coord) {
        TEST_FAIL("coordinator create", "create failed");
        free_plan(seed);
        return;
    }

    airy_task_plan_t *final = NULL;
    airy_grad_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    err = airy_grad_coordinator_execute(coord, NULL, seed, &final, &stats);
    if (err == AIRY_SUCCESS && stats.converged && final && mc.s2_call_count >= 1) {
        TEST_PASS("rejected seed regenerated and converged");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected converged+regen, got err=%d conv=%d s2=%d final=%p",
                 (int)err, stats.converged, mc.s2_call_count, (void *) final);
        TEST_FAIL("reject regen", msg);
    }
    airy_grad_coordinator_destroy(coord);
    if (final)
        free_plan(final);
    free_plan(seed);
}

void test_coordinator_failclosed_nonconverged(void)
{
    /* 坏 seed（E-02 环）+ s2 恒生成坏计划 → 永不收敛 → 必须 fail-closed */
    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    seed->task_plan_id = AIRY_STRDUP("bad_seed_fc");
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    airy_task_node_t *a = make_node("A", "a", "executor", NULL, 0, NULL, 0, 10, 0);
    airy_task_node_t *b = make_node("B", "b", "executor", NULL, 0, NULL, 0, 10, 0);
    add_dep(a, "B");
    add_dep(b, "A");
    seed->task_plan_nodes[seed->task_plan_node_count++] = a;
    seed->task_plan_nodes[seed->task_plan_node_count++] = b;

    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.max_iterations = 3;
    cfg.s2_plan = grad_mock_s2_bad;

    airy_grad_coordinator_t *coord = NULL;
    airy_err_t err = airy_grad_coordinator_create(&cfg, &coord);
    if (err != AIRY_SUCCESS || !coord) {
        TEST_FAIL("fail-closed", "create failed");
        free_plan(seed);
        return;
    }
    airy_task_plan_t *final = NULL;
    airy_grad_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    err = airy_grad_coordinator_execute(coord, NULL, seed, &final, &stats);
    if (err == AIRY_ETIMEDOUT && !stats.converged && final == NULL) {
        TEST_PASS("non-converged owned plan discarded (fail-closed)");
    } else {
        char msg[192];
        snprintf(msg, sizeof(msg), "expected ETIMEDOUT+final=NULL, got err=%d conv=%d final=%p",
                 (int)err, stats.converged, (void *)final);
        TEST_FAIL("fail-closed", msg);
    }
    if (final)
        free_plan(final);
    airy_grad_coordinator_destroy(coord);
    free_plan(seed);
}
