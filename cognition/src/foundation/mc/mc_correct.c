// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file mc_correct.c
 * @brief Metacognition correction-execution domain: strategy execution
 *        (AUTO/RERUN/ESCALATE) and self-correction judgment.
 */

#include "metacognition_internal.h"

/* ============================================================================
 * Correction execution
 * ============================================================================ */

airy_err_t airy_mc_correct(
    airy_metacognition_t *mc, airy_thinking_step_t *step, const mc_evaluation_result_t *eval,
    airy_err_t (*corrector_fn)(const char *, size_t, char **, size_t *, void *), void *user_data)
{

    if (!mc || !step || !eval) {
        AIRY_LOG_ERROR("airy_mc_correct: NULL params (mc=%p step=%p eval=%p)", (void *)mc,
                       (void *)step, (void *)eval);
        return AIRY_EINVAL;
    }

    switch (eval->strategy) {
    case MC_CORRECT_NONE:
        return AIRY_SUCCESS;

    case MC_CORRECT_AUTO: {
        if (!corrector_fn) {
            AIRY_LOG_ERROR("airy_mc_correct: AUTO correction requested but corrector_fn "
                           "is NULL (step_id=%u)",
                           step->step_id);
            return AIRY_EINVAL;
        }
        char *corrected = NULL;
        size_t corr_len = 0;
        airy_err_t err =
            corrector_fn(step->raw_input, step->raw_input_len, &corrected, &corr_len, user_data);
        if (err == AIRY_SUCCESS && corrected && corr_len > 0) {
            airy_tc_step_correct(step, corrected, corr_len);
            AIRY_FREE(corrected);
            mc->total_auto_fixes++;
            mc->total_corrections++;

            if (mc->chain && mc->chain->on_correction) {
                mc->chain->on_correction(step, eval->critique_text, mc->chain->callback_user_data);
            }
        }
        return err;
    }

    case MC_CORRECT_RERUN: {
        mc->total_corrections++;
        if (!corrector_fn) {
            AIRY_LOG_ERROR("airy_mc_correct: RERUN correction requested but corrector_fn "
                           "is NULL (step_id=%u)",
                           step->step_id);
            step->status = TC_STATUS_FAILED;
            mc->total_rejections++;
            return AIRY_EPERM;
        }

        const int max_retries = 3;
        airy_err_t last_err = AIRY_EPERM;
        for (int attempt = 0; attempt < max_retries; attempt++) {
            char *corrected = NULL;
            size_t corr_len = 0;

            char *enhanced_input = NULL;
            size_t enhanced_len = 0;
            if (attempt > 0 && eval->critique_text) {
                enhanced_len = step->raw_input_len + strlen(eval->critique_text) + 64;
                enhanced_input = (char *)AIRY_CALLOC(1, enhanced_len);
                if (enhanced_input) {
                    snprintf(enhanced_input, enhanced_len,
                             "[Original]\n%s\n[Critique #%d: %s]\n[Instruction: Improve based on "
                             "critique above]",
                             step->raw_input, attempt, eval->critique_text);
                }
            }
            const char *input_data = enhanced_input ? enhanced_input : step->raw_input;
            size_t input_len = enhanced_input ? strlen(enhanced_input) : step->raw_input_len;

            last_err = corrector_fn(input_data, input_len, &corrected, &corr_len, user_data);
            if (enhanced_input)
                AIRY_FREE(enhanced_input);

            if (last_err == AIRY_SUCCESS && corrected && corr_len > 0) {
                airy_tc_step_correct(step, corrected, corr_len);
                AIRY_FREE(corrected);
                if (mc->chain && mc->chain->on_correction) {
                    mc->chain->on_correction(step, eval->critique_text,
                                             mc->chain->callback_user_data);
                }
                mc->total_rerun_successes++;
                return AIRY_SUCCESS;
            }
            if (corrected)
                AIRY_FREE(corrected);

            if (attempt < max_retries - 1) {
                uint64_t backoff_ms = 1000ULL << (attempt > 20 ? 20 : attempt);
                airy_sleep_ms((uint32_t)backoff_ms);
            }
        }

        step->status = TC_STATUS_FAILED;
        mc->total_rejections++;
        AIRY_LOG_ERROR(
            "airy_mc_correct: RERUN all retries exhausted (step_id=%u last_err=%d)",
            step->step_id, (int)last_err);
        return last_err;
    }

    case MC_CORRECT_ESCALATE:
        mc->total_rejections++;
        step->status = TC_STATUS_FAILED;
        AIRY_LOG_ERROR(
            "MC_ESCALATE: step=%p strategy=ESCALATE overall=%.2f conf=%.2f acceptable=%d",
            (void *)step, eval ? eval->overall_score : 0.0f,
            eval ? eval->calibrated_confidence : 0.0f, eval ? eval->is_acceptable : 0);
        return AIRY_EPERM;

    default:
        AIRY_LOG_ERROR("airy_mc_correct: unknown correction strategy (strategy=%d)",
                       (int)eval->strategy);
        return AIRY_EINVAL;
    }
}

int airy_mc_should_self_correct(airy_metacognition_t *mc, tc_step_type_t step_type)
{
    if (!mc || mc->record_count < 3) {
        if (!mc)
            AIRY_LOG_WARN("airy_mc_should_self_correct: NULL mc parameter");
        return 0;
    }

    size_t recent_failures = 0;
    size_t check_count = (mc->record_count > 10) ? 10 : mc->record_count;

    for (size_t i = 0; i < check_count; i++) {
        size_t idx = (mc->record_head - 1 - i + mc->record_capacity) % mc->record_capacity;
        if (idx >= mc->record_count)
            break;
        if (mc->records[idx].result.strategy != MC_CORRECT_NONE)
            recent_failures++;
    }

    return (recent_failures >= check_count / 2) ? 1 : 0;
}
