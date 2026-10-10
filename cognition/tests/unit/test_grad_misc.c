// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_misc.c
 *   GRAD 杂项测试域：驳回补丁构造与 JSON 序列化、边界条件健壮性、
 *   决策链事件流（progress_cb）可见性。
 */

#include "test_grad_internal.h"

/* 决策链事件流回调上下文（progress_cb，test_progress_events 使用） */
typedef struct {
    int s2_done;
    int verify_start;
    int verify_done;
    int done;
} grad_prog_ctx_t;

static void grad_prog_cb(int level, const char *event, const char *data, void *user_data)
{
    grad_prog_ctx_t *pc = (grad_prog_ctx_t *)user_data;
    (void)level;
    (void)data;
    if (!pc || !event)
        return;
    if (strcmp(event, "grad_s2_done") == 0)
        pc->s2_done++;
    else if (strcmp(event, "grad_verify_start") == 0)
        pc->verify_start++;
    else if (strcmp(event, "grad_verify_done") == 0)
        pc->verify_done++;
    else if (strcmp(event, "grad_done") == 0)
        pc->done++;
}

void test_build_patch(void)
{
    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    report.error = AIRY_GRAD_E01_CAUSAL_BREAK;
    snprintf(report.affected_node, sizeof(report.affected_node), "S_05");
    snprintf(report.missing_artifact, sizeof(report.missing_artifact), "Compiled_Binary.exe");

    char *patch = NULL;
    airy_err_t err = airy_grad_build_patch(&report, 2, &patch);
    if (err == AIRY_SUCCESS && patch && strstr(patch, "\"error_code\":\"E-01\"") &&
        strstr(patch, "\"affected_scope\":[\"S_05\"]") && strstr(patch, "Compiled_Binary.exe")) {
        TEST_PASS("E-01 rejection patch matches GRAD spec");
    } else {
        TEST_FAIL("build patch", patch ? patch : "NULL patch");
    }
    if (patch)
        AIRY_FREE(patch);

    __builtin_memset(&report, 0, sizeof(report));
    report.error = AIRY_GRAD_E02_DEADLOCK;
    snprintf(report.cycle_path, sizeof(report.cycle_path), "A -> B -> A");
    char *json = NULL;
    err = airy_grad_report_to_json(&report, &json);
    if (err == AIRY_SUCCESS && json && strstr(json, "E-02")) {
        TEST_PASS("report JSON serialization contains E-02");
    } else {
        TEST_FAIL("report JSON", json ? json : "NULL json");
    }
    if (json)
        AIRY_FREE(json);
}

void test_edge_cases(void)
{
    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan(NULL, NULL, &report);
    if (err == AIRY_EINVAL) {
        TEST_PASS("NULL plan rejected");
    } else {
        TEST_FAIL("NULL plan", "expected EINVAL");
    }

    airy_task_plan_t *empty = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(empty, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK) {
        TEST_PASS("empty plan passes");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d", (int)err, (int)report.error);
        TEST_FAIL("empty plan", msg);
    }
    free_plan(empty);

    airy_task_plan_t *ig = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    ig->task_plan_id = AIRY_STRDUP("ig_plan");
    ig->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    ig->task_plan_nodes[ig->task_plan_node_count++] =
        make_node("S_01", "guard", "executor", NULL, 0, NULL, 0, 10, 1);
    __builtin_memset(&report, 0, sizeof(report));
    airy_grad_budget_t ig_budget = {600000, 1024};
    err = airy_grad_verify_plan(ig, &ig_budget, &report);
    if (err == AIRY_SUCCESS && report.invariant_guards == 1) {
        TEST_PASS("invariant_guard counted for semantic review");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected 1 guard, got %u (err=%d)",
                 (unsigned)report.invariant_guards, (int)err);
        TEST_FAIL("invariant_guard", msg);
    }
    free_plan(ig);
}

void test_progress_events(void)
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

    grad_prog_ctx_t pc;
    __builtin_memset(&pc, 0, sizeof(pc));

    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.s2_plan = grad_mock_s2;
    cfg.progress_cb = grad_prog_cb;
    cfg.progress_user_data = &pc;

    airy_grad_coordinator_t *coord = NULL;
    airy_err_t err = airy_grad_coordinator_create(&cfg, &coord);
    if (err != AIRY_SUCCESS || !coord) {
        TEST_FAIL("progress events", "create failed");
        free_plan(seed);
        return;
    }

    airy_task_plan_t *final = NULL;
    airy_grad_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    err = airy_grad_coordinator_execute(coord, NULL, seed, &final, &stats);

    char msg[192];
    if (err == AIRY_SUCCESS && pc.s2_done >= 1 && pc.verify_start >= 1 && pc.verify_done >= 1 &&
        pc.done >= 1) {
        snprintf(msg, sizeof(msg), "s2=%d vstart=%d vdone=%d done=%d", pc.s2_done,
                 pc.verify_start, pc.verify_done, pc.done);
        TEST_PASS("progress events fired");
        printf("        %s\n", msg);
    } else {
        snprintf(msg, sizeof(msg), "err=%d s2=%d vstart=%d vdone=%d done=%d", (int)err, pc.s2_done,
                 pc.verify_start, pc.verify_done, pc.done);
        TEST_FAIL("progress events", msg);
    }
    airy_grad_coordinator_destroy(coord);
    free_plan(seed);
}
