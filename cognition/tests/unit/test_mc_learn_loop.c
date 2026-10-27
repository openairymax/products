// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * test_mc_learn_loop.c - 元认知持久学习闭环验证测试 (MCL)
 *
 * 验证覆盖:
 *   MCL-1: 模式检测建档只以窗口为种子，重复检测不再累计
 *   MCL-2: feedback 回填驱动模式计数与失败率
 *   MCL-3: 未知模式不给出策略建议（AIRY_ENOENT + NONE）
 *   MCL-4: 样本充分的已知模式修正本次评估策略
 *   MCL-5: 预纠正提示按步型命中并计数
 *   MCL-6: stats 暴露学习段（口径可查）
 *
 * 该测试自包含，不依赖外部服务（无LLM调用，无网络）。
 */

#include "metacognition.h"
#include "thinking_chain.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)                      \
    do {                                    \
        printf("  Running " #name "...\n"); \
        test_##name();                      \
        printf("  PASSED\n");               \
    } while (0)

/* 低质输出：相关性极低、无结构，稳定落入需修正分级。 */
#define BAD_CONTENT "x"
#define REQ_TEXT "Explain quantum computing"

static void eval_bad(airy_metacognition_t *mc, uint32_t id, tc_step_type_t type,
                     mc_evaluation_result_t *out)
{
    airy_thinking_step_t st;
    __builtin_memset(&st, 0, sizeof(st));
    st.step_id = id;
    st.type = type;
    st.status = TC_STATUS_COMPLETED;
    st.content = (char *)BAD_CONTENT;
    st.content_len = strlen(BAD_CONTENT);
    st.raw_input = (char *)REQ_TEXT;
    st.raw_input_len = strlen(REQ_TEXT);
    st.confidence = 0.8f;

    assert(airy_mc_evaluate_step(mc, &st, NULL, 0, out) == AIRY_SUCCESS);
    assert(out->strategy != MC_CORRECT_NONE);
    if (out->critique_text) {
        free(out->critique_text);
        out->critique_text = NULL;
    }
}

/* ============================================================================
 * MCL-1: 重复检测不再重复累计同一批历史记录
 * ============================================================================ */
TEST(mcl1_detect_seed_no_inflation)
{
    airy_metacognition_t *mc = NULL;
    assert(airy_mc_create(&mc) == AIRY_SUCCESS);

    mc_evaluation_result_t r;
    for (uint32_t i = 0; i < 4; i++)
        eval_bad(mc, i + 1, TC_STEP_GENERATION, &r);

    mc_error_pattern_t *pats = NULL;
    size_t count = 0;
    assert(airy_mc_detect_patterns(mc, &pats, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(pats != NULL);
    assert(pats[0].occurrence_count == 4);
    assert(pats[0].failure_count == 4);
    assert(pats[0].failure_rate > 0.99f);
    assert(pats[0].is_active == 1);
    assert(mc->patterns_detected == 1);

    uint64_t occ = pats[0].occurrence_count;
    uint64_t fail = pats[0].failure_count;
    assert(airy_mc_detect_patterns(mc, &pats, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(pats[0].occurrence_count == occ);
    assert(pats[0].failure_count == fail);
    assert(mc->patterns_detected == 1);

    airy_mc_destroy(mc);
}

/* ============================================================================
 * MCL-2: feedback 回填驱动模式计数与失败率
 * ============================================================================ */
TEST(mcl2_feedback_accumulates)
{
    airy_metacognition_t *mc = NULL;
    assert(airy_mc_create(&mc) == AIRY_SUCCESS);

    mc_evaluation_result_t r;
    eval_bad(mc, 1, TC_STEP_GENERATION, &r);
    assert(mc->pattern_count == 0);

    assert(airy_mc_feedback(mc, r.overall_score, 0) == AIRY_SUCCESS);
    assert(mc->pattern_count == 1);
    assert(mc->patterns[0].occurrence_count == 1);
    assert(mc->patterns[0].failure_count == 1);
    assert(mc->patterns[0].failure_rate > 0.99f);

    assert(airy_mc_feedback(mc, r.overall_score, 1) == AIRY_SUCCESS);
    assert(mc->patterns[0].occurrence_count == 2);
    assert(mc->patterns[0].failure_count == 1);
    assert(mc->patterns[0].failure_rate > 0.49f && mc->patterns[0].failure_rate < 0.51f);
    assert(mc->patterns[0].best_strategy == r.strategy);

    airy_mc_destroy(mc);
}

/* ============================================================================
 * MCL-3: 未知模式不给出策略建议
 * ============================================================================ */
TEST(mcl3_unknown_pattern_no_hint)
{
    airy_metacognition_t *mc = NULL;
    assert(airy_mc_create(&mc) == AIRY_SUCCESS);

    mc_correction_strategy_t strategy = MC_CORRECT_ESCALATE;
    assert(airy_mc_learn_best_strategy(mc, "generation_rel_0.1", &strategy) == AIRY_ENOENT);
    assert(strategy == MC_CORRECT_NONE);

    airy_mc_destroy(mc);
}

/* ============================================================================
 * MCL-4: 样本充分的已知模式修正本次评估策略
 * ============================================================================ */
TEST(mcl4_learned_strategy_upgrade)
{
    airy_metacognition_t *mc = NULL;
    assert(airy_mc_create(&mc) == AIRY_SUCCESS);

    mc_evaluation_result_t r;
    for (uint32_t i = 0; i < 3; i++)
        eval_bad(mc, i + 1, TC_STEP_GENERATION, &r);
    mc_correction_strategy_t baseline = r.strategy;
    assert(baseline != MC_CORRECT_ESCALATE);

    mc_error_pattern_t *pats = NULL;
    size_t count = 0;
    assert(airy_mc_detect_patterns(mc, &pats, &count) == AIRY_SUCCESS);
    assert(count == 1);
    char key[128];
    snprintf(key, sizeof(key), "%s", pats[0].pattern_key);

    /* 检测窗口种子（3 条）不足以支撑建议。 */
    mc_correction_strategy_t strategy = MC_CORRECT_ESCALATE;
    assert(airy_mc_learn_best_strategy(mc, key, &strategy) == AIRY_SUCCESS);
    assert(strategy == MC_CORRECT_NONE);

    /* 真实回填把样本推到 6 条，最优策略为 ESCALATE。 */
    for (int i = 0; i < 3; i++)
        assert(airy_mc_record_strategy_result(mc, key, MC_CORRECT_ESCALATE, 1) == AIRY_SUCCESS);
    assert(airy_mc_learn_best_strategy(mc, key, &strategy) == AIRY_SUCCESS);
    assert(strategy == MC_CORRECT_ESCALATE);

    mc_evaluation_result_t upgraded;
    eval_bad(mc, 10, TC_STEP_GENERATION, &upgraded);
    assert(upgraded.strategy == MC_CORRECT_ESCALATE);
    assert(upgraded.severity == MC_SEV_CRITICAL);

    airy_mc_destroy(mc);
}

/* ============================================================================
 * MCL-5: 预纠正提示按步型命中并计数
 * ============================================================================ */
TEST(mcl5_preempt_hint)
{
    airy_metacognition_t *mc = NULL;
    assert(airy_mc_create(&mc) == AIRY_SUCCESS);

    mc_evaluation_result_t r;
    for (uint32_t i = 0; i < 3; i++)
        eval_bad(mc, i + 1, TC_STEP_GENERATION, &r);

    mc_error_pattern_t *pats = NULL;
    size_t count = 0;
    assert(airy_mc_detect_patterns(mc, &pats, &count) == AIRY_SUCCESS);
    assert(count == 1);

    char *hint = NULL;
    size_t hint_len = 0;
    assert(airy_mc_preemptive_check(mc, TC_STEP_GENERATION, REQ_TEXT, strlen(REQ_TEXT), &hint,
                                    &hint_len) == 1);
    assert(hint != NULL);
    assert(hint_len == strlen(hint));
    assert(strstr(hint, "PREEMPTIVE GUIDANCE") != NULL);
    assert(mc->preemptive_corrections == 1);
    free(hint);

    char *other = NULL;
    size_t other_len = 0;
    assert(airy_mc_preemptive_check(mc, TC_STEP_AUDIT, REQ_TEXT, strlen(REQ_TEXT), &other,
                                    &other_len) == 0);
    assert(other == NULL);
    assert(other_len == 0);
    assert(mc->preemptive_corrections == 1);

    airy_mc_destroy(mc);
}

/* ============================================================================
 * MCL-6: stats 暴露学习段
 * ============================================================================ */
TEST(mcl6_stats_learning_section)
{
    airy_metacognition_t *mc = NULL;
    assert(airy_mc_create(&mc) == AIRY_SUCCESS);

    mc_evaluation_result_t r;
    eval_bad(mc, 1, TC_STEP_GENERATION, &r);
    assert(airy_mc_feedback(mc, r.overall_score, 0) == AIRY_SUCCESS);

    char *json = NULL;
    assert(airy_mc_stats(mc, &json) == AIRY_SUCCESS);
    assert(json != NULL);
    printf("    stats: %s\n", json);
    assert(strstr(json, "\"learning\":{\"patterns\":1") != NULL);
    assert(strstr(json, "\"detected\":0") != NULL);
    assert(strstr(json, "\"preemptive\":0") != NULL);
    assert(strstr(json, "\"effectiveness\":") != NULL);
    free(json);

    airy_mc_destroy(mc);
}

/* ============================================================================
 * 主入口
 * ============================================================================ */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("=========================================\n");
    printf("  Metacognition Learning Loop Tests\n");
    printf("  MetaCognition Learning (MCL)\n");
    printf("=========================================\n\n");

    printf("--- MCL-1: Detect Seed Without Inflation ---\n");
    RUN_TEST(mcl1_detect_seed_no_inflation);

    printf("\n--- MCL-2: Feedback Accumulation ---\n");
    RUN_TEST(mcl2_feedback_accumulates);

    printf("\n--- MCL-3: Unknown Pattern Gives No Hint ---\n");
    RUN_TEST(mcl3_unknown_pattern_no_hint);

    printf("\n--- MCL-4: Learned Strategy Upgrade ---\n");
    RUN_TEST(mcl4_learned_strategy_upgrade);

    printf("\n--- MCL-5: Preemptive Hint By Step Type ---\n");
    RUN_TEST(mcl5_preempt_hint);

    printf("\n--- MCL-6: Stats Learning Section ---\n");
    RUN_TEST(mcl6_stats_learning_section);

    printf("\n=========================================\n");
    printf("  All metacognition learning tests PASSED\n");
    printf("=========================================\n");

    return 0;
}
