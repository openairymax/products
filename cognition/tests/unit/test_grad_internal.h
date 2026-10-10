// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_internal.h
 *   GRAD 协议测试拆分后的共享内部头（test_grad.c 主文件与四个域文件共用）
 *
 * 共享内容：测试断言宏、全局计数、辅助函数与 mock 声明。
 * 按功能域拆分：
 *   - test_grad_verifiers.c：E-01/E-02/E-03/E-05 验证器 + 验证范围
 *   - test_grad_coordinator.c：协调器（seed 收敛 / 驳回再生成 / fail-closed）
 *   - test_grad_arbiter.c：B 终裁（置信度降级 / E-04 目的漂移 / 畸形输入）
 *   - test_grad_misc.c：驳回补丁 / 边界条件 / 决策链事件流
 */

#ifndef TEST_GRAD_INTERNAL_H
#define TEST_GRAD_INTERNAL_H

#include "airy_rt.h"
#include "cognition.h"
#include "gccp.h"
#include "grad_verifier.h"
#include "grad_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)
#define TEST_FAIL(name, msg)                    \
    do {                                        \
        printf("  [FAIL] %s: %s\n", name, msg); \
        tests_failed++;                         \
    } while (0)

#define RUN_TEST(func)                   \
    do {                                 \
        printf("\n--- %s ---\n", #func); \
        tests_run++;                     \
        int prev_failed = tests_failed;  \
        func();                          \
        if (prev_failed == tests_failed) \
            tests_passed++;              \
    } while (0)

/* 全局计数（定义于 test_grad.c 主文件） */
extern int tests_run;
extern int tests_passed;
extern int tests_failed;

/* 辅助函数（定义于 test_grad.c 主文件） */
airy_task_node_t *make_node(const char *id, const char *goal, const char *role,
                            const char **inputs, size_t input_count, const char **outputs,
                            size_t output_count, int64_t cost_ms, uint8_t invariant_guard);
void add_dep(airy_task_node_t *n, const char *dep_id);
void free_plan(airy_task_plan_t *plan);

/* 共享 mock：S2 计划生成器（定义于 test_grad.c 主文件） */
typedef struct {
    int s2_call_count;
} grad_mock_ctx_t;

airy_err_t grad_mock_s2(const airy_gccp_goal_t *goal, const char *patch_json,
                        airy_task_plan_t **out_plan, void *user_data);
airy_err_t grad_mock_s2_bad(const airy_gccp_goal_t *goal, const char *patch_json,
                            airy_task_plan_t **out_plan, void *user_data);

/* 共享 mock：B 终裁器与上下文（定义于 test_grad.c 主文件） */
typedef struct {
    const char *verdict_json;   /* 固定返回的终裁 JSON（NULL = 采纳 C） */
    const char *verdict_json_2; /* 第二次调用起返回（NULL = 始终复用 verdict_json） */
    int call_count;
} grad_arb_ctx_t;

airy_err_t grad_mock_arbiter(const airy_gccp_goal_t *goal, const airy_grad_report_t *report,
                             const airy_task_plan_t *plan, char **out_verdict,
                             void *user_data);

/* 终裁用例辅助（定义于 test_grad_arbiter.c） */
airy_task_plan_t *make_seed_single(const char *id, const char *plan_id);
void run_arb_verdict(const char *verdict_json, int expect_fix_loop, const char *case_name);

/* 各域测试函数（供 main 注册） */
void test_e02_deadlock(void);
void test_e01_causal(void);
void test_e03_resource(void);
void test_verify_scope(void);
void test_e05_verify_insufficient(void);
void test_coordinator_seed_converge(void);
void test_coordinator_reject_regenerate(void);
void test_coordinator_failclosed_nonconverged(void);
void test_arbiter_conf_degrade(void);
void test_arbiter_e04_purpose_drift(void);
void test_arbiter_malformed_verdicts(void);
void test_build_patch(void);
void test_edge_cases(void);
void test_progress_events(void);

#endif /* TEST_GRAD_INTERNAL_H */
