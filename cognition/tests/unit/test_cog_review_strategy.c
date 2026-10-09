// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_cog_review_strategy.c
 * @brief 认知并行审查（CPR）策略载荷测试（products/cognition，M5-4 迁出）
 *
 * 测试内容：
 *   1. 参数校验：cog_review_run 非法参数 → EINVAL
 *   2. 无 LLM 降级：svc/adapter 均为 NULL → degraded=1 + 决策不虚构
 *   3. 意图加权投票：多子 agent 独立意见 → 最高加权类别胜出 + 置信度
 *   4. 问题审查合并：clarify_needed 聚合 + 风险计数
 *   5. 资源释放：cog_review_result_free(NULL) 安全；free 后状态干净
 *   6. 并行度裁剪：显式 max_parallel 安全接受
 *   7. 边界/覆盖角色汇总：四角色意见统一合并
 *   8. 补全闭包注入：机制核注入 cog_review_complete_fn → 意见收集 + 非降级
 *
 * @note 不依赖 llm_d 守护进程（LLM 路径经 cog_review_internal.h 暴露的
 *       纯函数直测；并行 worker 路径在降级用例中验证不创建线程的稳健性）。
 * @note 用例 8 用测试替身（test double）验证机制核闭包注入契约：策略侧只
 *       持 cog_review_complete_fn 函数指针，不与机制核符号链接耦合。
 * @note 不使用 assert() 执行副作用操作（NDEBUG 下 assert 展开为 ((void)0)）。
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "cog_review_strategy.h"
#include "cog_review_internal.h"

#include "airy_rt.h"
#include "error.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_PASS(name)                \
    do {                               \
        printf("  [PASS] %s\n", name); \
        tests_run++;                   \
        tests_passed++;                \
    } while (0)

#define TEST_FAIL(name, msg)                    \
    do {                                        \
        printf("  [FAIL] %s: %s\n", name, msg); \
        tests_run++;                            \
    } while (0)

#define CHECK(cond, name, msg)    \
    do {                          \
        if (cond) {               \
            TEST_PASS(name);      \
        } else {                  \
            TEST_FAIL(name, msg); \
        }                         \
    } while (0)

/* ==================== 1. 参数校验 ==================== */

static int test_cog_review_null_args(void)
{
    cog_review_result_t res;
    airy_err_t err;

    err = cog_review_run(NULL, NULL, NULL, 0, 0, NULL);
    CHECK(err == AIRY_EINVAL, "run(NULL input) -> EINVAL", "expected EINVAL");

    err = cog_review_run(NULL, NULL, "x", 1, 0, NULL);
    CHECK(err == AIRY_EINVAL, "run(NULL result) -> EINVAL", "expected EINVAL");

    err = cog_review_run(NULL, NULL, "", 0, 0, &res);
    CHECK(err == AIRY_EINVAL, "run(empty input) -> EINVAL", "expected EINVAL");

    /* 零值结果初始化 */
    cog_review_result_init(&res);
    CHECK(res.opinion_count == 0 && res.decision_json == NULL && res.degraded == 0,
          "result_init zeroes fields", "fields not zeroed");
    return 0;
}

/* ==================== 2. 无 LLM 降级 ==================== */

static int test_cog_review_degraded(void)
{
    const char *input = "Deploy the service to production and monitor it";
    cog_review_result_t res;
    cog_review_result_init(&res);

    airy_err_t err = cog_review_run(NULL, NULL, input, strlen(input), 0, &res);
    CHECK(err == AIRY_SUCCESS, "run(no LLM) -> SUCCESS", "expected SUCCESS not blocking");
    CHECK(res.degraded == 1, "no LLM -> degraded=1", "degraded flag not set");
    CHECK(res.opinion_count == 0, "no LLM -> zero opinions", "opinions must be empty");
    CHECK(res.decision_json != NULL, "no LLM -> decision present", "decision missing");

    if (res.decision_json) {
#ifdef AIRY_HAS_CJSON
        cJSON *dec = cJSON_Parse(res.decision_json);
        CHECK(dec != NULL, "decision parses as JSON", "invalid JSON");
        if (dec) {
            cJSON *intent = cJSON_GetObjectItem(dec, "intent");
            cJSON *degraded = cJSON_GetObjectItem(dec, "degraded");
            CHECK(cJSON_IsString(intent) && strcmp(intent->valuestring, "unknown") == 0,
                  "degraded intent=unknown", "intent should be unknown");
            CHECK(cJSON_IsNumber(degraded) && degraded->valueint == 1,
                  "degraded flag in JSON=1", "json degraded mismatch");
            cJSON_Delete(dec);
        }
#else
        CHECK(strstr(res.decision_json, "degraded") != NULL, "decision mentions degraded",
              "degraded marker missing");
#endif
    }

    cog_review_result_free(&res);
    CHECK(res.opinion_count == 0 && res.decision_json == NULL && res.degraded == 0,
          "free leaves clean state", "state not cleaned");
    return 0;
}

/* ==================== 3. 意图加权投票 ==================== */

static int test_cog_review_aggregate_voting(void)
{
    cog_review_result_t res;
    cog_review_result_init(&res);

    /* 两个认知确认子 agent：task(0.6) 与 chat(0.9) —— chat 加权胜出 */
    res.opinions[0].role = COG_REVIEW_ROLE_INTENT_CONFIRM;
    AIRY_STRNCPY_TERM(res.opinions[0].agent_id, "intent-confirmer-a",
                      sizeof(res.opinions[0].agent_id));
    res.opinions[0].opinion_json = AIRY_STRDUP("{\"intent\":\"task\",\"confidence\":0.6}");

    res.opinions[1].role = COG_REVIEW_ROLE_INTENT_CONFIRM;
    AIRY_STRNCPY_TERM(res.opinions[1].agent_id, "intent-confirmer-b",
                      sizeof(res.opinions[1].agent_id));
    res.opinions[1].opinion_json = AIRY_STRDUP("{\"intent\":\"chat\",\"confidence\":0.9}");
    res.opinion_count = 2;

    airy_err_t err = cog_review_aggregate(&res);
    CHECK(err == AIRY_SUCCESS, "aggregate voting -> SUCCESS", "aggregate failed");
    CHECK(res.degraded == 0, "voting with opinions -> not degraded", "degraded mismatch");

#ifdef AIRY_HAS_CJSON
    cJSON *dec = cJSON_Parse(res.decision_json);
    CHECK(dec != NULL, "voting decision parses", "invalid decision JSON");
    if (dec) {
        cJSON *intent = cJSON_GetObjectItem(dec, "intent");
        cJSON *conf = cJSON_GetObjectItem(dec, "confidence");
        CHECK(cJSON_IsString(intent) && strcmp(intent->valuestring, "chat") == 0,
              "voting winner=chat", "expected chat wins 0.9 > 0.6");
        /* 置信度 = 0.9 / (0.6+0.9) = 0.6 */
        if (cJSON_IsNumber(conf)) {
            double c = conf->valuedouble;
            CHECK(c > 0.59 && c < 0.61, "voting confidence=0.6", "confidence mismatch");
        } else {
            TEST_FAIL("voting confidence field", "confidence not a number");
        }
        cJSON_Delete(dec);
    }
#else
    CHECK(res.decision_json != NULL, "voting decision present", "decision missing");
#endif

    cog_review_result_free(&res);
    return 0;
}

/* ==================== 4. 问题审查合并 ==================== */

static int test_cog_review_aggregate_problem(void)
{
    cog_review_result_t res;
    cog_review_result_init(&res);

    res.opinions[0].role = COG_REVIEW_ROLE_INTENT_CONFIRM;
    AIRY_STRNCPY_TERM(res.opinions[0].agent_id, "intent-confirmer",
                      sizeof(res.opinions[0].agent_id));
    res.opinions[0].opinion_json =
        AIRY_STRDUP("{\"intent\":\"task\",\"confidence\":0.8,\"reason\":\"executable\"}");

    res.opinions[1].role = COG_REVIEW_ROLE_PROBLEM;
    AIRY_STRNCPY_TERM(res.opinions[1].agent_id, "problem-reviewer",
                      sizeof(res.opinions[1].agent_id));
    res.opinions[1].opinion_json = AIRY_STRDUP(
        "{\"clarify_needed\":1,\"risks\":[\"no target env\",\"credentials unknown\"],"
        "\"missing\":[\"target region\"]}");
    res.opinion_count = 2;

    airy_err_t err = cog_review_aggregate(&res);
    CHECK(err == AIRY_SUCCESS, "aggregate problem -> SUCCESS", "aggregate failed");

#ifdef AIRY_HAS_CJSON
    cJSON *dec = cJSON_Parse(res.decision_json);
    CHECK(dec != NULL, "problem decision parses", "invalid decision JSON");
    if (dec) {
        cJSON *clar = cJSON_GetObjectItem(dec, "clarify_needed");
        cJSON *risks = cJSON_GetObjectItem(dec, "risk_count");
        cJSON *intent = cJSON_GetObjectItem(dec, "intent");
        CHECK(cJSON_IsNumber(clar) && clar->valueint == 1, "clarify_needed=1",
              "clarify aggregation failed");
        CHECK(cJSON_IsNumber(risks) && risks->valueint == 2, "risk_count=2",
              "risk aggregation failed");
        CHECK(cJSON_IsString(intent) && strcmp(intent->valuestring, "task") == 0,
              "intent=task", "intent mismatch");
        cJSON_Delete(dec);
    }
#endif

    cog_review_result_free(&res);
    return 0;
}

/* ==================== 5. 资源释放安全 ==================== */

static int test_cog_review_free_safety(void)
{
    cog_review_result_free(NULL);
    TEST_PASS("free(NULL) is safe");

    cog_review_result_t res;
    cog_review_result_init(&res);
    res.opinions[0].role = COG_REVIEW_ROLE_INTENT_CONFIRM;
    res.opinions[0].opinion_json = AIRY_STRDUP("{\"intent\":\"agent\",\"confidence\":0.7}");
    res.opinion_count = 1;
    res.decision_json = AIRY_STRDUP("{\"intent\":\"agent\"}");
    cog_review_result_free(&res);
    CHECK(res.opinion_count == 0 && res.decision_json == NULL,
          "free releases opinions+decision", "leak on free");
    return 0;
}

/* ==================== 6. 并行度裁剪（2.5.x） ==================== */

static int test_cog_review_parallel_clamp(void)
{
    /* 无 LLM 时 max_parallel 不影响降级路径（返回 SUCCESS + degraded=1） */
    cog_review_result_t res;
    cog_review_result_init(&res);
    const char *input = "x";
    airy_err_t err = cog_review_run(NULL, NULL, input, 1, 1, &res);
    CHECK(err == AIRY_SUCCESS && res.degraded == 1, "max_parallel=1 no-LLM degrade",
          "parallel clamp must not break no-LLM path");
    cog_review_result_free(&res);

    /* 显式超出上限 → 裁剪到 COG_REVIEW_MAX_PARALLEL（仍需 LLM 才走 worker，
     * 无 LLM 仅验证参数被安全接受） */
    cog_review_result_init(&res);
    err = cog_review_run(NULL, NULL, input, 1, 64, &res);
    CHECK(err == AIRY_SUCCESS && res.degraded == 1, "max_parallel overflow safe",
          "parallel cap must be accepted safely");
    cog_review_result_free(&res);
    return 0;
}

/* ==================== 7. 边界/覆盖角色汇总 ==================== */

static int test_cog_review_aggregate_multi_roles(void)
{
    cog_review_result_t res;
    cog_review_result_init(&res);

    /* 四个角色并行意见：intent(task,0.7) + problem(1 风险) + boundary(1 风险)
     * + coverage(clarify=1) —— 风险数应合并累计、clarify 任一为真即真 */
    res.opinions[0].role = COG_REVIEW_ROLE_INTENT_CONFIRM;
    AIRY_STRNCPY_TERM(res.opinions[0].agent_id, "intent-confirmer",
                      sizeof(res.opinions[0].agent_id));
    res.opinions[0].opinion_json =
        AIRY_STRDUP("{\"intent\":\"task\",\"confidence\":0.7}");

    res.opinions[1].role = COG_REVIEW_ROLE_PROBLEM;
    AIRY_STRNCPY_TERM(res.opinions[1].agent_id, "problem-reviewer",
                      sizeof(res.opinions[1].agent_id));
    res.opinions[1].opinion_json =
        AIRY_STRDUP("{\"clarify_needed\":0,\"risks\":[\"no env\"]}");

    res.opinions[2].role = COG_REVIEW_ROLE_BOUNDARY;
    AIRY_STRNCPY_TERM(res.opinions[2].agent_id, "boundary-checker",
                      sizeof(res.opinions[2].agent_id));
    res.opinions[2].opinion_json =
        AIRY_STRDUP("{\"clarify_needed\":0,\"risks\":[\"quota limit\"]}");

    res.opinions[3].role = COG_REVIEW_ROLE_COVERAGE;
    AIRY_STRNCPY_TERM(res.opinions[3].agent_id, "coverage-checker",
                      sizeof(res.opinions[3].agent_id));
    res.opinions[3].opinion_json =
        AIRY_STRDUP("{\"clarify_needed\":1,\"risks\":[]}");
    res.opinion_count = 4;

    airy_err_t err = cog_review_aggregate(&res);
    CHECK(err == AIRY_SUCCESS, "aggregate multi-role -> SUCCESS", "aggregate failed");

#ifdef AIRY_HAS_CJSON
    cJSON *dec = cJSON_Parse(res.decision_json);
    CHECK(dec != NULL, "multi-role decision parses", "invalid decision JSON");
    if (dec) {
        cJSON *clar = cJSON_GetObjectItem(dec, "clarify_needed");
        cJSON *risks = cJSON_GetObjectItem(dec, "risk_count");
        cJSON *intent = cJSON_GetObjectItem(dec, "intent");
        cJSON *roles = cJSON_GetObjectItem(dec, "roles");
        CHECK(cJSON_IsNumber(clar) && clar->valueint == 1, "clarify merged across roles",
              "coverage clarify not aggregated");
        CHECK(cJSON_IsNumber(risks) && risks->valueint == 2, "risk_count=2 across roles",
              "boundary risk not merged");
        CHECK(cJSON_IsString(intent) && strcmp(intent->valuestring, "task") == 0,
              "intent=task", "intent mismatch");
        CHECK(cJSON_IsNumber(roles) && roles->valueint == 4, "roles=4",
              "role count mismatch");
        cJSON_Delete(dec);
    }
#endif

    cog_review_result_free(&res);
    return 0;
}

/* ==================== 8. 补全闭包注入 ==================== */

/**
 * 机制核补全闭包的测试替身：记录调用次数并按契约返回一份静态响应。
 * 静态响应避免分配；测试二进制未注入 LLM ops（are_ops_get_llm()==NULL），
 * worker 不会对返回值调用 response_free，故无需释放。max_parallel=1 时
 * 仅单 worker 单线程，无并发写。
 */
typedef struct {
    int calls;
    const char *reply;
} fake_completer_t;

static llm_response_t g_fake_resp;
static llm_message_t g_fake_choices[1];

static int fake_complete(void *ctx, const llm_request_config_t *cfg,
                         llm_response_t **out_response)
{
    fake_completer_t *f = (fake_completer_t *)ctx;
    if (!f || !cfg || !out_response)
        return -1;
    f->calls++;
    g_fake_choices[0].role = "assistant";
    g_fake_choices[0].content = f->reply;
    memset(&g_fake_resp, 0, sizeof(g_fake_resp));
    g_fake_resp.choices = g_fake_choices;
    g_fake_resp.choice_count = 1;
    *out_response = &g_fake_resp;
    return 0;
}

static int test_cog_review_complete_inject(void)
{
    fake_completer_t fake;
    fake.calls = 0;
    fake.reply = "{\"intent\":\"chat\",\"confidence\":0.8,\"reason\":\"greeting\"}";

    cog_review_result_t res;
    cog_review_result_init(&res);

    const char *input = "hello there";
    airy_err_t err = cog_review_run(fake_complete, &fake, input, strlen(input), 1, &res);
    CHECK(err == AIRY_SUCCESS, "run(complete) -> SUCCESS", "expected SUCCESS");
    CHECK(fake.calls >= 1, "complete closure invoked", "mechanism closure not called");
    CHECK(res.opinion_count >= 1, "complete -> opinions collected", "no opinion collected");
    CHECK(res.degraded == 0, "complete path -> not degraded", "should not degrade with opinion");

#ifdef AIRY_HAS_CJSON
    cJSON *dec = res.decision_json ? cJSON_Parse(res.decision_json) : NULL;
    CHECK(dec != NULL, "complete decision parses", "invalid decision JSON");
    if (dec) {
        cJSON *intent = cJSON_GetObjectItem(dec, "intent");
        CHECK(cJSON_IsString(intent) && strcmp(intent->valuestring, "chat") == 0,
              "complete intent=chat", "intent from closure reply mismatch");
        cJSON_Delete(dec);
    }
#endif

    cog_review_result_free(&res);
    return 0;
}

int main(void)
{
    printf("=== Cognition CogReview Strategy Tests ===\n\n");

    test_cog_review_null_args();
    test_cog_review_degraded();
    test_cog_review_aggregate_voting();
    test_cog_review_aggregate_problem();
    test_cog_review_free_safety();
    test_cog_review_parallel_clamp();
    test_cog_review_aggregate_multi_roles();
    test_cog_review_complete_inject();

    printf("\n%d/%d passed\n", tests_passed, tests_run);
    if (tests_passed != tests_run) {
        fprintf(stderr, "FAILED: %d/%d tests passed\n", tests_passed, tests_run);
        return 1;
    }
    return 0;
}
