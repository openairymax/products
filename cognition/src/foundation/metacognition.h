/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file metacognition.h
 * @brief MC 策略载荷内部头——评估历史、校准器与引擎布局、全量实现 API。
 *
 * 机制/策略分离（0.1.19 M5-4）：机制核消费面（不透明句柄、维度/策略/
 * 严重度枚举、评估值载体、错误模式载体、10 项 ops 表）已收口至契约头
 * mc.h（atoms/coreloopthree/include）；本头文件是策略载荷的**内部头**
 * ——评估历史环、置信度校准器与 airy_metacognition 宿主布局、以及机制
 * 核不直接消费的扩展 API（quick 评估、校准、统计、历史、学习）仅限
 * 载荷实现单元（foundation/）触达，机制侧不得 include 本头。
 *
 * Metacognition is an agent's ability to monitor and regulate its own
 * thinking process. This module implements the core logic of the S1
 * verification role in Thinkdual:
 * - Reasoning quality self-evaluation (multi-dimension scoring)
 * - Error detection and automatic correction
 * - Confidence calibration (prevent overconfidence/underconfidence)
 * - Thinking-process audit log
 */

#ifndef AIRY_RT_METACOGNITION_H
#define AIRY_RT_METACOGNITION_H

#include "mc.h"
#include "thinking_chain.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_evaluation_record mc_evaluation_record_t;

#define MC_MAX_HISTORY_RECORDS 256
#define MC_DEFAULT_CONFIDENCE 0.5f
#define MC_CALIBRATION_WINDOW 32
#define MC_MAX_CRITIQUE_LEN 4096

/**
 * @brief Evaluation history record.
 */
struct mc_evaluation_record {
    uint32_t step_id;
    uint64_t timestamp_ns;
    mc_evaluation_result_t result;
    const char *original_content;
    const char *corrected_content;
};

/**
 * @brief Confidence calibrator state.
 */
typedef struct {
    float calibration_sum;
    size_t calibration_count;
    float last_calibration_error;
    float overconfidence_rate;
    float underconfidence_rate;
    struct {
        float predicted;
        float actual;
    } history[MC_CALIBRATION_WINDOW];
    size_t history_index;
} mc_calibrator_t;

#define MC_MAX_PATTERNS 32
/**
 * @brief Metacognition engine (with persistent-learning extension).
 */
struct airy_metacognition {

    float acceptance_threshold;
    float auto_correct_threshold;
    int enable_confidence_calibration;
    int enable_learning;


    mc_evaluation_record_t *records;
    size_t record_capacity;
    size_t record_count;
    size_t record_head;

    mc_calibrator_t calibrator;


    uint64_t total_evaluations;
    uint64_t total_corrections;
    uint64_t total_rejections;
    uint64_t total_auto_fixes;
    uint64_t total_rerun_successes;

    airy_thinking_chain_t *chain;

    mc_error_pattern_t patterns[MC_MAX_PATTERNS];
    size_t pattern_count;
    float adaptive_acceptance_threshold;
    size_t consecutive_accepts;
    size_t consecutive_rejects;
    uint64_t patterns_detected;
    uint64_t preemptive_corrections;
    float learning_effectiveness;
};

/**
 * @brief Create a metacognition engine instance.
 * @param out_mc Output handle
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_mc_create(airy_metacognition_t **out_mc);

/**
 * @brief Destroy the metacognition engine.
 */
AIRY_API void airy_mc_destroy(airy_metacognition_t *mc);

/**
 * @brief Attach the thinking chain (for callback notifications).
 */
AIRY_API void airy_mc_set_chain(airy_metacognition_t *mc, airy_thinking_chain_t *chain);

/**
 * @brief Run a full multi-dimension evaluation on a thinking step.
 *
 * Core metacognition function. Simulates the S1 verification role,
 * evaluating relevance, accuracy, completeness, consistency, and clarity.
 *
 * @param step Thinking step to evaluate
 * @param context Current context-window content (for consistency check)
 * @param context_len Context length
 * @param out_result Evaluation result (caller frees critique_text)
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_mc_evaluate_step(airy_metacognition_t *mc, airy_thinking_step_t *step,
                                          const char *context, size_t context_len,
                                          mc_evaluation_result_t *out_result);

/**
 * @brief Quick evaluation (checks only key dimensions).
 *
 * Used in streaming-critique scenarios; faster than full evaluation.
 */
AIRY_API airy_err_t airy_mc_evaluate_quick(airy_metacognition_t *mc, airy_thinking_step_t *step,
                                           float *out_score, int *out_acceptable);

/**
 * @brief Decide and execute the correction action based on the evaluation result.
 *
 * @param step Thinking step to correct
 * @param eval Evaluation result
 * @param corrector_fn Correction callback (the actual re-generator)
 * @param corrector_user_data Callback user data
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_mc_correct(
    airy_metacognition_t *mc, airy_thinking_step_t *step, const mc_evaluation_result_t *eval,
    airy_err_t (*corrector_fn)(const char *input, size_t input_len, char **output,
                               size_t *output_len, void *user_data),
    void *corrector_user_data);

/**
 * @brief Check whether self-correction is needed (based on history patterns).
 *
 * @return 1=needs correction, 0=no, -1=error
 */
AIRY_API int airy_mc_should_self_correct(airy_metacognition_t *mc, tc_step_type_t step_type);

/**
 * @brief Calibrate confidence (based on historical accuracy).
 *
 * If the agent historically fails often at a certain confidence level,
 * lower the confidence value it reports in the future.
 */
AIRY_API float airy_mc_calibrate_confidence(airy_metacognition_t *mc, float raw_confidence);

/**
 * @brief Feed back the actual result (for calibration learning).
 *
 * Called after a step completes, telling the system whether it was
 * actually correct.
 */
AIRY_API airy_err_t airy_mc_feedback(airy_metacognition_t *mc, float predicted_confidence,
                                     int was_correct);

/**
 * @brief Get metacognition statistics (JSON format).
 */
AIRY_API airy_err_t airy_mc_stats(airy_metacognition_t *mc, char **out_json);

/**
 * @brief Get the most recent N evaluation records.
 */
AIRY_API airy_err_t airy_mc_get_history(airy_metacognition_t *mc, size_t count,
                                        mc_evaluation_record_t **out_records, size_t *out_count);

/**
 * @brief Reset the calibrator and history records.
 */
AIRY_API void airy_mc_reset(airy_metacognition_t *mc);

/**
 * @brief Extract error-pattern features from evaluation results.
 *
 * Analyzes the most recent N evaluation results to identify recurring
 * failure patterns. Patterns are based on step type, low-scoring
 * dimensions, input keywords, etc.
 *
 * @param mc Metacognition engine
 * @param out_patterns Output detected pattern array
 * @param out_count Output pattern count
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_mc_detect_patterns(airy_metacognition_t *mc,
                                            mc_error_pattern_t **out_patterns, size_t *out_count);

/**
 * @brief Learn the optimal correction strategy.
 *
 * Chooses the most effective correction strategy for a specific error
 * pattern based on historical data, by analyzing each strategy's success
 * rate in similar scenarios.
 *
 * @param mc Metacognition engine
 * @param pattern_key Pattern key name
 * @param out_strategy Output recommended strategy
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_mc_learn_best_strategy(airy_metacognition_t *mc, const char *pattern_key,
                                                mc_correction_strategy_t *out_strategy);

/**
 * @brief Pre-detection and pre-correction.
 *
 * Before S2 generation, checks whether the current input matches a known
 * failure pattern. If so, returns a suggested system-prompt prefix to
 * proactively avoid that class of error.
 *
 * @param mc Metacognition engine
 * @param step_type Step type about to run
 * @param input Input content
 * @param input_len Input length
 * @param out_preemptive_hint Output preventive hint (caller frees)
 * @param out_hint_len Hint length
 * @return 1=pattern matched and hint given, 0=no match, -1=error
 */
AIRY_API int airy_mc_preemptive_check(airy_metacognition_t *mc, tc_step_type_t step_type,
                                      const char *input, size_t input_len,
                                      char **out_preemptive_hint, size_t *out_hint_len);

/**
 * @brief Record the strategy execution result (for learning).
 *
 * Called after each correction operation; records which strategy works
 * for which pattern.
 *
 * @param mc Metacognition engine
 * @param pattern_key Pattern key name
 * @param strategy Strategy used
 * @param success Whether successful (1=success)
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_mc_record_strategy_result(airy_metacognition_t *mc,
                                                   const char *pattern_key,
                                                   mc_correction_strategy_t strategy, int success);

/**
 * @brief Adaptively adjust the acceptance threshold.
 *
 * Dynamically adjusts acceptance_threshold based on consecutive
 * pass/reject history. Repeated passes → loosen the threshold (efficiency);
 * repeated rejects → tighten it (quality).
 *
 * @param mc Metacognition engine
 * @return The adjusted current threshold
 */
AIRY_API float airy_mc_adapt_threshold(airy_metacognition_t *mc);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_METACOGNITION_H */
