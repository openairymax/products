// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_thread_safety_int0202.c - INT-02.2 错误处理路径验证测试域
 *
 * 验证 NULL 输入处理、NULL 引擎处理、错误码传播。
 */

#include "test_thread_safety_internal.h"

void test_int02_2_null_input_handling(void)
{
    printf("    Testing NULL input handling...\n");

    /* 1. NULL engine → AIRY_EINVAL */
    airy_task_plan_t *plan = NULL;
    airy_err_t err = airy_cognition_process(NULL, "test", 4, &plan);
    assert(err != AIRY_SUCCESS);
    printf("    NULL engine → err=%s (expected: non-success)\n", error_str(err));

    /* 2. NULL input → AIRY_EINVAL */
    airy_plan_strategy_t *fallback = NULL;
    airy_cognition_engine_t *engine = create_default_engine(&fallback);
    err = airy_cognition_process(engine, NULL, 0, &plan);
    assert(err != AIRY_SUCCESS);
    printf("    NULL input → err=%s (expected: non-success)\n", error_str(err));

    /* 3. NULL out_plan → AIRY_EINVAL */
    err = airy_cognition_process(engine, "test", 4, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    NULL out_plan → err=%s (expected: non-success)\n", error_str(err));

    /* 4. NULL out_engine → AIRY_EINVAL */
    err = airy_cognition_create_take(NULL, NULL, NULL, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    NULL out_engine → err=%s (expected: non-success)\n", error_str(err));

    drop_engine(engine, fallback);
}

void test_int02_2_null_engine_handling(void)
{
    printf("    Testing NULL engine handling...\n");

    airy_cognition_destroy(NULL);
    printf("    destroy(NULL) → no crash (safe no-op)\n");

    /* 2. airy_cognition_set_fallback_plan(NULL, NULL) */
    airy_cognition_set_fallback_plan(NULL, NULL);
    printf("    set_fallback_plan(NULL, NULL) → no crash\n");

    /* 3. airy_cognition_set_context_take(NULL, NULL, NULL) */
    airy_cognition_set_context_take(NULL, NULL, NULL);
    printf("    set_context(NULL, NULL, NULL) → no crash\n");

    /* 4. airy_cognition_set_memory(NULL, NULL) */
    airy_cognition_set_memory(NULL, NULL);
    printf("    set_memory(NULL, NULL) → no crash\n");

    airy_err_t err = airy_cognition_stats(NULL, NULL, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    stats(NULL) → err=%s (expected: non-success)\n", error_str(err));

    err = airy_cognition_health_check(NULL, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    health_check(NULL) → err=%s (expected: non-success)\n", error_str(err));

    airy_task_plan_free(NULL);
    printf("    task_plan_free(NULL) → no crash\n");
}

void test_int02_2_error_code_propagation(void)
{
    printf("    Testing error code propagation through pipeline...\n");

    airy_plan_strategy_t *fallback = NULL;
    airy_cognition_engine_t *engine = create_default_engine(&fallback);

    airy_task_plan_t *plan = NULL;
    airy_err_t err = airy_cognition_process(engine, "valid input", 11, &plan);
    assert(err == AIRY_SUCCESS);
    printf("    Valid input → err=%s (expected: AIRY_SUCCESS)\n", error_str(err));
    if (plan) {
        airy_task_plan_free(plan);
    }

    const char *invalid_inputs[] = {"another valid input for stats tracking",
                                    "different input to verify pipeline",
                                    "third input for consistency check"};

    for (int i = 0; i < 3; i++) {
        plan = NULL;
        err = airy_cognition_process(engine, invalid_inputs[i], strlen(invalid_inputs[i]), &plan);
        assert(err == AIRY_SUCCESS);
        printf("    Input %d → err=%s\n", i + 1, error_str(err));
        if (plan) {
            airy_task_plan_free(plan);
        }
    }

    char *stats = NULL;
    size_t stats_len = 0;
    err = airy_cognition_stats(engine, &stats, &stats_len);
    assert(err == AIRY_SUCCESS);
    assert(stats != NULL);
    printf("    Stats after pipeline: %.100s\n", stats);
    free(stats);

    char *health_json = NULL;
    err = airy_cognition_health_check(engine, &health_json);
    assert(err == AIRY_SUCCESS);
    assert(health_json != NULL);
    assert(is_valid_json_prefix(health_json));
    printf("    Health check after pipeline: %.100s\n", health_json);
    free(health_json);

    err = airy_cognition_process(engine, NULL, 0, &plan);
    assert(err != AIRY_SUCCESS);
    printf("    NULL input after valid ops → err=%s (error NOT swallowed)\n", error_str(err));

    drop_engine(engine, fallback);
}

void test_int02_2_create_ex_error_handling(void)
{
    printf("    Testing create_ex error handling...\n");

    airy_cognition_engine_t *engine = NULL;
    airy_err_t err = airy_cognition_create_ex_take(NULL, NULL, NULL, NULL, &engine);
    assert(err == AIRY_SUCCESS);
    assert(engine != NULL);
    printf("    create_ex(NULL config) → succeeded\n");
    airy_cognition_destroy(engine);

    err = airy_cognition_create_ex_take(NULL, NULL, NULL, NULL, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    create_ex(NULL out_engine) → err=%s\n", error_str(err));

    airy_cognition_config_t config;
    memset(&config, 0, sizeof(config));
    config.cognition_default_timeout_ms = 10000;
    config.cognition_max_retries = 1;
    err = airy_cognition_create_ex_take(&config, NULL, NULL, NULL, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    create_ex(config, NULL out_engine) → err=%s\n", error_str(err));
}

void test_int02_2_intent_parser_error_handling(void)
{
    printf("    Testing intent parser error handling...\n");

    airy_err_t err = airy_intent_parser_create(NULL);
    assert(err != AIRY_SUCCESS);
    printf("    intent_parser_create(NULL) → err=%s\n", error_str(err));

    airy_intent_parser_destroy(NULL);
    printf("    intent_parser_destroy(NULL) → no crash\n");

    airy_intent_parser_t *parser = NULL;
    err = airy_intent_parser_create(&parser);
    assert(err == AIRY_SUCCESS);
    assert(parser != NULL);

    err = airy_intent_parser_parse(parser, NULL, 0, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    intent_parser_parse(NULL input) → err=%s\n", error_str(err));

    airy_intent_free(NULL);
    printf("    intent_free(NULL) → no crash\n");

    airy_intent_parser_destroy(parser);
}

void test_int02_2_memory_engine_error_handling(void)
{
    printf("    Testing memory engine error handling...\n");

    airy_err_t err = airy_memory_create(NULL, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    memory_create(NULL out_engine) → err=%s\n", error_str(err));

    airy_memory_destroy(NULL);
    printf("    memory_destroy(NULL) → no crash\n");

    airy_memory_engine_t *mem = NULL;
    err = airy_memory_create(NULL, &mem);
    assert(err == AIRY_SUCCESS);
    assert(mem != NULL);

    err = airy_memory_write(mem, NULL, NULL);
    assert(err != AIRY_SUCCESS);
    printf("    memory_write(NULL record) → err=%s\n", error_str(err));

    airy_memory_destroy(mem);
}
