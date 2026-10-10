// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_arbiter.c
 *   GRAD B 终裁测试域：置信度降级（低置信 reject 忽略 / 高置信触发修复）、
 *   E-04 目的漂移守门（e04=fail 硬拒绝）、畸形终裁输入（markdown 围栏、
 *   纯文本、类型异常 confidence、空串）健壮性。
 */

#include "test_grad_internal.h"

airy_task_plan_t *make_seed_single(const char *id, const char *plan_id)
{
    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!seed)
        return NULL;
    seed->task_plan_id = AIRY_STRDUP(plan_id);
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    if (!seed->task_plan_nodes) {
        AIRY_FREE(seed->task_plan_id);
        AIRY_FREE(seed);
        return NULL;
    }
    seed->task_plan_nodes[seed->task_plan_node_count++] =
        make_node(id, "execute", "executor", NULL, 0, NULL, 0, 10, 0);
    seed->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (seed->task_plan_entry_points) {
        seed->task_plan_entry_points[0] = AIRY_STRDUP(id);
        seed->task_plan_entry_count = 1;
    }
    return seed;
}

void run_arb_verdict(const char *verdict_json, int expect_fix_loop, const char *case_name)
{
    airy_task_plan_t *seed = make_seed_single("S_01", "seed_mal");
    if (!seed) {
        TEST_FAIL(case_name, "seed alloc failed");
        return;
    }
    grad_mock_ctx_t s2mc;
    __builtin_memset(&s2mc, 0, sizeof(s2mc));
    grad_arb_ctx_t amc = {.verdict_json = verdict_json};

    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.s2_plan = grad_mock_s2;
    cfg.s1_arbiter = grad_mock_arbiter;
    cfg.s2_user_data = &s2mc;
    cfg.s1_user_data = &amc;

    airy_grad_coordinator_t *coord = NULL;
    airy_err_t err = airy_grad_coordinator_create(&cfg, &coord);
    if (err != AIRY_SUCCESS || !coord) {
        TEST_FAIL(case_name, "create failed");
        free_plan(seed);
        return;
    }
    airy_task_plan_t *final = NULL;
    airy_grad_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    err = airy_grad_coordinator_execute(coord, NULL, seed, &final, &stats);

    char msg[200];
    if (expect_fix_loop) {
        /* 高置信 reject（字符串或数字）→ 修复循环触发，但 mock 恒 reject
         * → 打满迭代未收敛（ETIMEDOUT）+ 未收敛 owned 计划丢弃。 */
        if (s2mc.s2_call_count >= 1 && final == NULL && err == AIRY_ETIMEDOUT) {
            TEST_PASS(case_name);
        } else {
            snprintf(msg, sizeof(msg), "expected fix loop, got s2=%d err=%d final=%p",
                     s2mc.s2_call_count, (int)err, (void *)final);
            TEST_FAIL(case_name, msg);
        }
    } else {
        /* defer to C → 收敛，无再生成，seed 原样返回（BORROW → final==NULL）*/
        if (err == AIRY_SUCCESS && stats.converged && s2mc.s2_call_count == 0 &&
            final == NULL) {
            TEST_PASS(case_name);
        } else {
            snprintf(msg, sizeof(msg),
                     "expected converge+defer, got err=%d conv=%d s2=%d final=%p", (int)err,
                     stats.converged, s2mc.s2_call_count, (void *)final);
            TEST_FAIL(case_name, msg);
        }
    }
    if (final)
        free_plan(final);
    airy_grad_coordinator_destroy(coord);
    free_plan(seed);
}

void test_arbiter_conf_degrade(void)
{
    /* 有效 seed（单节点，E-01/E-02/E-03 全过）*/
    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    seed->task_plan_id = AIRY_STRDUP("seed_conf");
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    seed->task_plan_nodes[seed->task_plan_node_count++] =
        make_node("S_01", "execute", "executor", NULL, 0, NULL, 0, 10, 0);
    seed->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (seed->task_plan_entry_points) {
        seed->task_plan_entry_points[0] = AIRY_STRDUP("S_01");
        seed->task_plan_entry_count = 1;
    }

    /* 低置信度 reject（0.3 < 0.55）→ 忽略终裁、采纳 C → 直接收敛，不触发修复 */
    grad_mock_ctx_t s2mc;
    __builtin_memset(&s2mc, 0, sizeof(s2mc));
    grad_arb_ctx_t amc = {.verdict_json = "{\"verdict\":\"reject\",\"confidence\":0.3}"};

    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.s2_plan = grad_mock_s2;
    cfg.s1_arbiter = grad_mock_arbiter;
    cfg.s2_user_data = &s2mc;
    cfg.s1_user_data = &amc;

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
    if (err == AIRY_SUCCESS && stats.converged && s2mc.s2_call_count == 0 && final == NULL) {
        TEST_PASS("low-confidence reject ignored (adopts Model C verdict)");
    } else {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "expected converged+no-regen, got err=%d conv=%d s2=%d final=%p", (int)err,
                 stats.converged, s2mc.s2_call_count, (void *)final);
        TEST_FAIL("arbiter conf degrade", msg);
    }
    if (final)
        free_plan(final);
    airy_grad_coordinator_destroy(coord);

    /* 高置信度 reject（0.9 ≥ 0.55）→ 终裁生效 → 触发修复循环 */
    __builtin_memset(&s2mc, 0, sizeof(s2mc));
    amc.verdict_json = "{\"verdict\":\"reject\",\"confidence\":0.9}";
    coord = NULL;
    err = airy_grad_coordinator_create(&cfg, &coord);
    if (err != AIRY_SUCCESS || !coord) {
        TEST_FAIL("coordinator create", "create failed");
        free_plan(seed);
        return;
    }
    final = NULL;
    __builtin_memset(&stats, 0, sizeof(stats));
    err = airy_grad_coordinator_execute(coord, NULL, seed, &final, &stats);
    /* mock 永远 reject → 打满 max_iterations 未收敛（ETIMEDOUT），但修复
     * 循环确实被触发（s2 ≥ 1）。核心验证：高置信度 reject 未被忽略，且
     * 未收敛时 owned 计划被丢弃（final==NULL，fail-closed 契约）。 */
    if (s2mc.s2_call_count >= 1 && final == NULL && err == AIRY_ETIMEDOUT) {
        TEST_PASS("high-confidence reject triggers fix loop");
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected fix loop, got s2=%d err=%d final=%p",
                 s2mc.s2_call_count, (int)err, (void *)final);
        TEST_FAIL("arbiter conf degrade", msg);
    }
    if (final)
        free_plan(final);
    airy_grad_coordinator_destroy(coord);
    free_plan(seed);
}

void test_arbiter_e04_purpose_drift(void)
{
    /* seed 含 invariant_guard 节点（S_01 guard=1），E-01/E-02/E-03 全过 */
    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    seed->task_plan_id = AIRY_STRDUP("seed_e04");
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    seed->task_plan_nodes[seed->task_plan_node_count++] =
        make_node("S_01", "execute", "executor", NULL, 0, NULL, 0, 10, 1);
    seed->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (seed->task_plan_entry_points) {
        seed->task_plan_entry_points[0] = AIRY_STRDUP("S_01");
        seed->task_plan_entry_count = 1;
    }

    /* 第一次终裁 e04=fail（目的漂移硬信号，verdict 文本却是 accept）→ 必须
     * reject → 触发修复；第二次 e04=pass → 收敛。 */
    grad_mock_ctx_t s2mc;
    __builtin_memset(&s2mc, 0, sizeof(s2mc));
    grad_arb_ctx_t amc = {
        .verdict_json = "{\"verdict\":\"accept\",\"confidence\":0.9,\"e04\":\"fail\"}",
        .verdict_json_2 = "{\"verdict\":\"accept\",\"confidence\":0.9,\"e04\":\"pass\"}",
    };

    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.max_iterations = 3;
    cfg.s2_plan = grad_mock_s2;
    cfg.s1_arbiter = grad_mock_arbiter;
    cfg.s2_user_data = &s2mc;
    cfg.s1_user_data = &amc;

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
    if (err == AIRY_SUCCESS && stats.converged && s2mc.s2_call_count >= 1 && final != NULL &&
        stats.rejections >= 1) {
        TEST_PASS("e04=fail rejects (purpose drift) and e04=pass converges");
    } else {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "expected fix loop then converge, got err=%d conv=%d s2=%d rej=%u final=%p",
                 (int)err, stats.converged, s2mc.s2_call_count, (unsigned)stats.rejections,
                 (void *)final);
        TEST_FAIL("arbiter e04 drift", msg);
    }
    if (final)
        free_plan(final);
    airy_grad_coordinator_destroy(coord);
    free_plan(seed);
}

void test_arbiter_malformed_verdicts(void)
{
    /* 1. markdown 围栏包裹（LLM 常见违规输出）→ 解析失败 → defer to C */
    run_arb_verdict("```json\n{\"verdict\":\"reject\",\"confidence\":0.9}\n```", 0,
                    "malformed markdown fence defers to C");

    /* 2. 纯文本（无 JSON）→ 解析失败 → defer to C */
    run_arb_verdict("I think this plan is fine, no JSON here", 0,
                    "plain text verdict defers to C");

    /* 3. verdict 空串 → cJSON_Parse 失败 → defer to C */
    run_arb_verdict("", 0, "empty verdict defers to C");

    /* 4. confidence 字符串 "0.9"（LLM 常见）→ atof → 高置信 → 修复循环 */
    run_arb_verdict("{\"verdict\":\"reject\",\"confidence\":\"0.9\"}", 1,
                    "string confidence parsed (high conf reject)");

    /* 5. confidence 数组类型 → 非法 → 0.0（θ_B 保护）→ 忽略 reject → 收敛 */
    run_arb_verdict("{\"verdict\":\"reject\",\"confidence\":[0.9]}", 0,
                    "array confidence treated as 0.0 (ignored)");

    /* 6. verdict 缺失 → 不 reject → 采纳 C → 收敛 */
    run_arb_verdict("{\"confidence\":0.9}", 0, "missing verdict converges");
}
