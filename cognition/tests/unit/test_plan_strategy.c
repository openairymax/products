// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_plan_strategy.c - 规划策略单元测试（products/cognition，M5-4 迁出）
 *
 * 覆盖两类规划策略的真实创建/规划/销毁：
 *   - hierarchical  领域关键词匹配分解（code 领域 4 子任务）+ 无匹配默认 5 子任务
 *   - ml            规则分解（query 意图 5 子任务 + verify 节点）
 * 计划回收经机制公共契约头 airy_plan_nodes.h 的 inline 回收件执行，验证机制核
 * 外策略库（不链接 airy_cognition）亦可自包含解析节点/计划回收路径。
 */

#include "plan_strategy.h"
#include "cognition.h"
#include "airy_plan_nodes.h"

#include "airy_memory.h"
#include "error.h"

#include <stdio.h>
#include <string.h>

#define TEST_PASS(name) printf("[PASS] %s\n", name)
#define TEST_FAIL(name, msg) printf("[FAIL] %s: %s\n", name, msg)

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name, msg)    \
    do {                          \
        if (cond) {               \
            TEST_PASS(name);      \
            g_pass++;             \
        } else {                  \
            TEST_FAIL(name, msg); \
            g_fail++;             \
        }                         \
    } while (0)

static void test_hierarchical(void)
{
    airy_plan_strategy_t *s = airy_plan_hierarchical_create(NULL, 3);
    CHECK(s != NULL, "hierarchical_create", "create returned NULL");
    if (!s)
        return;
    CHECK(s->plan != NULL && s->destroy != NULL, "hierarchical_vtable", "missing plan/destroy");

    airy_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    /* 领域命中：code → 4 子任务 */
    char goal_code[] = "write code module";
    intent.intent_goal = goal_code;
    airy_task_plan_t *plan = NULL;
    airy_err_t rc = s->plan(&intent, s->data, &plan);
    CHECK(rc == AIRY_SUCCESS && plan != NULL, "hierarchical_plan_code", "plan failed");
    CHECK(plan && plan->task_plan_node_count == 4, "hierarchical_code_4_nodes",
          "expected 4 nodes for code domain");
    if (plan && plan->task_plan_node_count == 4) {
        CHECK(plan->task_plan_nodes[0]->task_node_id != NULL &&
                  strcmp(plan->task_plan_nodes[0]->task_node_id, "analyze_requirements") == 0,
              "hierarchical_code_first_node", "unexpected first node id");
    }
    plan_nodes_reclaim(plan);

    /* 无领域命中：回退默认 5 子任务 */
    char goal_generic[] = "zzz unmapped goal";
    intent.intent_goal = goal_generic;
    plan = NULL;
    rc = s->plan(&intent, s->data, &plan);
    CHECK(rc == AIRY_SUCCESS && plan != NULL && plan->task_plan_node_count == 5,
          "hierarchical_default_5_nodes", "expected 5 default nodes");
    plan_nodes_reclaim(plan);

    s->destroy(s);
}

static void test_ml(void)
{
    airy_plan_strategy_t *s = airy_plan_ml_create(NULL, NULL);
    CHECK(s != NULL, "ml_create", "create returned NULL");
    if (!s)
        return;
    CHECK(s->plan != NULL && s->destroy != NULL, "ml_vtable", "missing plan/destroy");

    airy_intent_t intent;
    memset(&intent, 0, sizeof(intent));
    char goal_query[] = "query the database";
    intent.intent_goal = goal_query;
    intent.intent_flags = 1; /* complexity = 1 → 规则命中、粗粒度 */

    airy_task_plan_t *plan = NULL;
    airy_err_t rc = s->plan(&intent, s->data, &plan);
    CHECK(rc == AIRY_SUCCESS && plan != NULL, "ml_plan_query", "plan failed");
    if (plan) {
        /* query 规则 5 子任务 + 1 verify 节点 */
        CHECK(plan->task_plan_node_count == 6, "ml_query_6_nodes", "expected 5 + verify");
        CHECK(plan->task_plan_id != NULL, "ml_plan_id", "missing plan id");
    }

    plan_nodes_reclaim(plan);
    s->destroy(s);
}

int main(void)
{
    test_hierarchical();
    test_ml();

    printf("\n[SUMMARY] pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
