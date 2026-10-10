// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gccp_strategy.c
 * @brief GCCP 策略载荷测试（products/cognition，M5-4 迁出）
 *
 * 测试内容：
 *   1. 参数校验：probe/confirm/step 非法输入 → EINVAL
 *   2. 简单对话前置门：问候语短路（need_interaction=0）且不调 LLM
 *   3. 无闭包降级：complete=NULL → 启发式 4+1 问题集
 *   4. LLM 失败降级：闭包返回失败 → 启发式问题集
 *   5. LLM JSON 路径：probe 解析 need_interaction/prefill/questions
 *   6. 问题数硬截断：LLM 返回超量问题 → 截断到 5
 *   7. step 跳过收敛：answered=0 → done=1 且不调 LLM
 *   8. step 无 LLM：闭包缺失 → done=0 空 question（队列交还调用方）
 *   9. step LLM 收敛/追问：done 判定 + 上一问上下文注入（q8a）
 *  10. confirm 状态判定：confidence 阈值 → CONFIRMED/AMBIGUOUS
 *  11. confirm 缺 confidence 字段 → 阈值默认（q8a，保 CONFIRMED）
 *  12. confirm 降级：无 LLM → DEGRADED
 *  13. 资源释放：probe_free/goal_free NULL 安全
 *
 * @note 不依赖 llm_d 守护进程：LLM 路径经测试替身（fake complete 闭包）
 *       驱动；测试二进制未注入 llm ops（are_ops_get_llm()==NULL），
 *       响应释放为 no-op，故替身返回静态响应无泄漏。
 * @note 不使用 assert() 执行副作用操作（NDEBUG 下 assert 展开为 ((void)0)）。
 */

#include "gccp_strategy.h"

#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ==================== 补全闭包测试替身 ==================== */

typedef struct {
    int calls;
    int fail;          /* 非 0：模拟 LLM 调用失败（返回非 0） */
    const char *reply; /* 静态 JSON 响应文本 */
    char *last_input;  /* 最近一次 user input（strdup 快照，断言用） */
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
    free(f->last_input);
    f->last_input = NULL;
    if (cfg->messages && cfg->message_count > 0 && cfg->messages[cfg->message_count - 1].content)
        f->last_input = strdup(cfg->messages[cfg->message_count - 1].content);
    if (f->fail)
        return -1;
    g_fake_choices[0].role = "assistant";
    g_fake_choices[0].content = f->reply;
    memset(&g_fake_resp, 0, sizeof(g_fake_resp));
    g_fake_resp.choices = g_fake_choices;
    g_fake_resp.choice_count = 1;
    *out_response = &g_fake_resp;
    return 0;
}

static void fake_init(fake_completer_t *f, const char *reply)
{
    f->calls = 0;
    f->fail = 0;
    f->reply = reply;
    f->last_input = NULL;
}

static void fake_release(fake_completer_t *f)
{
    free(f->last_input);
    f->last_input = NULL;
}

/* ==================== 1. 参数校验 ==================== */

static int test_gccp_null_args(void)
{
    airy_gccp_probe_t *probe = NULL;
    airy_gccp_goal_t *goal = NULL;
    airy_gccp_step_t step;

    CHECK(gccp_probe(NULL, NULL, NULL, NULL, 0, &probe) == AIRY_EINVAL,
          "probe(NULL input) -> EINVAL", "expected EINVAL");
    CHECK(gccp_probe(NULL, NULL, NULL, "hello", 5, NULL) == AIRY_EINVAL,
          "probe(NULL out) -> EINVAL", "expected EINVAL");
    CHECK(gccp_probe(NULL, NULL, NULL, "hello", 0, &probe) == AIRY_EINVAL,
          "probe(empty input) -> EINVAL", "expected EINVAL");
    CHECK(gccp_confirm(NULL, NULL, NULL, NULL, 0, NULL, &goal) == AIRY_EINVAL,
          "confirm(NULL input) -> EINVAL", "expected EINVAL");
    CHECK(gccp_confirm(NULL, NULL, NULL, "hello", 0, NULL, &goal) == AIRY_EINVAL,
          "confirm(empty input) -> EINVAL", "expected EINVAL");
    CHECK(gccp_step(NULL, NULL, NULL, NULL, 0, NULL, 1, NULL, NULL) == AIRY_EINVAL,
          "step(NULL out_step) -> EINVAL", "expected EINVAL");
    CHECK(gccp_step(NULL, NULL, NULL, NULL, 0, NULL, 1, NULL, &step) == AIRY_EINVAL,
          "step(NULL input) -> EINVAL", "expected EINVAL");
    return 0;
}

/* ==================== 2. 简单对话前置门 ==================== */

static int test_gccp_simple_gate(void)
{
    fake_completer_t fake;
    fake_init(&fake, "{}");
    airy_gccp_probe_t *probe = NULL;

    airy_err_t err = gccp_probe(fake_complete, &fake, NULL, "你好", 6, &probe);
    CHECK(err == AIRY_EOK, "simple gate -> EOK", "expected EOK");
    CHECK(probe != NULL && probe->need_interaction == 0, "simple gate need_interaction=0",
          "must skip interaction");
    CHECK(probe != NULL && probe->question_count == 0, "simple gate zero questions",
          "must not carry questions");
    CHECK(fake.calls == 0, "simple gate skips LLM", "LLM must not be called");
    CHECK(probe && probe->prefill && probe->prefill->status == AIRY_GCCP_STATUS_DEGRADED,
          "simple gate prefill degraded", "prefill should be degraded");

    airy_gccp_probe_free(probe);
    fake_release(&fake);
    return 0;
}

/* ==================== 3. 无闭包降级 ==================== */

static int test_gccp_no_closure_degrade(void)
{
    airy_gccp_probe_t *probe = NULL;
    airy_err_t err = gccp_probe(NULL, NULL, NULL, "帮我搭建一个持续集成流水线",
                                strlen("帮我搭建一个持续集成流水线"), &probe);
    CHECK(err == AIRY_EOK, "no closure -> EOK", "expected EOK");
    CHECK(probe != NULL && probe->need_interaction == 1, "heuristic need_interaction=1",
          "heuristic must interact");
    CHECK(probe && probe->question_count == 5, "heuristic 4+1 questions", "expected 5 questions");
    CHECK(probe && probe->questions && probe->questions[0].question[0] != '\0',
          "heuristic question text present", "question text missing");
    CHECK(probe && probe->prefill && probe->prefill->raw_prompt != NULL,
          "heuristic prefill raw_prompt", "raw_prompt missing");

    airy_gccp_probe_free(probe);
    return 0;
}

/* ==================== 4. LLM 失败降级 ==================== */

static int test_gccp_llm_fail_degrade(void)
{
    fake_completer_t fake;
    fake_init(&fake, NULL);
    fake.fail = 1;
    airy_gccp_probe_t *probe = NULL;

    airy_err_t err = gccp_probe(fake_complete, &fake, NULL, "部署服务到生产环境并监控",
                                strlen("部署服务到生产环境并监控"), &probe);
    CHECK(err == AIRY_EOK, "LLM fail -> EOK (degraded)", "expected EOK");
    CHECK(probe != NULL && probe->question_count == 5, "LLM fail -> heuristic questions",
          "expected heuristic 5 questions");
    CHECK(fake.calls == 1, "closure attempted once", "closure must be called");

    airy_gccp_probe_free(probe);
    fake_release(&fake);
    return 0;
}

/* ==================== 5. LLM JSON 路径 ==================== */

static int test_gccp_probe_llm_json(void)
{
    const char *reply =
        "{\"need_interaction\":1,\"confidence\":0.7,"
        "\"prefill\":{\"endpoint\":\"流水线上线\",\"confidence\":0.7},"
        "\"questions\":["
        "{\"id\":\"endpoint\",\"question\":\"终点是什么？\",\"hint\":\"终态\",\"required\":1},"
        "{\"id\":\"verify\",\"question\":\"如何验收？\",\"hint\":\"标准\",\"required\":1}]}";
    fake_completer_t fake;
    fake_init(&fake, reply);
    airy_gccp_probe_t *probe = NULL;

    airy_err_t err = gccp_probe(fake_complete, &fake, NULL, "搭建持续集成流水线",
                                strlen("搭建持续集成流水线"), &probe);
    CHECK(err == AIRY_EOK, "probe LLM -> EOK", "expected EOK");
    CHECK(probe != NULL && probe->need_interaction == 1, "LLM need_interaction=1", "mismatch");
    CHECK(probe && probe->question_count == 2, "LLM questions=2", "expected 2 questions");
    CHECK(probe && probe->questions && strcmp(probe->questions[0].id, "endpoint") == 0,
          "LLM question id=endpoint", "id mismatch");
    CHECK(probe && probe->prefill && probe->prefill->goal_endpoint &&
              strcmp(probe->prefill->goal_endpoint, "流水线上线") == 0,
          "LLM prefill endpoint", "prefill endpoint mismatch");

    airy_gccp_probe_free(probe);
    fake_release(&fake);
    return 0;
}

/* ==================== 6. 问题数硬截断 ==================== */

static int test_gccp_probe_question_clamp(void)
{
    const char *reply =
        "{\"need_interaction\":1,\"questions\":["
        "{\"id\":\"q1\",\"question\":\"1\",\"required\":1},"
        "{\"id\":\"q2\",\"question\":\"2\",\"required\":1},"
        "{\"id\":\"q3\",\"question\":\"3\",\"required\":1},"
        "{\"id\":\"q4\",\"question\":\"4\",\"required\":1},"
        "{\"id\":\"q5\",\"question\":\"5\",\"required\":1},"
        "{\"id\":\"q6\",\"question\":\"6\",\"required\":1},"
        "{\"id\":\"q7\",\"question\":\"7\",\"required\":1}]}";
    fake_completer_t fake;
    fake_init(&fake, reply);
    airy_gccp_probe_t *probe = NULL;

    airy_err_t err = gccp_probe(fake_complete, &fake, NULL, "复杂多步任务请澄清目标边界",
                                strlen("复杂多步任务请澄清目标边界"), &probe);
    CHECK(err == AIRY_EOK, "clamp probe -> EOK", "expected EOK");
    CHECK(probe && probe->question_count == 5, "questions clamped to 5", "expected clamp to 5");

    airy_gccp_probe_free(probe);
    fake_release(&fake);
    return 0;
}

/* ==================== 7. step 跳过收敛 ==================== */

static int test_gccp_step_skip(void)
{
    fake_completer_t fake;
    fake_init(&fake, "{}");
    airy_gccp_step_t step;

    airy_err_t err = gccp_step(fake_complete, &fake, NULL, "task", 4, "{}", 0, NULL, &step);
    CHECK(err == AIRY_EOK, "step skip -> EOK", "expected EOK");
    CHECK(step.done == 1, "skip -> done=1", "skip must converge");
    CHECK(fake.calls == 0, "skip skips LLM", "LLM must not be called");

    fake_release(&fake);
    return 0;
}

/* ==================== 8. step 无 LLM ==================== */

static int test_gccp_step_no_llm(void)
{
    airy_gccp_step_t step;
    airy_err_t err = gccp_step(NULL, NULL, NULL, "task", 4, "{}", 1, NULL, &step);
    CHECK(err == AIRY_EOK, "step no LLM -> EOK", "expected EOK");
    CHECK(step.done == 0 && step.question[0] == '\0',
          "no LLM -> empty question (queue back to caller)", "must not fabricate questions");
    return 0;
}

/* ==================== 9. step LLM 收敛/追问 ==================== */

static int test_gccp_step_llm(void)
{
    /* 收敛：done=1 */
    fake_completer_t fake;
    fake_init(&fake,
              "{\"done\":1,\"reasoning\":\"目标已清晰\",\"next\":{\"question\":\"不该出现\"}}");
    airy_gccp_step_t step;
    airy_err_t err = gccp_step(fake_complete, &fake, NULL, "task", 4, "{\"q1\":\"a\"}", 1, NULL,
                               &step);
    CHECK(err == AIRY_EOK, "step done -> EOK", "expected EOK");
    CHECK(step.done == 1, "LLM done=1 -> converge", "expected done");
    CHECK(strstr(step.reasoning, "清晰") != NULL, "reasoning captured", "reasoning missing");
    CHECK(step.question[0] == '\0', "done=1 suppresses next question", "next must be dropped");
    fake_release(&fake);

    /* 追问：done=0 + next；验证上一问文本注入（q8a 上下文组装） */
    fake_init(&fake,
              "{\"done\":0,\"reasoning\":\"需补充\",\"next\":{\"question\":\"预算多少？\","
              "\"hint\":\"金额区间\"}}");
    airy_gccp_question_t last_q;
    memset(&last_q, 0, sizeof(last_q));
    snprintf(last_q.id, sizeof(last_q.id), "bottleneck");
    snprintf(last_q.question, sizeof(last_q.question), "有哪些约束？");
    memset(&step, 0, sizeof(step));
    err = gccp_step(fake_complete, &fake, NULL, "task", 4, "{\"bottleneck\":\"时间紧\"}", 1,
                    &last_q, &step);
    CHECK(err == AIRY_EOK, "step followup -> EOK", "expected EOK");
    CHECK(step.done == 0, "LLM done=0 -> continue", "expected continue");
    CHECK(strcmp(step.question, "预算多少？") == 0, "followup question captured", "q mismatch");
    CHECK(strcmp(step.hint, "金额区间") == 0, "followup hint captured", "hint mismatch");
    CHECK(fake.last_input && strstr(fake.last_input, "上一问（bottleneck）：有哪些约束？") != NULL,
          "last question injected into context", "q8a context injection missing");
    CHECK(fake.last_input && strstr(fake.last_input, "时间紧") != NULL,
          "answers injected into context", "answers missing");
    fake_release(&fake);
    return 0;
}

/* ==================== 10. confirm 状态判定 ==================== */

static int test_gccp_confirm_threshold(void)
{
    /* 高置信 → CONFIRMED */
    fake_completer_t fake;
    fake_init(&fake,
              "{\"endpoint\":\"上线\",\"confidence\":0.9}");
    airy_gccp_goal_t *goal = NULL;
    airy_err_t err = gccp_confirm(fake_complete, &fake, NULL, "task", 4, "{}", &goal);
    CHECK(err == AIRY_EOK, "confirm LLM -> EOK", "expected EOK");
    CHECK(goal && goal->status == AIRY_GCCP_STATUS_CONFIRMED, "conf 0.9 -> CONFIRMED",
          "expected CONFIRMED");
    airy_gccp_goal_free(goal);
    fake_release(&fake);

    /* 低置信 → AMBIGUOUS */
    fake_init(&fake, "{\"endpoint\":\"上线\",\"confidence\":0.3}");
    goal = NULL;
    err = gccp_confirm(fake_complete, &fake, NULL, "task", 4, "{}", &goal);
    CHECK(err == AIRY_EOK, "confirm low-conf -> EOK", "expected EOK");
    CHECK(goal && goal->status == AIRY_GCCP_STATUS_AMBIGUOUS, "conf 0.3 -> AMBIGUOUS",
          "expected AMBIGUOUS");
    airy_gccp_goal_free(goal);
    fake_release(&fake);
    return 0;
}

/* ==================== 11. confirm 缺 confidence 字段（q8a） ==================== */

static int test_gccp_confirm_missing_confidence(void)
{
    fake_completer_t fake;
    fake_init(&fake, "{\"endpoint\":\"上线\"}");
    airy_gccp_goal_t *goal = NULL;

    airy_err_t err = gccp_confirm(fake_complete, &fake, NULL, "task", 4, "{}", &goal);
    CHECK(err == AIRY_EOK, "confirm no-confidence -> EOK", "expected EOK");
    CHECK(goal && goal->status == AIRY_GCCP_STATUS_CONFIRMED,
          "omitted confidence -> threshold default (CONFIRMED)", "q8a regression");
    CHECK(goal && goal->raw_prompt && strcmp(goal->raw_prompt, "task") == 0,
          "raw_prompt preserved", "raw_prompt missing");
    airy_gccp_goal_free(goal);
    fake_release(&fake);
    return 0;
}

/* ==================== 12. confirm 降级 ==================== */

static int test_gccp_confirm_degrade(void)
{
    airy_gccp_goal_t *goal = NULL;
    airy_err_t err = gccp_confirm(NULL, NULL, NULL, "task", 4, NULL, &goal);
    CHECK(err == AIRY_EOK, "confirm no LLM -> EOK", "expected EOK");
    CHECK(goal && goal->status == AIRY_GCCP_STATUS_DEGRADED, "no LLM -> DEGRADED",
          "expected DEGRADED");
    CHECK(goal && goal->goal_endpoint && strcmp(goal->goal_endpoint, "task") == 0,
          "degraded endpoint = full input", "endpoint mismatch");
    airy_gccp_goal_free(goal);
    return 0;
}

/* ==================== 13. 资源释放安全 ==================== */

static int test_gccp_free_safety(void)
{
    airy_gccp_probe_free(NULL);
    airy_gccp_goal_free(NULL);
    TEST_PASS("free(NULL) is safe");

    /* 完整 probe 释放后字段归零防悬垂 */
    airy_gccp_probe_t *probe = NULL;
    gccp_probe(NULL, NULL, NULL, "复杂任务需要澄清目标边界", strlen("复杂任务需要澄清目标边界"),
               &probe);
    CHECK(probe != NULL, "probe built for free test", "probe missing");
    airy_gccp_probe_free(probe);
    TEST_PASS("probe free completes");
    return 0;
}

int main(void)
{
    printf("=== Cognition GCCP Strategy Tests ===\n\n");

    test_gccp_null_args();
    test_gccp_simple_gate();
    test_gccp_no_closure_degrade();
    test_gccp_llm_fail_degrade();
    test_gccp_probe_llm_json();
    test_gccp_probe_question_clamp();
    test_gccp_step_skip();
    test_gccp_step_no_llm();
    test_gccp_step_llm();
    test_gccp_confirm_threshold();
    test_gccp_confirm_missing_confidence();
    test_gccp_confirm_degrade();
    test_gccp_free_safety();

    printf("\n%d/%d passed\n", tests_passed, tests_run);
    if (tests_passed != tests_run) {
        fprintf(stderr, "FAILED: %d/%d tests passed\n", tests_passed, tests_run);
        return 1;
    }
    return 0;
}
