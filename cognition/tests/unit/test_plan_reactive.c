// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_plan_reactive.c - 规划策略载荷注表（plan ops）装配与载荷单元测试
 * （0.1.19 M5-4 §271，自 atoms 迁入后的独立测试面）
 *
 * 覆盖：
 *   1. payload_registry plan 注表往返：cog_payload_plan() →
 *      are_ops_set_plan() → are_ops_get_plan() 指针一致，且经 ops
 *      分发面工厂可创建 reactive 策略（daemon think_d 装配同路径）；
 *   2. BAN-257 缺席语义：注入 NULL 后 are_ops_get_plan() 返回 NULL
 *      （装配层得 NULL → 引擎裸启动），再注恢复；
 *   3. reactive 中文关键词规则命中（llm=NULL，纯载荷规则路径）；
 *   4. reflective 无 LLM（llm=NULL, memory=NULL）：llm_build_dynamic_plan
 *      返 ESERVICE 后 build_fallback_plan 兜底，plan() 最终返回
 *      AIRY_SUCCESS + 非空计划（节点>0），fallback 策略安全可用；
 *   5. NULL intent 防御：plan(NULL, ...) 返回错误码，不崩溃。
 */

#include "cognition.h"
#include "plan_strategy.h"
#include "payload_registry.h"
#include "airy_plan_ops.h"
#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define FAIL(name, msg)                       \
    do {                                      \
        printf("[FAIL] %s: %s\n", name, msg); \
        tests_failed++;                       \
    } while (0)

#define RUN_TEST(func)                     \
    do {                                   \
        tests_run++;                       \
        int prev_failed = tests_failed;    \
        func();                            \
        if (prev_failed == tests_failed) { \
            printf("[PASS] %s\n", #func);  \
        }                                  \
    } while (0)

static airy_task_plan_t *reactive_plan_for(const char *goal, size_t *out_count)
{
    airy_plan_strategy_t *strat = airy_plan_reactive_create(NULL);
    if (!strat || !strat->plan)
        return NULL;
    airy_intent_t intent;
    AIRY_MEMSET(&intent, 0, sizeof(intent));
    intent.intent_goal = (char *)goal;
    intent.intent_goal_len = goal ? strlen(goal) : 0;
    intent.intent_flags = 0x02;
    airy_task_plan_t *plan = NULL;
    airy_err_t err = strat->plan(&intent, strat->data, &plan);
    if (out_count)
        *out_count = plan ? plan->task_plan_node_count : 0;
    if (err != AIRY_SUCCESS || !plan) {
        if (plan)
            airy_task_plan_free(plan);
        strat->destroy(strat);
        return NULL;
    }
    strat->destroy(strat);
    return plan;
}

static void test_plan_ops_roundtrip(void)
{
    const airy_plan_ops_t *ops = cog_payload_plan();
    if (!ops || !ops->create_reactive || !ops->create_reflective) {
        FAIL("plan_ops_roundtrip", "payload plan ops incomplete");
        return;
    }
    are_ops_set_plan(ops);
    if (are_ops_get_plan() != ops) {
        FAIL("plan_ops_roundtrip", "get mismatch after set");
        return;
    }
    /* 经 ops 分发面创建 reactive 策略（think_d 装配层同路径） */
    airy_plan_strategy_t *strat = are_ops_get_plan()->create_reactive(NULL);
    if (!strat || !strat->plan) {
        FAIL("plan_ops_roundtrip", "ops dispatch factory failed");
        return;
    }
    strat->destroy(strat);
}

static void test_plan_ops_absent_semantics(void)
{
    /* BAN-257：ops 缺席 → get 返回 NULL（装配层得 NULL → 引擎裸启动，
     * process 期 fail fast），注表面必须忠实反映缺席态并可恢复。 */
    are_ops_set_plan(NULL);
    if (are_ops_get_plan() != NULL) {
        FAIL("plan_ops_absent_semantics", "get must be NULL after set(NULL)");
    }
    are_ops_set_plan(cog_payload_plan());
    if (!are_ops_get_plan()) {
        FAIL("plan_ops_absent_semantics", "re-inject failed");
    }
}

static void test_plan_payload_zh_smoke(void)
{
    /* 载荷规则路径冒烟：中文 goal 须命中规则产出多节点计划（规则表
     * 含中文关键词；全 miss 会退化为校验型兜底 DAG）。 */
    size_t count = 0;
    airy_task_plan_t *plan = reactive_plan_for("分析这份数据并写一份报告", &count);
    if (!plan || count < 2) {
        FAIL("plan_payload_zh_smoke", "expected >=2 nodes for zh goal");
        if (plan)
            airy_task_plan_free(plan);
        return;
    }
    airy_task_plan_free(plan);
}

static void test_plan_reflective_no_llm_fallback(void)
{
    airy_plan_strategy_t *strat = airy_plan_reflective_create(NULL, NULL);
    if (!strat || !strat->plan) {
        FAIL("plan_reflective_no_llm_fallback", "factory failed");
        return;
    }
    airy_intent_t intent;
    AIRY_MEMSET(&intent, 0, sizeof(intent));
    intent.intent_goal = (char *)"分析这份数据并写一份报告";
    intent.intent_goal_len = strlen("分析这份数据并写一份报告");
    intent.intent_flags = 0x02;
    airy_task_plan_t *plan = NULL;
    airy_err_t err = strat->plan(&intent, strat->data, &plan);
    if (err != AIRY_SUCCESS || !plan || plan->task_plan_node_count == 0) {
        FAIL("plan_reflective_no_llm_fallback", "expected SUCCESS + non-empty plan");
        if (plan)
            airy_task_plan_free(plan);
        strat->destroy(strat);
        return;
    }
    airy_task_plan_free(plan);
    strat->destroy(strat);
}

static void test_plan_null_intent_defense(void)
{
    airy_plan_strategy_t *strat = airy_plan_reactive_create(NULL);
    if (!strat || !strat->plan) {
        FAIL("plan_null_intent_defense", "factory failed");
        return;
    }
    airy_task_plan_t *plan = NULL;
    airy_err_t err = strat->plan(NULL, strat->data, &plan);
    if (err == AIRY_SUCCESS || plan) {
        FAIL("plan_null_intent_defense", "NULL intent must fail fast");
        if (plan)
            airy_task_plan_free(plan);
    }
    strat->destroy(strat);
}

int main(void)
{
    /* 注表装配（daemon svc.c 同路径）：plan ops 注入先行，后续测试经
     * ops 分发面与载荷工厂消费。reflective 路径入口按契约 fail-fast
     * 检查 TC/MC 载荷在场，故一并注入（缺任一则返 AIRY_ENOSYS）。 */
    are_ops_set_plan(cog_payload_plan());
    are_ops_set_tc(cog_payload_tc());
    are_ops_set_mc(cog_payload_mc());

    RUN_TEST(test_plan_ops_roundtrip);
    RUN_TEST(test_plan_ops_absent_semantics);
    RUN_TEST(test_plan_payload_zh_smoke);
    RUN_TEST(test_plan_reflective_no_llm_fallback);
    RUN_TEST(test_plan_null_intent_defense);

    printf("\nresults: %d/%d passed (%d failed)\n", tests_run - tests_failed, tests_run,
           tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
