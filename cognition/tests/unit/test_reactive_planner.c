// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_reactive_planner.c - reactive 规划器 + 工作内存检索单元测试
 *
 * 覆盖：
 *   1. airy_tc_working_memory_retrieve 存储/检索/删除往返（实现名须与头文件
 *      声明一致，否则工作内存只写不读）；
 *   2. reactive 规划器启发式节点目标彼此 DISTINCT（多节点不得复用整段
 *      原始 goal，否则认知审查子 agent 会误判"重复未分解"）；
 *   3. CPR 汇总认知决策消费：clarify_needed 收敛计划、risk_count 追加验证节点。
 */

#include "cognition.h"
#include "plan_strategy.h"
#include "foundation/thinking_chain.h"
#include "airy_memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PASS(name) printf("[PASS] %s\n", name)
#define TEST_FAIL(name, msg)                  \
    do {                                      \
        printf("[FAIL] %s: %s\n", name, msg); \
        tests_failed++;                       \
    } while (0)

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(func)                     \
    do {                                   \
        tests_run++;                       \
        int prev_failed = tests_failed;    \
        func();                            \
        if (prev_failed == tests_failed) { \
            tests_passed++;                \
        }                                  \
    } while (0)

static void test_wm_retrieve_roundtrip(void)
{
    airy_working_memory_t *mem = NULL;
    if (airy_tc_working_memory_create(8, &mem) != AIRY_SUCCESS || !mem) {
        TEST_FAIL("wm_retrieve_roundtrip", "create failed");
        return;
    }
    const char *json = "{\"intent\":\"task\",\"clarify_needed\":0,\"risk_count\":0}";
    if (airy_tc_working_memory_store(mem, "cog_review_decision", json, strlen(json) + 1,
                                     "application/json", 1) != AIRY_SUCCESS) {
        TEST_FAIL("wm_retrieve_roundtrip", "store failed");
        airy_tc_working_memory_destroy(mem);
        return;
    }
    void *val = NULL;
    size_t sz = 0;
    if (airy_tc_working_memory_retrieve(mem, "cog_review_decision", &val, &sz) != AIRY_SUCCESS ||
        !val || sz != strlen(json) + 1 || strcmp((const char *)val, json) != 0) {
        TEST_FAIL("wm_retrieve_roundtrip", "retrieve content mismatch");
        airy_tc_working_memory_destroy(mem);
        return;
    }
    /* 未命中键：必须报错（非成功） */
    if (airy_tc_working_memory_retrieve(mem, "absent_key", &val, &sz) == AIRY_SUCCESS) {
        TEST_FAIL("wm_retrieve_roundtrip", "miss must not succeed");
        airy_tc_working_memory_destroy(mem);
        return;
    }
    if (airy_tc_working_memory_remove(mem, "cog_review_decision") != AIRY_SUCCESS) {
        TEST_FAIL("wm_retrieve_roundtrip", "remove failed");
        airy_tc_working_memory_destroy(mem);
        return;
    }
    airy_tc_working_memory_destroy(mem);
    TEST_PASS("wm_retrieve_roundtrip");
}

static airy_task_plan_t *reactive_plan_for(const char *goal, const char *decision,
                                           size_t *out_count)
{
    airy_plan_strategy_t *strat = airy_plan_reactive_create(NULL);
    if (!strat || !strat->plan) {
        return NULL;
    }
    airy_intent_t intent;
    AIRY_MEMSET(&intent, 0, sizeof(intent));
    intent.intent_goal = (char *)goal;
    intent.intent_goal_len = strlen(goal);
    intent.intent_flags = 0x02;
    intent.intent_cog_decision = (char *)decision;
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

static void test_reactive_distinct_goals(void)
{
    /* 命中 analyze + write 两条规则 → 2 个节点，目标必须彼此 DISTINCT 且
     * 携带对应规则动作（不得复用整段原始 goal 导致目标雷同）。
     * 注：match_rules 为大小写敏感 strstr，输入用小写命中规则。 */
    const char *goal = "analyze the dataset and write a summary report";
    size_t count = 0;
    airy_task_plan_t *plan = reactive_plan_for(goal, NULL, &count);
    if (!plan || count < 2) {
        TEST_FAIL("reactive_distinct_goals", "expected >=2 nodes");
        if (plan)
            airy_task_plan_free(plan);
        return;
    }
    int dup = 0;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *ni = plan->task_plan_nodes[i];
        if (!ni || !ni->task_node_goal || !ni->task_node_goal[0]) {
            dup = 1;
            break;
        }
        for (size_t j = i + 1; j < plan->task_plan_node_count; j++) {
            const airy_task_node_t *nj = plan->task_plan_nodes[j];
            if (nj && nj->task_node_goal &&
                strcmp(ni->task_node_goal, nj->task_node_goal) == 0) {
                dup = 1;
                break;
            }
        }
    }
    /* 目标须携带命中规则的 action（规则数组序中 write 先于 analyze） */
    const char *g0 = plan->task_plan_nodes[0]->task_node_goal;
    int has_action = (strncmp(g0, "write_output", strlen("write_output")) == 0) ||
                     (strncmp(g0, "analyze_and_report", strlen("analyze_and_report")) == 0);
    if (dup) {
        TEST_FAIL("reactive_distinct_goals", "node goals not distinct");
    } else if (!has_action) {
        TEST_FAIL("reactive_distinct_goals", "goal lacks rule action");
    } else {
        TEST_PASS("reactive_distinct_goals");
    }
    airy_task_plan_free(plan);
}

static void test_reactive_decision_consumed(void)
{
    /* 命中 4 条规则 → 4 节点；clarify_needed=1 收敛到 2，risk_count=2 追加
     * 验证节点 → 最终 3 节点且末位为 validator（"验证最终结果"）。 */
    const char *goal = "Analyze the data, compare results, write a report and run it";
    const char *decision = "{\"intent\":\"task\",\"clarify_needed\":1,\"risk_count\":2}";
    size_t count = 0;
    airy_task_plan_t *plan = reactive_plan_for(goal, decision, &count);
    if (!plan) {
        TEST_FAIL("reactive_decision_consumed", "plan failed");
        return;
    }
    if (count != 3) {
        TEST_FAIL("reactive_decision_consumed", "expected 3 nodes (clarify cap + validator)");
        airy_task_plan_free(plan);
        return;
    }
    const airy_task_node_t *last = plan->task_plan_nodes[count - 1];
    if (!last || !last->task_node_goal ||
        strncmp(last->task_node_goal, "验证最终结果", strlen("验证最终结果")) != 0) {
        TEST_FAIL("reactive_decision_consumed", "last node not validator");
        airy_task_plan_free(plan);
        return;
    }
    airy_task_plan_free(plan);
    TEST_PASS("reactive_decision_consumed");
}

static void test_reactive_validator_no_user_goal(void)
{
    /* 回归守则：validator 节点 goal 不得携带用户指令（若写成
     * goal = "验证最终结果：<完整指令>"，只读验证角色看到写动作词会
     * 越界调用被 ACL 拒绝）。节点级 goal 只描述验证职责。 */
    const char *goal = "create a new file with fs_write and then verify it";
    const char *decision = "{\"intent\":\"task\",\"clarify_needed\":0,\"risk_count\":1}";
    size_t count = 0;
    airy_task_plan_t *plan = reactive_plan_for(goal, decision, &count);
    if (!plan || count == 0) {
        TEST_FAIL("validator_no_user_goal", "plan failed");
        if (plan)
            airy_task_plan_free(plan);
        return;
    }
    const airy_task_node_t *last = plan->task_plan_nodes[count - 1];
    if (!last || !last->task_node_goal) {
        TEST_FAIL("validator_no_user_goal", "validator goal missing");
        airy_task_plan_free(plan);
        return;
    }
    /* 1. 目标以验证职责开头 */
    if (strncmp(last->task_node_goal, "验证最终结果", strlen("验证最终结果")) != 0) {
        TEST_FAIL("validator_no_user_goal", "validator goal lacks verify role");
        airy_task_plan_free(plan);
        return;
    }
    /* 2. 不携带用户指令中的动作词（fs_write）与完整指令文本 */
    if (strstr(last->task_node_goal, "fs_write") ||
        strstr(last->task_node_goal, "create a new file")) {
        TEST_FAIL("validator_no_user_goal", "validator goal leaked user instruction");
        airy_task_plan_free(plan);
        return;
    }
    /* 3. 非验证节点的 goal 仍携带动作 + 摘要（可为执行体指明职责） */
    int any_action = 0;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *n = plan->task_plan_nodes[i];
        if (n && n->task_node_goal && n->task_node_goal[0]) {
            if (strstr(n->task_node_goal, "create_artifact") ||
                strstr(n->task_node_goal, "write_output") ||
                strstr(n->task_node_goal, "验证"))
                any_action = 1;
        }
    }
    if (!any_action) {
        TEST_FAIL("validator_no_user_goal", "worker node goal lost action");
        airy_task_plan_free(plan);
        return;
    }
    airy_task_plan_free(plan);
    TEST_PASS("validator_no_user_goal");
}

static void test_reactive_utf8_goal_truncation(void)
{
    /* 中文 goal 在节点目标拼接时按字节截断会切断多字节 UTF-8 序列
     * （%.*s 字节截断会把「多」字切成非法序列 \xe5），
     * 必须回退到字符边界，节点 goal 为合法 UTF-8。 */
    const char *goal = "请用 web_search 搜索 Linux kernel 最新稳定版本是多少，并给出具体版本号";
    size_t count = 0;
    airy_task_plan_t *plan = reactive_plan_for(goal, NULL, &count);
    if (!plan || count == 0) {
        TEST_FAIL("reactive_utf8_goal_truncation", "plan failed");
        if (plan)
            airy_task_plan_free(plan);
        return;
    }
    int bad = 0;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *ni = plan->task_plan_nodes[i];
        if (!ni || !ni->task_node_goal)
            continue;
        /* 非法 UTF-8 判定：出现 0x80-0xBF 连续字节但无引导字节 */
        size_t j = 0;
        while (ni->task_node_goal[j]) {
            unsigned char c = (unsigned char)ni->task_node_goal[j];
            if ((c & 0x80) == 0) {
                j++;
            } else if ((c & 0xC0) == 0xC0) {
                size_t seq = 0;
                if ((c & 0xE0) == 0xC0)
                    seq = 2;
                else if ((c & 0xF0) == 0xE0)
                    seq = 3;
                else if ((c & 0xF8) == 0xF0)
                    seq = 4;
                if (seq == 0 || j + seq > strlen(ni->task_node_goal) + 1) {
                    bad = 1;
                    break;
                }
                for (size_t k = 1; k < seq; k++) {
                    if (((unsigned char)ni->task_node_goal[j + k] & 0xC0) != 0x80) {
                        bad = 1;
                        break;
                    }
                }
                j += seq;
            } else {
                /* 0x80-0xBF 孤立连续字节 */
                bad = 1;
                break;
            }
        }
        if (bad)
            break;
    }
    if (bad) {
        TEST_FAIL("reactive_utf8_goal_truncation", "node goal is not valid UTF-8");
    } else {
        TEST_PASS("reactive_utf8_goal_truncation");
    }
    airy_task_plan_free(plan);
}

static void test_reactive_chinese_keywords(void)
{
    /* 中文任务必须命中规则（规则表若仅含英文关键词，中文
     * goal 全 miss → 退化为校验型兜底 DAG，无法完成生成型任务）。 */
    const char *goal = "分析这份数据并写一份报告";
    size_t count = 0;
    airy_task_plan_t *plan = reactive_plan_for(goal, NULL, &count);
    if (!plan || count < 2) {
        TEST_FAIL("reactive_chinese_keywords", "expected >=2 nodes for zh goal");
        if (plan)
            airy_task_plan_free(plan);
        return;
    }
    const char *g0 = plan->task_plan_nodes[0]->task_node_goal;
    const char *g1 = plan->task_plan_nodes[1]->task_node_goal;
    int ok = g0 && g1 && strncmp(g0, "write_output", strlen("write_output")) == 0 &&
             strncmp(g1, "analyze_and_report", strlen("analyze_and_report")) == 0;
    if (!ok) {
        TEST_FAIL("reactive_chinese_keywords", "zh keywords did not hit rule actions");
    } else {
        TEST_PASS("reactive_chinese_keywords");
    }
    airy_task_plan_free(plan);
}

static void test_reactive_fallback_generative(void)
{
    /* 规则 miss + LLM 不可用时，兜底动作必须为生成型（不得
     * 出现"校验执行结果"类校验序列），执行体必须可写（coding）。 */
    const char *goal = "帮我把这个任务搞定";
    size_t count = 0;
    airy_task_plan_t *plan = reactive_plan_for(goal, NULL, &count);
    if (!plan || count == 0) {
        TEST_FAIL("reactive_fallback_generative", "plan failed");
        return;
    }
    const airy_task_node_t *n0 = plan->task_plan_nodes[0];
    int ok = n0 && n0->task_node_goal &&
             strncmp(n0->task_node_goal, "生成初始方案", strlen("生成初始方案")) == 0 &&
             n0->task_node_agent_role && strcmp(n0->task_node_agent_role, "coding") == 0;
    for (size_t i = 0; ok && i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *n = plan->task_plan_nodes[i];
        if (n && n->task_node_goal &&
            (strstr(n->task_node_goal, "校验执行结果") ||
             strstr(n->task_node_goal, "验证最终输出")))
            ok = 0;
    }
    if (!ok) {
        TEST_FAIL("reactive_fallback_generative", "fallback not generative/writable");
    } else {
        TEST_PASS("reactive_fallback_generative");
    }
    airy_task_plan_free(plan);
}

int main(void)
{
    RUN_TEST(test_wm_retrieve_roundtrip);
    RUN_TEST(test_reactive_distinct_goals);
    RUN_TEST(test_reactive_decision_consumed);
    RUN_TEST(test_reactive_validator_no_user_goal);
    RUN_TEST(test_reactive_utf8_goal_truncation);
    RUN_TEST(test_reactive_chinese_keywords);
    RUN_TEST(test_reactive_fallback_generative);

    printf("\nresults: %d/%d passed (%d failed)\n", tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
