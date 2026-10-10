// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_step.c
 * @brief Thinking-chain Thinking Step domain: reasoning-step lifecycle and dependency chain.
 */

#include "tc_internal.h"

/* ============================================================================
 * Thinking Step implementation
 * ============================================================================ */

airy_err_t airy_tc_step_create(airy_thinking_chain_t *chain, tc_step_type_t type, const char *input,
                               size_t input_len, const uint32_t *depends_on, size_t depends_count,
                               airy_thinking_step_t **out_step)
{

    if (!chain || !out_step) {
        AIRY_LOG_ERROR("airy_tc_step_create: NULL params (chain=%p out_step=%p)", (void *)chain,
                       (void *)out_step);
        return AIRY_EINVAL;
    }

    if (chain->step_count >= TC_MAX_THINKING_STEPS) {
        AIRY_LOG_ERROR(
            "airy_tc_step_create: max thinking steps exceeded (step_count=%zu max=%d type=%d)",
            chain->step_count, TC_MAX_THINKING_STEPS, (int)type);
        return AIRY_ERANGE;
    }

    if (chain->step_count >= chain->step_capacity) {
        size_t new_cap = chain->step_capacity * 2;
        airy_thinking_step_t **new_steps =
            (airy_thinking_step_t **)AIRY_REALLOC(chain->steps,
                                                  new_cap * sizeof(airy_thinking_step_t *));
        if (!new_steps) {
            AIRY_LOG_ERROR("airy_tc_step_create: steps REALLOC failed (new_cap=%zu)", new_cap);
            return AIRY_ENOMEM;
        }
        chain->steps = new_steps;
        chain->step_capacity = new_cap;
    }

    airy_thinking_step_t *step =
        (airy_thinking_step_t *)AIRY_CALLOC(1, sizeof(airy_thinking_step_t));
    if (!step) {
        AIRY_LOG_ERROR("airy_tc_step_create: step allocation failed (step_count=%zu)",
                       chain->step_count);
        return AIRY_ENOMEM;
    }
    chain->steps[chain->step_count] = step;

    step->step_id = chain->next_step_id++;
    step->type = type;
    step->status = TC_STATUS_PENDING;
    step->start_time_ns = 0;
    step->end_time_ns = 0;
    step->confidence = 0.0f;
    step->correction_count = 0;
    step->verify_result = TC_VERIFY_ACCEPT;

    if (input && input_len > 0) {
        step->raw_input = (char *)AIRY_MALLOC(input_len + 1);
        if (step->raw_input) {
            __builtin_memcpy(step->raw_input, input, input_len);
            step->raw_input[input_len] = '\0';
            step->raw_input_len = input_len;
        }
    }

    if (depends_on && depends_count > 0) {
        SAFE_MALLOC_ARRAY(step->depends_on, depends_count, sizeof(uint32_t));
        if (step->depends_on) {
            __builtin_memcpy(step->depends_on, depends_on, depends_count * sizeof(uint32_t));
            step->depends_count = depends_count;
        }
    }

    chain->step_count++;
    if (chain->ctx_window)
        chain->ctx_window->total_steps++;

    *out_step = step;
    return AIRY_SUCCESS;
}

airy_err_t airy_tc_step_complete(airy_thinking_step_t *step, const char *content,
                                 size_t content_len, float confidence, const char *role)
{

    if (!step || !content || content_len == 0) {
        AIRY_LOG_ERROR(
            "airy_tc_step_complete: NULL/invalid params (step=%p content=%p content_len=%zu)",
            (void *)step, (void *)content, content_len);
        return AIRY_EINVAL;
    }

    if (step->status == TC_STATUS_COMPLETED || step->status == TC_STATUS_CORRECTED) {
        AIRY_LOG_WARN("airy_tc_step_complete: state transition error, step already in terminal "
                      "state (step_id=%u status=%d)",
                      step->step_id, (int)step->status);
    }

    step->content = (char *)AIRY_MALLOC(content_len + 1);
    if (!step->content) {
        AIRY_LOG_ERROR(
            "airy_tc_step_complete: content allocation failed (step_id=%u content_len=%zu)",
            step->step_id, content_len);
        return AIRY_ENOMEM;
    }
    __builtin_memcpy(step->content, content, content_len);
    step->content[content_len] = '\0';
    step->content_len = content_len;

    step->confidence = (confidence >= 0.0f && confidence <= 1.0f) ? confidence : 0.5f;
    step->status = TC_STATUS_COMPLETED;
    step->end_time_ns = tc_time_now_ns();

    if (role)
        step->role = AIRY_STRDUP(role);

    return AIRY_SUCCESS;
}

airy_err_t airy_tc_step_verify(airy_thinking_step_t *step, int *is_valid, const char *critique,
                               size_t critique_len)
{

    if (!step || !is_valid) {
        AIRY_LOG_ERROR("airy_tc_step_verify: NULL params (step=%p is_valid=%p)", (void *)step,
                       (void *)is_valid);
        return AIRY_EINVAL;
    }

    if (critique && critique_len > 0) {
        step->critique = (char *)AIRY_MALLOC(critique_len + 1);
        if (step->critique) {
            __builtin_memcpy(step->critique, critique, critique_len);
            step->critique[critique_len] = '\0';
            step->critique_len = critique_len;
        }
    }

    *is_valid =
        (step->verify_result == TC_VERIFY_ACCEPT || step->verify_result == TC_VERIFY_MINOR_FIX) ?
            1 :
            0;
    return AIRY_SUCCESS;
}

airy_err_t airy_tc_step_correct(airy_thinking_step_t *step, const char *corrected_content,
                                size_t corrected_len)
{

    if (!step || !corrected_content || corrected_len == 0) {
        AIRY_LOG_ERROR("airy_tc_step_correct: NULL/invalid params (step=%p corrected_content=%p "
                       "corrected_len=%zu)",
                       (void *)step, (void *)corrected_content, corrected_len);
        return AIRY_EINVAL;
    }
    if (step->status == TC_STATUS_PENDING) {
        AIRY_LOG_WARN(
            "airy_tc_step_correct: state transition error, correcting a PENDING step (step_id=%u)",
            step->step_id);
    }
    if (step->correction_count >= (step->chain_ref ?
                                       step->chain_ref->ctx_window->max_corrections_per_chunk :
                                       TC_MAX_CORRECTIONS_DEFAULT)) {
        AIRY_LOG_WARN(
            "airy_tc_step_correct: max corrections exceeded (step_id=%u correction_count=%d)",
            step->step_id, step->correction_count);
        step->status = TC_STATUS_SKIPPED;
        return AIRY_ERANGE;
    }

    char **new_history =
        (char **)AIRY_REALLOC(step->correction_history,
                              (step->correction_history_count + 1) * sizeof(char *));
    if (!new_history && step->correction_history_count > 0) {
        AIRY_LOG_ERROR(
            "airy_tc_step_correct: correction_history REALLOC failed (step_id=%u count=%zu)",
            step->step_id, step->correction_history_count);
        return AIRY_ENOMEM;
    }
    step->correction_history = new_history;

    if (step->content) {
        step->correction_history[step->correction_history_count++] = step->content;
    }

    step->content = (char *)AIRY_MALLOC(corrected_len + 1);
    if (!step->content) {
        AIRY_LOG_ERROR(
            "airy_tc_step_correct: content allocation failed (step_id=%u corrected_len=%zu)",
            step->step_id, corrected_len);
        return AIRY_ENOMEM;
    }
    __builtin_memcpy(step->content, corrected_content, corrected_len);
    step->content[corrected_len] = '\0';
    step->content_len = corrected_len;
    step->correction_count++;
    step->status = TC_STATUS_CORRECTED;

    if (step->chain_ref && step->chain_ref->ctx_window) {
        step->chain_ref->ctx_window->total_corrections++;
    }

    return AIRY_SUCCESS;
}

int airy_tc_step_is_ready(const airy_thinking_step_t *step, const airy_thinking_chain_t *chain)
{

    if (!step || !chain) {
        AIRY_LOG_ERROR("airy_tc_step_is_ready: NULL params (step=%p chain=%p)", (void *)step,
                       (void *)chain);
        return AIRY_EINVAL;
    }

    for (size_t d = 0; d < step->depends_count; d++) {
        uint32_t dep_id = step->depends_on[d];
        int found_completed = 0;
        for (size_t s = 0; s < chain->step_count; s++) {
            if (chain->steps[s] && chain->steps[s]->step_id == dep_id &&
                (chain->steps[s]->status == TC_STATUS_COMPLETED ||
                 chain->steps[s]->status == TC_STATUS_CORRECTED)) {
                found_completed = 1;
                break;
            }
        }
        if (!found_completed)
            return 0;
    }
    return 1;
}
