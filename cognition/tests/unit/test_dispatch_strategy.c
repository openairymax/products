// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_dispatch_strategy.c - 认知分发策略单元测试（products/cognition）
 *
 * 覆盖四类分发策略的真实创建/择优/销毁：
 *   - weighted   三因子加权打分择优（含配置覆盖与默认权重）
 *   - priority   最高优先级择优
 *   - round_robin 轮询循环（含跨调用游标推进）
 *   - ml_based   四维打分择优 + 在线反馈(report_outcome) + 平均奖励
 */

#include "dispatch_strategy.h"
#include "agent_registry.h"
#include "cognition.h"

#include "airy_memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

/* 候选 agent：best 在优先级/成本/成功率/信任四维均占优 */
static agent_info_t g_a0 = {(char *)"agent-low", (char *)"worker", 5.0f, 0.5f, 0.5f, 1};
static agent_info_t g_a1 = {(char *)"agent-best", (char *)"worker", 0.1f, 0.9f, 0.9f, 9};
static agent_info_t g_a2 = {(char *)"agent-mid", (char *)"worker", 2.0f, 0.7f, 0.7f, 5};

static airy_err_t fake_get_agents(void *ctx, const char *role, agent_info_t ***out_agents,
                                  size_t *out_count)
{
    (void)ctx;
    (void)role;
    static agent_info_t *list[3];
    list[0] = &g_a0;
    list[1] = &g_a1;
    list[2] = &g_a2;
    *out_agents = list;
    *out_count = 3;
    return AIRY_SUCCESS;
}

static const void *g_candidates[3];

static void setup(void)
{
    g_candidates[0] = &g_a0;
    g_candidates[1] = &g_a1;
    g_candidates[2] = &g_a2;
}

static airy_task_node_t g_task;

static void test_priority(void)
{
    airy_dispatching_strategy_t *s = NULL;
    airy_err_t err = airy_dispatching_priority_create((void *)0x1, fake_get_agents, &s);
    CHECK(err == AIRY_SUCCESS && s != NULL, "priority create", "创建失败");

    char *sel = NULL;
    err = s->dispatch(&g_task, g_candidates, 3, s->data, &sel);
    CHECK(err == AIRY_SUCCESS && sel && strcmp(sel, "agent-best") == 0,
          "priority selects highest", "未选中最高优先级 agent");
    AIRY_FREE(sel);

    s->destroy(s);

    err = airy_dispatching_priority_create(NULL, fake_get_agents, &s);
    CHECK(err == AIRY_EINVAL, "priority rejects null ctx", "空 ctx 未拒绝");
}

static void test_round_robin(void)
{
    airy_dispatching_strategy_t *s = airy_dispatching_round_robin_create((void *)0x1, fake_get_agents);
    CHECK(s != NULL, "rr create", "创建失败");

    const char *expect[4] = {"agent-low", "agent-best", "agent-mid", "agent-low"};
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        char *sel = NULL;
        airy_err_t err = s->dispatch(&g_task, g_candidates, 3, s->data, &sel);
        if (err != AIRY_SUCCESS || !sel || strcmp(sel, expect[i]) != 0)
            ok = 0;
        AIRY_FREE(sel);
    }
    CHECK(ok, "rr cycles in order", "轮询顺序错误");

    s->destroy(s);
}

static void test_weighted(void)
{
    airy_dispatching_strategy_t *s =
        airy_dispatching_weighted_create(NULL, (void *)0x1, fake_get_agents);
    CHECK(s != NULL, "weighted create default", "创建失败");

    char *sel = NULL;
    airy_err_t err = s->dispatch(&g_task, g_candidates, 3, s->data, &sel);
    CHECK(err == AIRY_SUCCESS && sel && strcmp(sel, "agent-best") == 0,
          "weighted selects best", "加权未选中最优 agent");
    AIRY_FREE(sel);
    s->destroy(s);

    /* 配置强制只看成本 → 成本最低者仍为 best */
    airy_dispatching_weighted_config_t cfg = {1.0f, 0.0f, 0.0f};
    airy_dispatching_strategy_t *s2 =
        airy_dispatching_weighted_create(&cfg, (void *)0x1, fake_get_agents);
    CHECK(s2 != NULL, "weighted create config", "配置构造失败");
    sel = NULL;
    err = s2->dispatch(&g_task, g_candidates, 3, s2->data, &sel);
    CHECK(err == AIRY_SUCCESS && sel && strcmp(sel, "agent-best") == 0,
          "weighted honors cost-only config", "成本单因子未选中低成本 agent");
    AIRY_FREE(sel);
    s2->destroy(s2);
}

static void test_ml(void)
{
    airy_dispatching_strategy_t *s =
        airy_dispatching_ml_create(NULL, (void *)0x1, fake_get_agents);
    CHECK(s != NULL, "ml create", "创建失败");

    CHECK(airy_dispatching_ml_avg_reward(s) == 0.5f, "ml default avg reward",
          "无历史时平均奖励应为 0.5");

    char *sel = NULL;
    airy_err_t err = s->dispatch(&g_task, g_candidates, 3, s->data, &sel);
    CHECK(err == AIRY_SUCCESS && sel && strcmp(sel, "agent-best") == 0,
          "ml selects best", "ML 未选中最优 agent");
    AIRY_FREE(sel);

    err = airy_dispatching_ml_report_outcome(s, 1.0f);
    CHECK(err == AIRY_SUCCESS, "ml report outcome", "反馈上报失败");
    CHECK(airy_dispatching_ml_report_outcome(NULL, 0.5f) == AIRY_EINVAL,
          "ml report rejects null", "空策略未拒绝");

    float avg = airy_dispatching_ml_avg_reward(s);
    CHECK(avg == 1.0f, "ml avg reward updated", "平均奖励未随反馈更新");

    s->destroy(s);
    CHECK(airy_dispatching_ml_avg_reward(NULL) == 0.0f, "ml avg reward null safe",
          "空策略平均奖励应为 0");
}

int main(void)
{
    setup();
    memset(&g_task, 0, sizeof(g_task));

    test_priority();
    test_round_robin();
    test_weighted();
    test_ml();

    printf("\n[dispatch_strategy] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
