// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_thread_safety_int0201.c - INT-02.1 并发认知引擎测试域
 *
 * 创建多个线程，每个线程执行独立引擎的完整生命周期。
 * 该测试设计为通过 ThreadSanitizer 检测数据竞争：
 *   - 每个线程创建独立的引擎实例
 *   - 若内部存在全局/静态状态竞争，TSan 将报告
 *   - 使用 pthread 进行线程创建和同步
 */

#include "test_thread_safety_internal.h"

void test_int02_1_concurrent_cognition_engines(void)
{
#define INT02_1_NUM_THREADS 4

    pthread_t threads[INT02_1_NUM_THREADS];
    thread_worker_args_t args[INT02_1_NUM_THREADS];
    airy_mtx_t result_mutex;
    int result_count = 0;

    airy_mtx_init(&result_mutex);

    const char *inputs[] = {"Analyze the quarterly sales data and provide recommendations",
                            "Define machine learning and explain its key algorithms",
                            "Write a Python function to calculate Fibonacci numbers",
                            "Summarize the history of artificial intelligence"};

    printf("    Spawning %d concurrent threads (basic engines)...\n", INT02_1_NUM_THREADS);

    for (int i = 0; i < INT02_1_NUM_THREADS; i++) {
        args[i].thread_id = i + 1;
        args[i].input = inputs[i];
        args[i].expect_success = 1;
        args[i].result_count = &result_count;
        args[i].result_mutex = &result_mutex;

        int rc = pthread_create(&threads[i], NULL, thread_worker, &args[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < INT02_1_NUM_THREADS; i++) {
        void *retval = NULL;
        int rc = pthread_join(threads[i], &retval);
        assert(rc == 0);
        printf("    Thread %d joined: retval=%d\n", i + 1, (int)(intptr_t)retval);
    }

    printf("    Basic engine threads: %d/%d succeeded\n", result_count, INT02_1_NUM_THREADS);

    assert(result_count == INT02_1_NUM_THREADS);

    airy_mtx_destroy(&result_mutex);

#undef INT02_1_NUM_THREADS
}

void test_int02_1_concurrent_feedback_engines(void)
{
#define INT02_1_FB_NUM_THREADS 4

    pthread_t threads[INT02_1_FB_NUM_THREADS];
    thread_feedback_worker_args_t args[INT02_1_FB_NUM_THREADS];
    airy_mtx_t result_mutex;
    int result_count = 0;

    airy_mtx_init(&result_mutex);

    const char *inputs[] = {"Calculate the average of [10, 20, 30, 40, 50]",
                            "Explain the theory of relativity in simple terms",
                            "Compare and contrast SQL and NoSQL databases",
                            "Design a REST API for a todo list application"};

    printf("    Spawning %d concurrent threads (feedback engines)...\n", INT02_1_FB_NUM_THREADS);

    for (int i = 0; i < INT02_1_FB_NUM_THREADS; i++) {
        args[i].thread_id = i + 1;
        args[i].input = inputs[i];
        args[i].result_count = &result_count;
        args[i].result_mutex = &result_mutex;

        int rc = pthread_create(&threads[i], NULL, thread_feedback_worker, &args[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < INT02_1_FB_NUM_THREADS; i++) {
        void *retval = NULL;
        int rc = pthread_join(threads[i], &retval);
        assert(rc == 0);
        printf("    FB-Thread %d joined: retval=%d\n", i + 1, (int)(intptr_t)retval);
    }

    printf("    Feedback engine threads: %d/%d succeeded\n", result_count, INT02_1_FB_NUM_THREADS);
    assert(result_count == INT02_1_FB_NUM_THREADS);

    airy_mtx_destroy(&result_mutex);

#undef INT02_1_FB_NUM_THREADS
}

void test_int02_1_concurrent_strategy_engines(void)
{
#define INT02_1_ST_NUM_THREADS 4

    pthread_t threads[INT02_1_ST_NUM_THREADS];
    thread_strategy_worker_args_t args[INT02_1_ST_NUM_THREADS];
    airy_mtx_t result_mutex;
    int result_count = 0;

    airy_mtx_init(&result_mutex);

    const char *inputs[] = {"Plan a multi-step analysis of customer feedback data",
                            "Coordinate the response to a system outage incident",
                            "Dispatch tasks to the appropriate team members",
                            "Orchestrate a complex workflow with dependencies"};

    printf("    Spawning %d concurrent threads (strategy engines)...\n", INT02_1_ST_NUM_THREADS);

    for (int i = 0; i < INT02_1_ST_NUM_THREADS; i++) {
        args[i].thread_id = i + 1;
        args[i].input = inputs[i];
        args[i].result_count = &result_count;
        args[i].result_mutex = &result_mutex;

        int rc = pthread_create(&threads[i], NULL, thread_strategy_worker, &args[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < INT02_1_ST_NUM_THREADS; i++) {
        void *retval = NULL;
        int rc = pthread_join(threads[i], &retval);
        assert(rc == 0);
        printf("    ST-Thread %d joined: retval=%d\n", i + 1, (int)(intptr_t)retval);
    }

    printf("    Strategy engine threads: %d/%d succeeded\n", result_count, INT02_1_ST_NUM_THREADS);
    assert(result_count == INT02_1_ST_NUM_THREADS);

    airy_mtx_destroy(&result_mutex);

#undef INT02_1_ST_NUM_THREADS
}

void test_int02_1_mixed_concurrent_engines(void)
{
#define INT02_1_MIX_NUM_THREADS 6

    pthread_t threads[INT02_1_MIX_NUM_THREADS];
    mix_worker_args_t args[INT02_1_MIX_NUM_THREADS];
    airy_mtx_t result_mutex;
    int result_count = 0;

    airy_mtx_init(&result_mutex);

    const char *inputs[] = {"Basic task A: analyze data",
                            "Feedback task B: verify results",
                            "Strategy task C: coordinate response",
                            "Basic task D: summarize findings",
                            "Feedback task E: validate output",
                            "Strategy task F: dispatch work"};

    const mix_worker_type_t types[] = {MIX_TYPE_BASIC, MIX_TYPE_FEEDBACK, MIX_TYPE_STRATEGY,
                                       MIX_TYPE_BASIC, MIX_TYPE_FEEDBACK, MIX_TYPE_STRATEGY};

    printf("    Spawning %d mixed concurrent threads...\n", INT02_1_MIX_NUM_THREADS);

    for (int i = 0; i < INT02_1_MIX_NUM_THREADS; i++) {
        args[i].thread_id = i + 1;
        args[i].type = types[i];
        args[i].input = inputs[i];
        args[i].result_count = &result_count;
        args[i].result_mutex = &result_mutex;

        int rc = pthread_create(&threads[i], NULL, thread_mix_worker, &args[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < INT02_1_MIX_NUM_THREADS; i++) {
        void *retval = NULL;
        int rc = pthread_join(threads[i], &retval);
        assert(rc == 0);
        printf("    Mix-Thread %d joined: retval=%d\n", i + 1, (int)(intptr_t)retval);
    }

    printf("    Mixed engine threads: %d/%d succeeded\n", result_count, INT02_1_MIX_NUM_THREADS);
    assert(result_count == INT02_1_MIX_NUM_THREADS);

    airy_mtx_destroy(&result_mutex);

#undef INT02_1_MIX_NUM_THREADS
}
