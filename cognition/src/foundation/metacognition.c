// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file metacognition.c
 * @brief Metacognition module core implementation — create/destroy/chain
 *        attach + audit records (stats/history/reset).
 *
 * Implements the core logic of the Thinkdual S1 verification role:
 * - 5-dimension evaluation (relevance/accuracy/completeness/consistency/clarity)
 * - Confidence calibration (historical bias tracking)
 * - Correction-strategy selection and execution
 * - Self-correction pattern detection
 */

#include "mc/metacognition_internal.h"

uint64_t mc_time_now(void)
{
    return airy_time_ns();
}

float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* ============================================================================
 * Create / destroy
 * ============================================================================ */

airy_err_t airy_mc_create(airy_metacognition_t **out_mc)
{
    if (!out_mc) {
        AIRY_LOG_ERROR("airy_mc_create: NULL out_mc parameter");
        return AIRY_EINVAL;
    }

    airy_metacognition_t *mc = (airy_metacognition_t *)AIRY_CALLOC(1, sizeof(airy_metacognition_t));
    if (!mc) {
        AIRY_LOG_ERROR("airy_mc_create: allocation failed for metacognition");
        return AIRY_ENOMEM;
    }

    mc->acceptance_threshold = 0.7f;
    mc->auto_correct_threshold = 0.5f;
    mc->enable_confidence_calibration = 1;
    mc->enable_learning = 1;

    mc->record_capacity = MC_MAX_HISTORY_RECORDS;
    mc->records =
        (mc_evaluation_record_t *)AIRY_CALLOC(mc->record_capacity, sizeof(mc_evaluation_record_t));
    if (!mc->records) {
        AIRY_LOG_ERROR("airy_mc_create: records allocation failed (capacity=%zu)",
                       mc->record_capacity);
        AIRY_FREE(mc);
        return AIRY_ENOMEM;
    }
    mc->record_count = 0;
    mc->record_head = 0;

    __builtin_memset(&mc->calibrator, 0, sizeof(mc_calibrator_t));

    mc->total_evaluations = 0;
    mc->total_corrections = 0;
    mc->total_rejections = 0;
    mc->total_auto_fixes = 0;
    mc->total_rerun_successes = 0;
    mc->chain = NULL;

    __builtin_memset(mc->patterns, 0, sizeof(mc->patterns));
    mc->pattern_count = 0;
    mc->adaptive_acceptance_threshold = mc->acceptance_threshold;
    mc->consecutive_accepts = 0;
    mc->consecutive_rejects = 0;
    mc->patterns_detected = 0;
    mc->preemptive_corrections = 0;
    mc->learning_effectiveness = 0.0f;

    *out_mc = mc;
    return AIRY_SUCCESS;
}

void airy_mc_destroy(airy_metacognition_t *mc)
{
    if (!mc)
        return;
    for (size_t i = 0; i < mc->record_count; i++) {
        if (mc->records[i].result.critique_text)
            AIRY_FREE((void *)mc->records[i].result.critique_text);
    }
    AIRY_FREE(mc->records);
    AIRY_FREE(mc);
}

void airy_mc_set_chain(airy_metacognition_t *mc, airy_thinking_chain_t *chain)
{
    if (!mc)
        return;
    mc->chain = chain;
}

/* ============================================================================
 * Stats and diagnostics
 * ============================================================================ */

airy_err_t airy_mc_stats(airy_metacognition_t *mc, char **out_json)
{
    if (!mc || !out_json) {
        AIRY_LOG_ERROR("airy_mc_stats: NULL params (mc=%p out_json=%p)", (void *)mc,
                       (void *)out_json);
        return AIRY_EINVAL;
    }

    char buf[1024];
    int written =
        snprintf(buf, sizeof(buf),
                 "{\"evaluations\":%llu,"
                 "\"corrections\":%llu,"
                 "\"rejections\":%llu,"
                 "\"auto_fixes\":%llu,"
                 "\"calibration\":{\"samples\":%zu,\"bias\":%.4f,"
                 "\"overconf_rate\":%.4f,\"underconf_rate\":%.4f},"
                 "\"records\":%zu}",
                 (unsigned long long)mc->total_evaluations,
                 (unsigned long long)mc->total_corrections,
                 (unsigned long long)mc->total_rejections, (unsigned long long)mc->total_auto_fixes,
                 mc->calibrator.calibration_count,
                 mc->calibrator.calibration_count > 0 ?
                     mc->calibrator.calibration_sum / (float)mc->calibrator.calibration_count :
                     0.0f,
                 mc->calibrator.overconfidence_rate, mc->calibrator.underconfidence_rate,
                 mc->record_count);

    if (written < 0 || (size_t)written >= sizeof(buf)) {
        AIRY_LOG_ERROR(
            "airy_mc_stats: stats JSON truncation or encoding error (written=%d buf_size=%zu)",
            written, sizeof(buf));
        return AIRY_ERANGE;
    }

    char *result = AIRY_STRDUP(buf);
    if (!result) {
        AIRY_LOG_ERROR("airy_mc_stats: result allocation failed");
        return AIRY_ENOMEM;
    }
    *out_json = result;
    return AIRY_SUCCESS;
}

airy_err_t airy_mc_get_history(airy_metacognition_t *mc, size_t count,
                               mc_evaluation_record_t **out_records, size_t *out_count)
{

    if (!mc || !out_records || !out_count) {
        AIRY_LOG_ERROR("airy_mc_get_history: NULL params (mc=%p out_records=%p out_count=%p)",
                       (void *)mc, (void *)out_records, (void *)out_count);
        return AIRY_EINVAL;
    }

    size_t avail = (count < mc->record_count) ? count : mc->record_count;
    *out_records =
        &mc->records[(mc->record_head - avail + mc->record_capacity) % mc->record_capacity];
    *out_count = avail;
    return AIRY_SUCCESS;
}

void airy_mc_reset(airy_metacognition_t *mc)
{
    if (!mc)
        return;
    for (size_t i = 0; i < mc->record_count; i++) {
        if (mc->records[i].result.critique_text) {
            AIRY_FREE((void *)mc->records[i].result.critique_text);
            mc->records[i].result.critique_text = NULL;
        }
    }
    mc->record_count = 0;
    mc->record_head = 0;
    __builtin_memset(&mc->calibrator, 0, sizeof(mc_calibrator_t));
    mc->total_evaluations = 0;
    mc->total_corrections = 0;
    mc->total_rejections = 0;
    mc->total_auto_fixes = 0;
    mc->total_rerun_successes = 0;

    __builtin_memset(mc->patterns, 0, sizeof(mc->patterns));
    mc->pattern_count = 0;
    mc->adaptive_acceptance_threshold = mc->acceptance_threshold;
    mc->consecutive_accepts = 0;
    mc->consecutive_rejects = 0;
    mc->patterns_detected = 0;
    mc->preemptive_corrections = 0;
    mc->learning_effectiveness = 0.0f;
}
