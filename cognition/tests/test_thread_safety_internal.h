// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_thread_safety_internal.h
 *   ThreadSanitizer 线程安全验证测试（INT-02）拆分后的共享内部头
 *
 * 共享内容：RUN_TEST 宏、辅助函数与线程工作函数声明。
 * 按功能域拆分：
 *   - test_thread_safety_int0201.c：INT-02.1 并发认知引擎测试（4 用例）
 *   - test_thread_safety_int0202.c：INT-02.2 错误处理路径验证（6 用例）
 */

#ifndef TEST_THREAD_SAFETY_INTERNAL_H
#define TEST_THREAD_SAFETY_INTERNAL_H

#include "cognition.h"
#include "memory.h"
#include "airy_memory.h"
/* d8 清理：移除 sync_compat.h。本文件使用 airy_mtx_t + airy_mtx_* 函数（platform.h API），
 * 通过 airy_memory.h → error.h → types.h → platform.h 间接获得，无需 sync_compat.h。 */
#include "error.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUN_TEST(name)                      \
    do {                                    \
        printf("  Running " #name "...\n"); \
        test_##name();                      \
        printf("  PASSED\n");               \
    } while (0)

/* 辅助（定义于 test_thread_safety.c 主文件）
 * create_default_engine：注入 reactive 主策略 + reflective fallback（DT-01/DT-04）；
 *   out_fallback 非 NULL 时回传 fallback 句柄，调用方须在 drop_engine 中释放。 */
airy_cognition_engine_t *create_default_engine(airy_plan_strategy_t **out_fallback);
airy_plan_strategy_t *attach_fallback(airy_cognition_engine_t *engine);
void drop_engine(airy_cognition_engine_t *engine, airy_plan_strategy_t *fallback);
int is_valid_json_prefix(const char *str);
const char *error_str(airy_err_t err);

/* 线程工作函数与参数（定义于 test_thread_safety.c 主文件） */
typedef struct {
    int thread_id;
    const char *input;
    int expect_success;
    int *result_count;
    airy_mtx_t *result_mutex;
} thread_worker_args_t;

void *thread_worker(void *arg);

typedef struct {
    int thread_id;
    const char *input;
    int *result_count;
    airy_mtx_t *result_mutex;
} thread_feedback_worker_args_t;

void *thread_feedback_worker(void *arg);

typedef struct {
    int thread_id;
    const char *input;
    int *result_count;
    airy_mtx_t *result_mutex;
} thread_strategy_worker_args_t;

airy_err_t mock_coord_func(const char **prompts, size_t count, void *context, char **out_result);
void mock_coord_destroy(airy_coordinator_strategy_t *s);
airy_err_t mock_disp_func(const airy_task_node_t *task, const void **candidates, size_t count,
                          void *context, char **out_agent_id);
void mock_disp_destroy(airy_dispatching_strategy_t *s);
void *thread_strategy_worker(void *arg);

typedef enum { MIX_TYPE_BASIC = 0, MIX_TYPE_FEEDBACK = 1, MIX_TYPE_STRATEGY = 2 } mix_worker_type_t;

typedef struct {
    int thread_id;
    mix_worker_type_t type;
    const char *input;
    int *result_count;
    airy_mtx_t *result_mutex;
} mix_worker_args_t;

void *thread_mix_worker(void *arg);

/* 各域测试函数 */
void test_int02_1_concurrent_cognition_engines(void);
void test_int02_1_concurrent_feedback_engines(void);
void test_int02_1_concurrent_strategy_engines(void);
void test_int02_1_mixed_concurrent_engines(void);
void test_int02_2_null_input_handling(void);
void test_int02_2_null_engine_handling(void);
void test_int02_2_error_code_propagation(void);
void test_int02_2_create_ex_error_handling(void);
void test_int02_2_intent_parser_error_handling(void);
void test_int02_2_memory_engine_error_handling(void);

#endif /* TEST_THREAD_SAFETY_INTERNAL_H */
