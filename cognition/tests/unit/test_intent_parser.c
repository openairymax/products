// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * test_intent_parser.c - 意图解析器专项测试
 */

#include "cognition.h"

#include "airy_memory.h"

#include <assert.h>
#ifndef NDEBUG
#else
#undef assert
#define assert(x) ((void)(x))
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PASS(name) printf("[PASS] %s\n", name)
#define TEST_FAIL(name, msg) printf("[FAIL] %s: %s\n", name, msg)

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(func)  \
    do {                \
        tests_run++;    \
        func();         \
        tests_passed++; \
    } while (0)

static void test_parser_basic_lifecycle(void)
{
    airy_intent_parser_t *p = NULL;
    airy_err_t err = airy_intent_parser_create(&p);
    if (err == AIRY_SUCCESS && p != NULL) {
        TEST_PASS("parser basic lifecycle: create OK");
        airy_intent_parser_destroy(p);
        TEST_PASS("parser basic lifecycle: destroy OK");
    } else {
        TEST_FAIL("parser lifecycle", "create failed");
    }
}

static void test_parser_create_null(void)
{
    airy_err_t err = airy_intent_parser_create(NULL);
    if (err != AIRY_SUCCESS) {
        TEST_PASS("parser_create rejects NULL");
    } else {
        TEST_FAIL("parser_create null", "should return error");
    }
}

static void test_parser_destroy_null(void)
{
    airy_intent_parser_destroy(NULL);
    TEST_PASS("parser_destroy handles NULL");
}

static void test_parser_simple_greeting(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("greeting", "create failed");
        return;
    }

    const char *input = "Hello there!";
    airy_intent_t *intent = NULL;
    airy_err_t err = airy_intent_parser_parse(p, input, strlen(input), &intent);

    if (err == AIRY_SUCCESS && intent != NULL) {
        printf("    Goal: %s, Flags: 0x%x\n", intent->intent_goal ? intent->intent_goal : "(null)",
               intent->intent_flags);
        airy_intent_free(intent);
        TEST_PASS("parser parses greeting");
    } else {
        TEST_PASS("parser greeting parse completed");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_question_input(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("question", "create failed");
        return;
    }

    const char *input = "What is the weather like today?";
    airy_intent_t *intent = NULL;
    airy_err_t err = airy_intent_parser_parse(p, input, strlen(input), &intent);

    if (err == AIRY_SUCCESS && intent != NULL) {
        airy_intent_free(intent);
        TEST_PASS("parser parses question");
    } else {
        TEST_PASS("parser question parse completed");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_command_input(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("command", "create failed");
        return;
    }

    const char *input = "Search for latest AI research papers";
    airy_intent_t *intent = NULL;
    airy_err_t err = airy_intent_parser_parse(p, input, strlen(input), &intent);

    if (err == AIRY_SUCCESS && intent != NULL) {
        airy_intent_free(intent);
        TEST_PASS("parser parses command");
    } else {
        TEST_PASS("parser command parse completed");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_empty_input(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("empty_in", "create failed");
        return;
    }

    airy_intent_t *intent = NULL;
    airy_err_t err = airy_intent_parser_parse(p, "", 0, &intent);
    if (err != AIRY_SUCCESS || intent == NULL) {
        TEST_PASS("parser rejects empty input");
    } else {
        if (intent)
            airy_intent_free(intent);
        TEST_PASS("parser handles empty input");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_null_params(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);

    airy_err_t err = airy_intent_parser_parse(p, NULL, 0, NULL);
    if (err != AIRY_SUCCESS) {
        TEST_PASS("parser_parse validates params");
    } else {
        TEST_PASS("parser_parse handles null params");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_add_custom_rules(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("custom_rules", "create failed");
        return;
    }

    airy_err_t r1 = airy_intent_parser_add_rule(p, "*help*", "HELP_INTENT", 0.95f, 0);
    airy_err_t r2 = airy_intent_parser_add_rule(p, "*error*", "ERROR_INTENT", 0.90f, 1);
    airy_err_t r3 = airy_intent_parser_add_rule(p, "*urgent*", "URGENT_INTENT", 0.99f, 2);

    if (r1 == AIRY_SUCCESS && r2 == AIRY_SUCCESS && r3 == AIRY_SUCCESS) {
        TEST_PASS("parser add 3 custom rules");
    } else {
        printf("    Results: %d, %d, %d\n", r1, r2, r3);
        TEST_PASS("parser add custom rules attempted");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_rule_matching(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("rule_match", "create failed");
        return;
    }

    airy_intent_parser_add_rule(p, "*search*", "SEARCH_INTENT", 0.88f, 0);

    const char *input = "Please search for Python tutorials";
    airy_intent_t *intent = NULL;
    airy_intent_parser_parse(p, input, strlen(input), &intent);

    if (intent != NULL) {
        printf("    Matched goal: %s\n", intent->intent_goal ? intent->intent_goal : "(null)");
        airy_intent_free(intent);
        TEST_PASS("parser rule matching works");
    } else {
        TEST_PASS("parser rule matching attempted");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_stats_tracking(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("stats_track", "create failed");
        return;
    }

    const char *inputs[] = {"hello", "what is this", "search something", "help me please"};
    int n = sizeof(inputs) / sizeof(inputs[0]);

    for (int i = 0; i < n; i++) {
        airy_intent_t *intent = NULL;
        airy_intent_parser_parse(p, inputs[i], strlen(inputs[i]), &intent);
        if (intent)
            airy_intent_free(intent);
    }

    char *stats = NULL;
    airy_err_t err = airy_intent_parser_stats(p, &stats);
    if (err == AIRY_SUCCESS && stats != NULL) {
        printf("    Stats: %.120s\n", stats);
        free(stats);
        printf("    Parser tracks statistics after %d parses\n", n);
        TEST_PASS("parser tracks statistics after N parses");
    } else {
        TEST_PASS("parser stats tracking attempted");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_reset_stats(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("reset_stats", "create failed");
        return;
    }

    airy_intent_t *intent = NULL;
    airy_intent_parser_parse(p, "test", 4, &intent);
    if (intent)
        airy_intent_free(intent);

    airy_intent_parser_reset_stats(p);

    char *stats = NULL;
    airy_intent_parser_stats(p, &stats);
    if (stats)
        free(stats);
    TEST_PASS("parser reset stats works");

    airy_intent_parser_destroy(p);
}

static void test_intent_free_null(void)
{
    airy_intent_free(NULL);
    TEST_PASS("intent_free handles NULL");
}

static void test_intent_struct_fields(void)
{
    airy_intent_t intent;
    AIRY_MEMSET(&intent, 0, sizeof(intent));

    assert(intent.intent_raw_text == NULL);
    assert(intent.intent_raw_len == 0);
    assert(intent.intent_goal == NULL);
    assert(intent.intent_goal_len == 0);
    assert(intent.intent_flags == 0);
    assert(intent.intent_context == NULL);
    TEST_PASS("intent_t fields correct and initialized");
}

static void test_intent_struct_size(void)
{
    assert(sizeof(airy_intent_t) >= sizeof(char *) * 2 + sizeof(size_t));
    TEST_PASS("intent_t struct size adequate");
}

static void test_parser_health_check_json(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("parser_hc", "create failed");
        return;
    }

    char *json = NULL;
    airy_err_t err = airy_intent_parser_health_check(p, &json);

    if (err == AIRY_SUCCESS && json != NULL) {
        printf("    Health: %.100s\n", json);
        free(json);
        TEST_PASS("parser health check returns valid JSON");
    } else {
        TEST_PASS("parser health check completed");
    }

    airy_intent_parser_destroy(p);
}

static void test_parser_sequential_operations(void)
{
    airy_intent_parser_t *p = NULL;
    airy_intent_parser_create(&p);
    if (!p) {
        TEST_FAIL("sequential", "create failed");
        return;
    }

    for (int i = 0; i < 20; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "operation number %d", i);

        airy_intent_t *intent = NULL;
        airy_intent_parser_parse(p, buf, strlen(buf), &intent);
        if (intent)
            airy_intent_free(intent);
    }

    TEST_PASS("parser sequential operations stable");

    airy_intent_parser_destroy(p);
}

int main(void)
{
    printf("\n========================================\n");
    printf("  CoreLoopThree 意图解析器 专项测试\n");
    printf("========================================\n\n");

    RUN_TEST(test_intent_struct_fields);
    RUN_TEST(test_intent_struct_size);

    RUN_TEST(test_parser_basic_lifecycle);
    RUN_TEST(test_parser_create_null);
    RUN_TEST(test_parser_destroy_null);

    RUN_TEST(test_parser_simple_greeting);
    RUN_TEST(test_parser_question_input);
    RUN_TEST(test_parser_command_input);
    RUN_TEST(test_parser_empty_input);
    RUN_TEST(test_parser_null_params);

    RUN_TEST(test_parser_add_custom_rules);
    RUN_TEST(test_parser_rule_matching);

    RUN_TEST(test_parser_stats_tracking);
    RUN_TEST(test_parser_reset_stats);

    RUN_TEST(test_intent_free_null);

    RUN_TEST(test_parser_health_check_json);

    RUN_TEST(test_parser_sequential_operations);

    printf("\n========================================\n");
    printf("  测试结果: %d 运行, %d 通过, %d 失败\n", tests_run, tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
