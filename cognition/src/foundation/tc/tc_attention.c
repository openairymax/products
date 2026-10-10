// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_attention.c
 * @brief 思考链路注意力分配域：动态预算与优先级计算
 */

#include "tc_internal.h"

static float get_step_weight(tc_step_type_t type, const tc_attention_weights_t *w)
{
    switch (type) {
    case TC_STEP_DECOMPOSITION:
        return w->decomposition_weight;
    case TC_STEP_PLANNING:
        return w->planning_weight;
    case TC_STEP_GENERATION:
        return w->generation_weight;
    case TC_STEP_VERIFICATION:
        return w->verification_weight;
    case TC_STEP_AUDIT:
        return w->audit_weight;
    case TC_STEP_ALIGNMENT:
        return w->alignment_weight;
    default:
        return 0.10f;
    }
}

airy_err_t airy_tc_set_attention_config(airy_thinking_chain_t *chain,
                                        const tc_attention_config_t *config)
{
    if (!chain || !config) {
        AIRY_LOG_ERROR("airy_tc_set_attention_config: NULL params (chain=%p config=%p)",
                       (void *)chain, (void *)config);
        return AIRY_EINVAL;
    }
    chain->attention_config = *config;
    chain->attention_configured = 1;
    return AIRY_SUCCESS;
}

airy_err_t airy_tc_allocate_attention(airy_thinking_chain_t *chain, airy_thinking_step_t *step,
                                      tc_allocation_result_t *out_alloc)
{
    if (!chain || !step || !out_alloc) {
        AIRY_LOG_ERROR("airy_tc_allocate_attention: NULL params (chain=%p step=%p out_alloc=%p)",
                       (void *)chain, (void *)step, (void *)out_alloc);
        return AIRY_EINVAL;
    }

    tc_attention_config_t cfg = chain->attention_configured ?
                                    chain->attention_config :
                                    (tc_attention_config_t)TC_ATTENTION_DEFAULTS;

    float weight = get_step_weight(step->type, &cfg.weights);
    size_t base_alloc = (size_t)((float)cfg.base_tokens * weight);

    float priority = airy_tc_compute_priority(step, chain);

    float urgency = 0.0f;
    if (step->status == TC_STATUS_PENDING && step->dependents_count > 0)
        urgency = 0.3f + 0.7f * ((float)step->dependents_count / 10.0f);
    if (urgency > 1.0f)
        urgency = 1.0f;

    float dynamic_factor = 1.0f + (priority * 0.5f) + (urgency * 0.3f);
    size_t final_alloc = (size_t)((float)base_alloc * dynamic_factor);

    if (final_alloc < cfg.min_step_tokens)
        final_alloc = cfg.min_step_tokens;
    if (final_alloc > cfg.max_step_tokens)
        final_alloc = cfg.max_step_tokens;

    size_t avail = 0;
    if (chain->ctx_window && chain->ctx_window->max_tokens > chain->ctx_window->used_tokens)
        avail = chain->ctx_window->max_tokens - chain->ctx_window->used_tokens;
    if (avail > 0 && final_alloc > avail)
        final_alloc = avail;

    out_alloc->allocated_tokens = final_alloc;
    out_alloc->priority_score = priority;
    out_alloc->urgency_score = urgency;
    out_alloc->is_elevated = (priority > 0.8f || urgency > 0.7f) ? 1 : 0;

    return AIRY_SUCCESS;
}

airy_err_t airy_tc_adjust_dynamic_budget(airy_thinking_chain_t *chain, tc_step_type_t step_type,
                                         float performance_score)
{
    if (!chain || performance_score < 0.0f || performance_score > 1.0f) {
        AIRY_LOG_ERROR(
            "airy_tc_adjust_dynamic_budget: invalid params (chain=%p performance_score=%.2f)",
            (void *)chain, performance_score);
        return AIRY_EINVAL;
    }
    if (!chain->attention_config.enable_dynamic_adjustment)
        return AIRY_SUCCESS;

    float *target_weight = NULL;
    tc_attention_weights_t *w = &chain->attention_config.weights;
    switch (step_type) {
    case TC_STEP_DECOMPOSITION:
        target_weight = &w->decomposition_weight;
        break;
    case TC_STEP_PLANNING:
        target_weight = &w->planning_weight;
        break;
    case TC_STEP_GENERATION:
        target_weight = &w->generation_weight;
        break;
    case TC_STEP_VERIFICATION:
        target_weight = &w->verification_weight;
        break;
    case TC_STEP_AUDIT:
        target_weight = &w->audit_weight;
        break;
    case TC_STEP_ALIGNMENT:
        target_weight = &w->alignment_weight;
        break;
    default:
        AIRY_LOG_ERROR("airy_tc_adjust_dynamic_budget: unknown step type (step_type=%d)",
                       (int)step_type);
        return AIRY_EINVAL;
    }

    float adjustment = (performance_score - 0.6f) * 0.05f;
    float new_weight = *target_weight + adjustment;
    if (new_weight < 0.03f)
        new_weight = 0.03f;
    if (new_weight > 0.50f)
        new_weight = 0.50f;

    float total = w->decomposition_weight + w->planning_weight + w->generation_weight +
                  w->verification_weight + w->audit_weight + w->alignment_weight;
    total += (new_weight - *target_weight);
    if (total > 0.01f) {
        float scale = 1.0f / total;
        w->decomposition_weight *= scale;
        w->planning_weight *= scale;
        w->generation_weight *= scale;
        w->verification_weight *= scale;
        w->audit_weight *= scale;
        w->alignment_weight *= scale;
        *target_weight = new_weight * scale;
    }

    return AIRY_SUCCESS;
}

float airy_tc_compute_priority(const airy_thinking_step_t *step, const airy_thinking_chain_t *chain)
{
    if (!step)
        return 0.0f;

    float dep_factor = (step->dependents_count > 0) ?
                           0.2f + 0.4f * fminf((float)step->dependents_count / 5.0f, 1.0f) :
                           0.1f;

    float type_factor = 0.15f;
    switch (step->type) {
    case TC_STEP_DECOMPOSITION:
        type_factor = 0.70f;
        break;
    case TC_STEP_PLANNING:
        type_factor = 0.65f;
        break;
    case TC_STEP_GENERATION:
        type_factor = 0.80f;
        break;
    case TC_STEP_VERIFICATION:
        type_factor = 0.55f;
        break;
    case TC_STEP_AUDIT:
        type_factor = 0.50f;
        break;
    case TC_STEP_ALIGNMENT:
        type_factor = 0.45f;
        break;
    case TC_STEP_CORRECTION:
        type_factor = 0.60f;
        break;
    }

    float conf_factor = (step->confidence > 0.0f) ? 1.0f - step->confidence : 0.5f;

    float status_factor = 0.3f;
    switch (step->status) {
    case TC_STATUS_PENDING:
        status_factor = 0.90f;
        break;
    case TC_STATUS_EXECUTING:
        status_factor = 0.95f;
        break;
    case TC_STATUS_FAILED:
        status_factor = 1.00f;
        break;
    default:
        status_factor = 0.20f;
        break;
    }

    float priority =
        dep_factor * 0.25f + type_factor * 0.35f + conf_factor * 0.20f + status_factor * 0.20f;
    if (priority > 1.0f)
        priority = 1.0f;
    if (priority < 0.0f)
        priority = 0.0f;
    return priority;
}
