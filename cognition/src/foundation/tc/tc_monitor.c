// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_monitor.c
 * @brief Thinking-chain execution-monitoring domain: anomaly detection and chain health check.
 */

#include "tc_internal.h"

/* ============================================================================
 * Execution-monitoring implementation
 * ============================================================================ */

static float compute_repetition_score(const char *content, size_t len)
{
    if (!content || len < 20)
        return 0.0f;

    size_t window = len / 2;
    if (window < 10)
        window = 10;
    if (window > len)
        window = len;

    int matching_bigrams = 0;
    int total_bigrams = 0;

    for (size_t i = 1; i + window <= len; i++) {
        total_bigrams++;
        if (i + window * 2 <= len && memcmp(content + i, content + i + window, window) == 0) {
            matching_bigrams++;
        }
    }

    if (total_bigrams == 0)
        return 0.0f;
    return (float)matching_bigrams / (float)total_bigrams;
}

airy_err_t airy_tc_step_monitor(const airy_thinking_step_t *step, const tc_monitor_config_t *config,
                                tc_monitor_result_t *out_result)
{
    if (!step || !out_result) {
        AIRY_LOG_ERROR("airy_tc_step_monitor: NULL params (step=%p out_result=%p)", (void *)step,
                       (void *)out_result);
        return AIRY_EINVAL;
    }

    tc_monitor_config_t defaults = TC_MONITOR_DEFAULTS;
    if (!config)
        config = &defaults;

    __builtin_memset(out_result, 0, sizeof(tc_monitor_result_t));
    out_result->anomaly = TC_ANOMALY_NONE;
    out_result->is_critical = 0;
    out_result->severity_score = 0.0f;

    if (step->status == TC_STATUS_EXECUTING && step->start_time_ns > 0) {
        uint64_t elapsed_ms = (tc_time_now_ns() - step->start_time_ns) / 1000000ULL;
        if (elapsed_ms > config->default_timeout_ms) {
            AIRY_LOG_ERROR(
                "airy_tc_step_monitor: timeout detected (step_id=%u elapsed=%llums limit=%ums)",
                step->step_id, (unsigned long long)elapsed_ms, config->default_timeout_ms);
            out_result->anomaly = TC_ANOMALY_TIMEOUT;
            out_result->is_critical = 1;
            out_result->severity_score = 0.95f;
            char desc[128];
            int dlen =
                snprintf(desc, sizeof(desc), "Step#%u timed out: %llums > %ums limit",
                         step->step_id, (unsigned long long)elapsed_ms, config->default_timeout_ms);
            out_result->description = (char *)AIRY_MALLOC(dlen + 1);
            if (out_result->description) {
                __builtin_memcpy(out_result->description, desc, dlen + 1);
                out_result->description_len = (size_t)dlen;
            }
            return AIRY_SUCCESS;
        }
    }

    if (step->content_len == 0 || !step->content) {
        AIRY_LOG_ERROR("airy_tc_step_monitor: empty output detected (step_id=%u type=%d)",
                       step->step_id, (int)step->type);
        out_result->anomaly = TC_ANOMALY_EMPTY_OUTPUT;
        out_result->severity_score = 0.8f;
        out_result->is_critical = 1;
        const char *desc = "Empty output detected";
        out_result->description = AIRY_STRDUP(desc);
        out_result->description_len = strlen(desc);
        return AIRY_SUCCESS;
    }

    if (step->content_len < config->min_output_chars && step->type != TC_STEP_VERIFICATION) {
        out_result->anomaly = TC_ANOMALY_TRUNCATED_OUTPUT;
        out_result->severity_score = 0.5f;
        char desc[96];
        int dlen = snprintf(desc, sizeof(desc), "Output too short: %zu chars < %zu minimum",
                            step->content_len, config->min_output_chars);
        out_result->description = (char *)AIRY_MALLOC(dlen + 1);
        if (out_result->description) {
            __builtin_memcpy(out_result->description, desc, dlen + 1);
            out_result->description_len = (size_t)dlen;
        }
    }

    if (step->content_len > config->max_output_chars) {
        float prev_sev = out_result->severity_score;
        out_result->anomaly = TC_ANOMALY_EXCESSIVE_OUTPUT;
        out_result->severity_score = (prev_sev > 0.6f) ? prev_sev : 0.4f;
        if (!out_result->description) {
            char desc[96];
            int dlen = snprintf(desc, sizeof(desc), "Output excessive: %zu chars > %zu maximum",
                                step->content_len, config->max_output_chars);
            out_result->description = (char *)AIRY_MALLOC(dlen + 1);
            if (out_result->description) {
                __builtin_memcpy(out_result->description, desc, dlen + 1);
                out_result->description_len = (size_t)dlen;
            }
        }
    }

    float rep_score = compute_repetition_score(step->content, step->content_len);
    if (rep_score > config->repetition_threshold) {
        float prev_sev = out_result->severity_score;
        if (prev_sev < 0.5f) {
            out_result->anomaly = TC_ANOMALY_REPETITIVE_CONTENT;
            out_result->severity_score = rep_score;
        } else {
            out_result->severity_score = (prev_sev + rep_score) / 2.0f;
        }
        if (!out_result->description) {
            char desc[96];
            int dlen =
                snprintf(desc, sizeof(desc), "Repetitive content detected (score=%.2f)", rep_score);
            out_result->description = (char *)AIRY_MALLOC(dlen + 1);
            if (out_result->description) {
                __builtin_memcpy(out_result->description, desc, dlen + 1);
                out_result->description_len = (size_t)dlen;
            }
        }
    }

    if (config->enable_quality_gate) {
        int quality_ok = (step->confidence >= config->quality_gate_threshold) ||
                         (step->status == TC_STATUS_COMPLETED && step->correction_count == 0);

        if (!quality_ok && step->confidence < config->quality_gate_threshold) {
            out_result->anomaly = TC_ANOMALY_CONFIDENCE_DROP;
            float prev_sev = out_result->severity_score;
            out_result->severity_score = (prev_sev > 0.3f) ? prev_sev : 0.35f;
            out_result->is_critical = (step->confidence < 0.15f) ? 1 : 0;
            if (!out_result->description) {
                char desc[96];
                int dlen = snprintf(desc, sizeof(desc), "Low confidence %.2f below threshold %.2f",
                                    step->confidence, config->quality_gate_threshold);
                out_result->description = (char *)AIRY_MALLOC(dlen + 1);
                if (out_result->description) {
                    __builtin_memcpy(out_result->description, desc, dlen + 1);
                    out_result->description_len = (size_t)dlen;
                }
            }
        }
    }

    return AIRY_SUCCESS;
}

airy_err_t airy_tc_chain_health_check(const airy_thinking_chain_t *chain, size_t *out_anomaly_count,
                                      int *out_has_critical)
{
    if (!chain || !out_anomaly_count || !out_has_critical) {
        AIRY_LOG_ERROR("airy_tc_chain_health_check: NULL params (chain=%p out_anomaly_count=%p "
                       "out_has_critical=%p)",
                       (void *)chain, (void *)out_anomaly_count, (void *)out_has_critical);
        return AIRY_EINVAL;
    }

    *out_anomaly_count = 0;
    *out_has_critical = 0;

    tc_monitor_config_t defaults = TC_MONITOR_DEFAULTS;

    for (size_t i = 0; i < chain->step_count; i++) {
        if (!chain->steps[i])
            continue;
        if (chain->steps[i]->status == TC_STATUS_PENDING)
            continue;

        tc_monitor_result_t mon;
        airy_err_t err = airy_tc_step_monitor(chain->steps[i], &defaults, &mon);
        if (err == AIRY_SUCCESS && mon.anomaly != TC_ANOMALY_NONE) {
            (*out_anomaly_count)++;
            if (mon.is_critical)
                *out_has_critical = 1;
            if (mon.description)
                AIRY_FREE(mon.description);
        }
    }

    return AIRY_SUCCESS;
}
