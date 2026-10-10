// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * test_thread_safety.c - ThreadSanitizer 线程安全验证测试 (INT-02)——主文件
 *
 * 验证覆盖:
 *   INT-02.1: ThreadSanitizer-ready 并发测试 (coordinator + dispatcher)
 *   INT-02.2: 错误处理路径验证 (NULL 输入 / NULL 引擎 / 错误码传播)
 *
 * 编译方式:
 *   gcc -fsanitize=thread -g -O1 -pthread -I../include \
 *       -I../../../../commons/utils/error \
 *       -I../../../../commons/utils/types \
 *       test_thread_safety.c test_thread_safety_int0201.c \
 *       test_thread_safety_int0202.c -o test_thread_safety
 *
 * 该测试设计为通过 ThreadSanitizer 检测数据竞争。
 * 每个线程创建独立的认知引擎实例，避免共享状态竞争。
 * 若存在内部全局状态竞争，TSan 将在运行时报告。
 *
 * 主文件承载：SPDX 头、辅助函数与线程工作函数（经 test_thread_safety_internal.h）、
 * int main()。测试函数按功能域拆分：
 *   - test_thread_safety_int0201.c：INT-02.1 并发认知引擎测试
 *   - test_thread_safety_int0202.c：INT-02.2 错误处理路径验证
 */

#include "payload_registry.h"
#include "test_thread_safety_internal.h"

#include "plan_strategy.h"

/* ============================================================================
 * 策略注入: 按 DT-01/DT-04 注入调用方策略（机制与策略分离，机制核零策略载荷）
 *   - 主策略 reactive：启发式兜底，无 LLM 亦可产出有效计划。
 *     经 create 的 TRANSFER 通道交给引擎，由引擎在 destroy 时释放。
 *   - fallback 策略 reflective：经 set_fallback_plan 注入，属 BORROW ——
 *     引擎销毁不释放，调用方须在 airy_cognition_destroy 之后自行释放。
 * ============================================================================ */
static airy_plan_strategy_t *new_primary_strat(void)
{
    airy_plan_strategy_t *strat = airy_plan_reactive_create(NULL);
    assert(strat != NULL);
    return strat;
}

airy_plan_strategy_t *attach_fallback(airy_cognition_engine_t *engine)
{
    airy_plan_strategy_t *fallback = airy_plan_reflective_create(NULL, NULL);
    assert(fallback != NULL);
    airy_cognition_set_fallback_plan(engine, fallback);
    return fallback;
}

void drop_engine(airy_cognition_engine_t *engine, airy_plan_strategy_t *fallback)
{
    if (engine)
        airy_cognition_destroy(engine);
    if (fallback)
        fallback->destroy(fallback);
}

/* ============================================================================
 * 辅助: 创建默认认知引擎，失败时通过 assert 终止
 * ============================================================================ */
airy_cognition_engine_t *create_default_engine(airy_plan_strategy_t **out_fallback)
{
    airy_cognition_engine_t *engine = NULL;
    airy_err_t err = airy_cognition_create_take(new_primary_strat(), NULL, NULL, &engine);
    assert(err == AIRY_SUCCESS);
    assert(engine != NULL);
    if (out_fallback)
        *out_fallback = attach_fallback(engine);
    return engine;
}

/* ============================================================================
 * 辅助: feedback 回调（测试用 no-op，验证回调通道不崩溃）
 * ============================================================================ */
static void null_feedback_callback(int level, const char *module, const char *event,
                                   const char *data, size_t data_len, void *user_data)
{
    (void)level;
    (void)module;
    (void)event;
    (void)data;
    (void)data_len;
    (void)user_data;
}

/* ============================================================================
 * 辅助: 验证字符串是否为合法的 JSON 起始标记
 * ============================================================================ */
int is_valid_json_prefix(const char *str)
{
    if (!str || str[0] == '\0')
        return 0;
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
        str++;
    return (*str == '{' || *str == '[');
}

/* ============================================================================
 * 辅助: 错误码可读字符串
 * ============================================================================ */
const char *error_str(airy_err_t err)
{
    if (err == AIRY_SUCCESS)
        return "AIRY_SUCCESS";
    if (err == AIRY_EINVAL)
        return "AIRY_EINVAL";
    if (err == AIRY_ENOMEM)
        return "AIRY_ENOMEM";
    return "UNKNOWN";
}

/* ============================================================================
 * 线程工作函数: 单个线程执行完整生命周期
 * create → process → stats → destroy
 * 每个线程使用独立的引擎实例，TSan 可检测内部全局状态竞争
 * ============================================================================ */
void *thread_worker(void *arg)
{
    thread_worker_args_t *args = (thread_worker_args_t *)arg;
    int local_success = 0;

    printf("    [Thread %d] Starting...\n", args->thread_id);

    airy_cognition_engine_t *engine = NULL;
    airy_plan_strategy_t *fallback = NULL;
    airy_err_t err = airy_cognition_create_take(new_primary_strat(), NULL, NULL, &engine);
    if (err == AIRY_SUCCESS && engine != NULL) {
        fallback = attach_fallback(engine);
        printf("    [Thread %d] Engine created\n", args->thread_id);

        airy_task_plan_t *plan = NULL;
        err = airy_cognition_process(engine, args->input, strlen(args->input), &plan);
        if (err == AIRY_SUCCESS) {
            printf("    [Thread %d] Process succeeded: plan=%p, nodes=%zu\n", args->thread_id,
                   (void *)plan, plan ? plan->task_plan_node_count : 0);
            if (plan) {
                airy_task_plan_free(plan);
            }
            local_success = 1;
        } else {
            printf("    [Thread %d] Process failed: err=%s\n", args->thread_id, error_str(err));
        }

        char *stats = NULL;
        size_t stats_len = 0;
        err = airy_cognition_stats(engine, &stats, &stats_len);
        if (err == AIRY_SUCCESS && stats) {
            printf("    [Thread %d] Stats: %.80s\n", args->thread_id, stats);
            free(stats);
        }

        char *health_json = NULL;
        err = airy_cognition_health_check(engine, &health_json);
        if (err == AIRY_SUCCESS && health_json) {
            assert(is_valid_json_prefix(health_json));
            free(health_json);
        }

        drop_engine(engine, fallback);
        printf("    [Thread %d] Engine destroyed\n", args->thread_id);
    } else {
        printf("    [Thread %d] Engine creation failed: err=%s\n", args->thread_id, error_str(err));
    }

    if (args->result_mutex) {
        airy_mtx_lock(args->result_mutex);
        (*args->result_count) += local_success;
        airy_mtx_unlock(args->result_mutex);
    }

    return (void *)(intptr_t)local_success;
}

/* ============================================================================
 * 线程工作函数 (feedback 变体): 带 feedback 回调的完整生命周期
 * create_ex → process → health_check → destroy
 * ============================================================================ */
void *thread_feedback_worker(void *arg)
{
    thread_feedback_worker_args_t *args = (thread_feedback_worker_args_t *)arg;
    int local_success = 0;

    printf("    [FB-Thread %d] Starting with feedback config...\n", args->thread_id);

    airy_cognition_config_t config;
    memset(&config, 0, sizeof(config));
    config.cognition_default_timeout_ms = 15000;
    config.cognition_max_retries = 2;
    config.feedback_callback = null_feedback_callback;
    config.feedback_user_data = NULL;

    airy_cognition_engine_t *engine = NULL;
    airy_plan_strategy_t *fallback = NULL;
    airy_err_t err =
        airy_cognition_create_ex_take(&config, new_primary_strat(), NULL, NULL, &engine);
    if (err == AIRY_SUCCESS && engine != NULL) {
        fallback = attach_fallback(engine);
        printf("    [FB-Thread %d] Engine created with feedback\n", args->thread_id);

        airy_task_plan_t *plan = NULL;
        err = airy_cognition_process(engine, args->input, strlen(args->input), &plan);
        if (err == AIRY_SUCCESS) {
            printf("    [FB-Thread %d] Process succeeded\n", args->thread_id);
            if (plan) {
                airy_task_plan_free(plan);
            }
            local_success = 1;
        } else {
            printf("    [FB-Thread %d] Process failed: err=%s\n", args->thread_id, error_str(err));
        }

        char *health_json = NULL;
        err = airy_cognition_health_check(engine, &health_json);
        if (err == AIRY_SUCCESS && health_json) {
            assert(is_valid_json_prefix(health_json));
            free(health_json);
        }

        drop_engine(engine, fallback);
        printf("    [FB-Thread %d] Engine destroyed\n", args->thread_id);
    } else {
        printf("    [FB-Thread %d] Engine creation failed: err=%s\n", args->thread_id,
               error_str(err));
    }

    if (args->result_mutex) {
        airy_mtx_lock(args->result_mutex);
        (*args->result_count) += local_success;
        airy_mtx_unlock(args->result_mutex);
    }

    return (void *)(intptr_t)local_success;
}

/* ============================================================================
 * 线程工作函数 (coordinator + dispatcher 变体):
 * 使用自定义 coordinator 和 dispatcher 策略的完整生命周期
 * ============================================================================ */
airy_err_t mock_coord_func(const char **prompts, size_t count, void *context, char **out_result)
{
    (void)context;
    if (!prompts || !out_result)
        return AIRY_EINVAL;

    const char *header = "{\"coordinated\":true,\"count\":";
    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%zu", count);
    const char *trailer = "}";

    size_t total_len = strlen(header) + strlen(count_buf) + strlen(trailer) + 1;
    *out_result = (char *)malloc(total_len);
    if (!*out_result)
        return AIRY_ENOMEM;
    snprintf(*out_result, total_len, "%s%s%s", header, count_buf, trailer);

    return AIRY_SUCCESS;
}

void mock_coord_destroy(airy_coordinator_strategy_t *s)
{
    free(s);
}

airy_err_t mock_disp_func(const airy_task_node_t *task, const void **candidates, size_t count,
                          void *context, char **out_agent_id)
{
    (void)task;
    (void)candidates;
    (void)context;
    if (!out_agent_id)
        return AIRY_EINVAL;

    *out_agent_id = strdup("agent-0");
    if (!*out_agent_id)
        return AIRY_ENOMEM;

    return AIRY_SUCCESS;
}

void mock_disp_destroy(airy_dispatching_strategy_t *s)
{
    free(s);
}

void *thread_strategy_worker(void *arg)
{
    thread_strategy_worker_args_t *args = (thread_strategy_worker_args_t *)arg;
    int local_success = 0;

    printf("    [ST-Thread %d] Starting with coordinator+dispatcher...\n", args->thread_id);

    airy_coordinator_strategy_t *coord = (airy_coordinator_strategy_t *)calloc(1, sizeof(*coord));
    if (!coord)
        return (void *)(intptr_t)0;
    coord->coordinate = mock_coord_func;
    coord->destroy = mock_coord_destroy;
    coord->data = NULL;

    airy_dispatching_strategy_t *disp = (airy_dispatching_strategy_t *)calloc(1, sizeof(*disp));
    if (!disp) {
        free(coord);
        return (void *)(intptr_t)0;
    }
    disp->dispatch = mock_disp_func;
    disp->destroy = mock_disp_destroy;
    disp->data = NULL;

    airy_cognition_engine_t *engine = NULL;
    airy_plan_strategy_t *fallback = NULL;
    airy_err_t err = airy_cognition_create_take(new_primary_strat(), coord, disp, &engine);
    if (err != AIRY_SUCCESS) {
        /* create 失败：协调/派发策略所有权未转移（_take 仅在成功时接管），
         * 由调用方负责释放，否则泄漏。 */
        coord->destroy(coord);
        disp->destroy(disp);
        coord = NULL;
        disp = NULL;
    }
    if (err == AIRY_SUCCESS && engine != NULL) {
        fallback = attach_fallback(engine);
        printf("    [ST-Thread %d] Engine created with strategies\n", args->thread_id);

        airy_task_plan_t *plan = NULL;
        err = airy_cognition_process(engine, args->input, strlen(args->input), &plan);
        if (err == AIRY_SUCCESS) {
            printf("    [ST-Thread %d] Process succeeded\n", args->thread_id);
            if (plan) {
                airy_task_plan_free(plan);
            }
            local_success = 1;
        } else {
            printf("    [ST-Thread %d] Process failed: err=%s\n", args->thread_id, error_str(err));
        }

        drop_engine(engine, fallback);
        printf("    [ST-Thread %d] Engine destroyed\n", args->thread_id);
    } else {
        printf("    [ST-Thread %d] Engine creation failed: err=%s\n", args->thread_id,
               error_str(err));
    }

    if (args->result_mutex) {
        airy_mtx_lock(args->result_mutex);
        (*args->result_count) += local_success;
        airy_mtx_unlock(args->result_mutex);
    }

    return (void *)(intptr_t)local_success;
}

/* ============================================================================
 * 线程工作函数 (混合变体): 同时运行三种引擎类型
 * ============================================================================ */
void *thread_mix_worker(void *arg)
{
    mix_worker_args_t *args = (mix_worker_args_t *)arg;
    int local_success = 0;

    printf("    [Mix-Thread %d type=%d] Starting...\n", args->thread_id, (int)args->type);

    airy_cognition_engine_t *engine = NULL;
    airy_err_t err;

    switch (args->type) {
    case MIX_TYPE_BASIC:
        err = airy_cognition_create_take(new_primary_strat(), NULL, NULL, &engine);
        break;
    case MIX_TYPE_FEEDBACK: {
        airy_cognition_config_t config;
        memset(&config, 0, sizeof(config));
        config.cognition_default_timeout_ms = 30000;
        config.cognition_max_retries = 3;
        config.feedback_callback = null_feedback_callback;
        config.feedback_user_data = NULL;
        err = airy_cognition_create_ex_take(&config, new_primary_strat(), NULL, NULL, &engine);
        break;
    }
    case MIX_TYPE_STRATEGY: {
        airy_coordinator_strategy_t *coord =
            (airy_coordinator_strategy_t *)calloc(1, sizeof(*coord));
        airy_dispatching_strategy_t *disp = (airy_dispatching_strategy_t *)calloc(1, sizeof(*disp));
        if (!coord || !disp) {
            free(coord);
            free(disp);
            err = AIRY_ENOMEM;
            break;
        }
        coord->coordinate = mock_coord_func;
        coord->destroy = mock_coord_destroy;
        coord->data = NULL;
        disp->dispatch = mock_disp_func;
        disp->destroy = mock_disp_destroy;
        disp->data = NULL;

        err = airy_cognition_create_take(new_primary_strat(), coord, disp, &engine);
        if (err != AIRY_SUCCESS) {
            /* create 失败：协调/派发策略所有权未转移（_take 仅在成功时接管），
             * 由调用方负责释放，否则泄漏。 */
            coord->destroy(coord);
            disp->destroy(disp);
        }
        break;
    }
    default:
        err = AIRY_EINVAL;
        break;
    }

    if (err == AIRY_SUCCESS && engine != NULL) {
        airy_plan_strategy_t *fallback = attach_fallback(engine);
        airy_task_plan_t *plan = NULL;
        err = airy_cognition_process(engine, args->input, strlen(args->input), &plan);
        if (err == AIRY_SUCCESS) {
            if (plan) {
                airy_task_plan_free(plan);
            }
            local_success = 1;
        }
        drop_engine(engine, fallback);
    }

    if (args->result_mutex) {
        airy_mtx_lock(args->result_mutex);
        (*args->result_count) += local_success;
        airy_mtx_unlock(args->result_mutex);
    }

    return (void *)(intptr_t)local_success;
}

/* ============================================================================
 * 主入口
 * ============================================================================ */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    are_ops_set_tc(cog_payload_tc());
    are_ops_set_mc(cog_payload_mc());

    printf("=========================================\n");
    printf("  CoreLoopThree Thread Safety Tests\n");
    printf("  ThreadSanitizer Verification (INT-02)\n");
    printf("=========================================\n\n");

    printf("--- INT-02.1: Concurrent Cognition Engine Tests ---\n");
    RUN_TEST(int02_1_concurrent_cognition_engines);
    RUN_TEST(int02_1_concurrent_feedback_engines);
    RUN_TEST(int02_1_concurrent_strategy_engines);
    RUN_TEST(int02_1_mixed_concurrent_engines);

    printf("\n--- INT-02.2: Error Handling Path Verification ---\n");
    RUN_TEST(int02_2_null_input_handling);
    RUN_TEST(int02_2_null_engine_handling);
    RUN_TEST(int02_2_error_code_propagation);
    RUN_TEST(int02_2_create_ex_error_handling);
    RUN_TEST(int02_2_intent_parser_error_handling);
    RUN_TEST(int02_2_memory_engine_error_handling);

    printf("\n=========================================\n");
    printf("  All thread safety tests PASSED\n");
    printf("=========================================\n");

    return 0;
}
