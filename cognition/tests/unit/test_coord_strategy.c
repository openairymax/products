// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_coord_strategy.c - 协调策略单元测试（products/cognition，M5-4 迁出）
 *
 * 覆盖协调策略载荷的真实创建/协调/销毁路径：
 *   - dmc      双模型（→ dual）与三模型（→ weighted）两条工厂分支
 *   - mcoord   多数投票（2 同 1 异 → 多数）
 *   - wcoord   加权融合（最高权模型选中，产 JSON 结果）
 *   - arbiter  human（回调选 2 → inputs[1]）与 model（NULL 拒绝 / 无 ops
 *              降级 inputs[0] / 注入 ops 表端到端选 2 → inputs[1]）
 * LLM 后端以注入表 airy_llm_ops 的测试替身（fake service_complete /
 * response_free）承载——策略载荷经注入表调用，永不链接 daemon 符号。
 */

#include "coord_strategy.h"
#include "airy_llm_ops.h"

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

/* 非空 LLM 句柄占位：策略/工厂只判非空并在有 ops 表时才解引用传递 */
static char g_llm_handle;
#define DUMMY_LLM ((llm_service_t *)(void *)&g_llm_handle)

/* ---- 注入的 LLM ops 测试替身（模拟仲裁后端返回 "2"） ---- */
static llm_response_t g_fake_resp;
static llm_message_t g_fake_choice;

static int fake_complete(llm_service_t *svc, const llm_request_config_t *cfg,
                         llm_response_t **out_response)
{
    (void)svc;
    (void)cfg;
    static char s_choice[] = "2";
    g_fake_choice.role = "assistant";
    g_fake_choice.content = s_choice;
    g_fake_resp.choices = &g_fake_choice;
    g_fake_resp.choice_count = 1;
    *out_response = &g_fake_resp;
    return 0;
}

static void fake_response_free(llm_response_t *resp)
{
    (void)resp;
}

static const airy_llm_ops_t g_fake_ops = {fake_complete, NULL, fake_response_free};

static void human_pick_second(const char *question, char *answer, size_t max_len)
{
    (void)question;
    if (max_len >= 2)
        snprintf(answer, max_len, "2");
}

static void test_dmc_dual(void)
{
    airy_coordinator_strategy_t *s = airy_dmc_create("m1", "m2", NULL, NULL);
    CHECK(s != NULL, "dmc_dual_create", "create returned NULL");
    if (!s)
        return;
    CHECK(s->coordinate != NULL && s->destroy != NULL, "dmc_dual_vtable",
          "missing coordinate/destroy");

    const char *inputs[2] = {"A", "B"};
    char *out = NULL;
    airy_err_t rc = s->coordinate(inputs, 2, s, &out);
    CHECK(rc == AIRY_SUCCESS && out != NULL && strcmp(out, "[Primary] A") == 0,
          "dmc_dual_primary_wins", "expected primary model output");
    if (out)
        AIRY_FREE(out);

    s->destroy(s);
}

static void test_dmc_triple(void)
{
    airy_coordinator_strategy_t *s = airy_dmc_create("m1", "m2", "m3", NULL);
    CHECK(s != NULL, "dmc_triple_create", "create returned NULL");
    if (!s)
        return;

    const char *inputs[3] = {"A", "B", "C"};
    char *out = NULL;
    airy_err_t rc = s->coordinate(inputs, 3, s, &out);
    CHECK(rc == AIRY_SUCCESS && out != NULL && strstr(out, "\"result\":\"A\"") != NULL,
          "dmc_triple_weighted", "expected weighted JSON result A");
    if (out)
        AIRY_FREE(out);

    s->destroy(s);
}

static void test_mcoord(void)
{
    const char *names[3] = {"m1", "m2", "m3"};
    airy_coordinator_strategy_t *s = airy_mcoord_create(names, 3, NULL);
    CHECK(s != NULL, "mcoord_create", "create returned NULL");
    if (!s)
        return;

    const char *inputs[3] = {"X", "Y", "X"};
    char *out = NULL;
    airy_err_t rc = s->coordinate(inputs, 3, s, &out);
    CHECK(rc == AIRY_SUCCESS && out != NULL && strcmp(out, "X") == 0, "mcoord_majority_x",
          "expected majority result X");
    if (out)
        AIRY_FREE(out);

    s->destroy(s);
}

static void test_wcoord(void)
{
    const char *names[3] = {"m1", "m2", "m3"};
    float weights[3] = {0.5f, 0.3f, 0.2f};
    airy_coordinator_strategy_t *s = airy_wcoord_create(names, weights, 3, DUMMY_LLM);
    CHECK(s != NULL, "wcoord_create", "create returned NULL");
    if (!s)
        return;

    const char *inputs[3] = {"A", "B", "C"};
    char *out = NULL;
    airy_err_t rc = s->coordinate(inputs, 3, s, &out);
    CHECK(rc == AIRY_SUCCESS && out != NULL && strstr(out, "\"result\":\"A\"") != NULL,
          "wcoord_highest_weight", "expected highest-weight model output A");
    if (out)
        AIRY_FREE(out);

    s->destroy(s);

    /* llm 为 NULL 时工厂拒绝（加权融合需 LLM 句柄） */
    CHECK(airy_wcoord_create(names, weights, 3, NULL) == NULL, "wcoord_reject_null_llm",
          "expected NULL when llm is NULL");
}

static void test_arbiter_human(void)
{
    airy_coordinator_strategy_t *s = airy_arbiter_human_create(human_pick_second);
    CHECK(s != NULL, "arbiter_human_create", "create returned NULL");
    if (!s)
        return;

    const char *inputs[2] = {"first", "second"};
    char *out = NULL;
    airy_err_t rc = s->coordinate(inputs, 2, s, &out);
    CHECK(rc == AIRY_SUCCESS && out != NULL && strcmp(out, "second") == 0, "arbiter_human_pick2",
          "expected inputs[1] via human callback");
    if (out)
        AIRY_FREE(out);

    s->destroy(s);
}

static void test_arbiter_model(void)
{
    /* llm 为 NULL 时工厂拒绝 */
    CHECK(airy_arbiter_model_create("arb", NULL) == NULL, "arbiter_model_reject_null_llm",
          "expected NULL when llm is NULL");

    /* 无 ops 表注入 → 降级 inputs[0] */
    are_ops_set_llm(NULL);
    airy_coordinator_strategy_t *s = airy_arbiter_model_create("arb", DUMMY_LLM);
    CHECK(s != NULL, "arbiter_model_create", "create returned NULL");
    if (s) {
        const char *inputs[2] = {"first", "second"};
        char *out = NULL;
        airy_err_t rc = s->coordinate(inputs, 2, s, &out);
        CHECK(rc == AIRY_SUCCESS && out != NULL && strcmp(out, "first") == 0,
              "arbiter_model_no_ops_fallback", "expected inputs[0] fallback");
        if (out)
            AIRY_FREE(out);
        s->destroy(s);
    }

    /* 注入 ops 表 → 端到端解析选择 2 → inputs[1] */
    are_ops_set_llm(&g_fake_ops);
    s = airy_arbiter_model_create("arb", DUMMY_LLM);
    CHECK(s != NULL, "arbiter_model_create_ops", "create returned NULL");
    if (s) {
        const char *inputs[2] = {"first", "second"};
        char *out = NULL;
        airy_err_t rc = s->coordinate(inputs, 2, s, &out);
        CHECK(rc == AIRY_SUCCESS && out != NULL && strcmp(out, "second") == 0,
              "arbiter_model_ops_pick2", "expected inputs[1] via injected ops");
        if (out)
            AIRY_FREE(out);
        s->destroy(s);
    }
    are_ops_set_llm(NULL);
}

int main(void)
{
    test_dmc_dual();
    test_dmc_triple();
    test_mcoord();
    test_wcoord();
    test_arbiter_human();
    test_arbiter_model();

    printf("\n[SUMMARY] pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
