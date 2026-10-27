// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file mc_learn.c
 * @brief 元认知持久学习域：错误模式检测/最优策略学习/预纠正/阈值自适应
 */

#include "metacognition_internal.h"

static const char *step_type_name(tc_step_type_t t)
{
    static const char *names[] = {"decomposition", "planning", "generation", "verification",
                                  "correction",    "audit",    "alignment"};
    int idx = (int)t;
    return (idx >= 0 && idx < 7) ? names[idx] : "unknown";
}

static const char *dim_short_name(mc_dimension_t d)
{
    static const char *names[] = {"rel", "acc", "cmp", "con", "clr"};
    return names[(int)d < MC_DIM_COUNT ? (int)d : 0];
}

size_t mc_pattern_lookup(const airy_metacognition_t *mc, const char *key)
{
    if (!mc || !key)
        return MC_MAX_PATTERNS;

    for (size_t i = 0; i < mc->pattern_count; i++) {
        if (strncmp(mc->patterns[i].pattern_key, key, 127) == 0)
            return i;
    }
    return MC_MAX_PATTERNS;
}

void mc_pattern_key(tc_step_type_t type, const mc_evaluation_result_t *result, char *out,
                    size_t cap)
{
    if (!result || !out || cap == 0)
        return;

    float worst_score = 1.0f;
    mc_dimension_t worst_dim = MC_DIM_RELEVANCE;
    for (int d = 0; d < MC_DIM_COUNT; d++) {
        if (result->dimensions[d].score < worst_score) {
            worst_score = result->dimensions[d].score;
            worst_dim = (mc_dimension_t)d;
        }
    }
    snprintf(out, cap, "%s_%s_%.1f", step_type_name(type), dim_short_name(worst_dim), worst_score);
}

static size_t find_or_create_pattern(airy_metacognition_t *mc, const char *key)
{
    size_t found = mc_pattern_lookup(mc, key);
    if (found < MC_MAX_PATTERNS)
        return found;
    if (mc->pattern_count >= MC_MAX_PATTERNS)
        return MC_MAX_PATTERNS;
    size_t idx = mc->pattern_count++;
    __builtin_memset(&mc->patterns[idx], 0, sizeof(mc_error_pattern_t));
    snprintf(mc->patterns[idx].pattern_key, sizeof(mc->patterns[idx].pattern_key), "%s", key);
    return idx;
}

airy_err_t airy_mc_detect_patterns(airy_metacognition_t *mc, mc_error_pattern_t **out_patterns,
                                   size_t *out_count)
{
    if (!mc) {
        AIRY_LOG_ERROR("airy_mc_detect_patterns: NULL params (mc=%p out_patterns=%p out_count=%p)",
                       (void *)mc, (void *)out_patterns, (void *)out_count);
        return AIRY_EINVAL;
    }

    mc_error_pattern_t *local_patterns = NULL;
    size_t local_count = 0;
    if (!out_patterns)
        out_patterns = &local_patterns;
    if (!out_count)
        out_count = &local_count;
    if (mc->record_count < 3) {
        *out_patterns = NULL;
        *out_count = 0;
        return AIRY_SUCCESS;
    }

    typedef struct {
        char key[128];
        uint64_t total;
        uint64_t fail;
    } pattern_acc_t;
    pattern_acc_t acc[MC_MAX_PATTERNS] = {{{0}, 0, 0}};
    size_t acc_count = 0;

    size_t check_n = (mc->record_count > 20) ? 20 : mc->record_count;
    for (size_t i = 0; i < check_n; i++) {
        size_t idx = (mc->record_head - 1 - i + mc->record_capacity) % mc->record_capacity;
        if (idx >= mc->record_count)
            continue;
        mc_evaluation_record_t *rec = &mc->records[idx];

        char pkey[128];
        mc_pattern_key(rec->type, &rec->result, pkey, sizeof(pkey));

        size_t aidx = acc_count;
        for (size_t j = 0; j < acc_count; j++) {
            if (strncmp(acc[j].key, pkey, 127) == 0) {
                aidx = j;
                break;
            }
        }
        if (aidx == acc_count && acc_count < MC_MAX_PATTERNS) {
            snprintf(acc[aidx].key, sizeof(acc[aidx].key), "%s", pkey);
            acc_count++;
        }
        if (aidx < MC_MAX_PATTERNS) {
            acc[aidx].total++;
            if (rec->result.strategy != MC_CORRECT_NONE)
                acc[aidx].fail++;
        }
    }

    /* 检测器只负责「建档 / 刷新活跃度」：新建模式以当前窗口为种子，
     * 既有模式不再重复累计同一批历史记录，计数增量由 feedback 回填维护。 */
    for (size_t i = 0; i < acc_count; i++) {
        if (acc[i].total < 2 || acc[i].fail < acc[i].total / 3)
            continue;

        size_t pidx = mc_pattern_lookup(mc, acc[i].key);
        if (pidx >= MC_MAX_PATTERNS) {
            pidx = find_or_create_pattern(mc, acc[i].key);
            if (pidx >= MC_MAX_PATTERNS)
                continue;
            mc->patterns[pidx].occurrence_count = acc[i].total;
            mc->patterns[pidx].failure_count = acc[i].fail;
            mc->patterns_detected++;
        }

        mc_error_pattern_t *pat = &mc->patterns[pidx];
        pat->failure_rate = (float)pat->failure_count /
                            (float)(pat->occurrence_count > 0 ? pat->occurrence_count : 1);
        pat->last_seen_ns = mc_time_now();
        pat->is_active = 1;
    }

    *out_patterns = (mc->pattern_count > 0) ? mc->patterns : NULL;
    *out_count = mc->pattern_count;
    return AIRY_SUCCESS;
}

airy_err_t airy_mc_learn_best_strategy(airy_metacognition_t *mc, const char *pattern_key,
                                       mc_correction_strategy_t *out_strategy)
{
    if (!mc || !pattern_key || !out_strategy) {
        AIRY_LOG_ERROR(
            "airy_mc_learn_best_strategy: NULL params (mc=%p pattern_key=%p out_strategy=%p)",
            (void *)mc, (void *)pattern_key, (void *)out_strategy);
        return AIRY_EINVAL;
    }

    *out_strategy = MC_CORRECT_NONE;

    size_t pidx = mc_pattern_lookup(mc, pattern_key);
    if (pidx >= MC_MAX_PATTERNS)
        return AIRY_ENOENT;

    /* 样本不足时不下发建议：检测窗口种子不等于真实运行证据。 */
    if (mc->patterns[pidx].occurrence_count < 5)
        return AIRY_SUCCESS;

    if (mc->patterns[pidx].strategy_success_rate > 0.3f &&
        mc->patterns[pidx].best_strategy != MC_CORRECT_NONE) {
        *out_strategy = mc->patterns[pidx].best_strategy;
        return AIRY_SUCCESS;
    }

    if (mc->patterns[pidx].failure_rate > 0.8f) {
        *out_strategy = MC_CORRECT_ESCALATE;
    } else if (mc->patterns[pidx].failure_rate > 0.5f) {
        *out_strategy = MC_CORRECT_RERUN;
    } else {
        *out_strategy = MC_CORRECT_AUTO;
    }

    return AIRY_SUCCESS;
}

airy_err_t mc_learn_from_feedback(airy_metacognition_t *mc, int was_correct)
{
    if (!mc || !mc->enable_learning || mc->record_capacity == 0 || mc->record_count == 0)
        return AIRY_SUCCESS;

    size_t idx = (mc->record_head - 1 + mc->record_capacity) % mc->record_capacity;
    const mc_evaluation_record_t *rec = &mc->records[idx];
    if (rec->result.strategy == MC_CORRECT_NONE)
        return AIRY_SUCCESS;

    char key[128];
    mc_pattern_key(rec->type, &rec->result, key, sizeof(key));
    return airy_mc_record_strategy_result(mc, key, rec->result.strategy, was_correct);
}

int airy_mc_preemptive_check(airy_metacognition_t *mc, tc_step_type_t step_type, const char *input,
                             size_t input_len, char **out_preemptive_hint, size_t *out_hint_len)
{
    if (!mc || !input || !out_preemptive_hint || !out_hint_len) {
        AIRY_LOG_ERROR("airy_mc_preemptive_check: NULL params (mc=%p input=%p "
                       "out_preemptive_hint=%p out_hint_len=%p)",
                       (void *)mc, (void *)input, (void *)out_preemptive_hint,
                       (void *)out_hint_len);
        return AIRY_EINVAL;
    }
    *out_preemptive_hint = NULL;
    *out_hint_len = 0;

    if (mc->pattern_count == 0 || !mc->enable_learning)
        return 0;

    for (size_t i = 0; i < mc->pattern_count; i++) {
        mc_error_pattern_t *pat = &mc->patterns[i];
        if (!pat->is_active || pat->failure_rate < 0.4f)
            continue;

        char type_prefix[32];
        snprintf(type_prefix, sizeof(type_prefix), "%s_", step_type_name(step_type));

        if (strstr(pat->pattern_key, type_prefix) == NULL)
            continue;

        size_t hlen = 384 + strlen(pat->pattern_key) + 64;
        char *hint = (char *)AIRY_MALLOC(hlen);
        if (!hint)
            return AIRY_EINVAL;

        int written =
            snprintf(hint, hlen,
                     "[PREEMPTIVE GUIDANCE] Detected known failure pattern '%s' "
                     "(failure_rate=%.0f%%, occurrences=%llu). "
                     "Precautionary instructions:\n"
                     "- Pay extra attention to %s\n"
                     "- Verify your output against the original request before finalizing\n"
                     "- If unsure about any fact, explicitly state uncertainty\n"
                     "- Structure your response clearly with numbered points",
                     pat->pattern_key, pat->failure_rate * 100.0f,
                     (unsigned long long)pat->occurrence_count, pat->pattern_key);

        if (written <= 0 || (size_t)written >= hlen) {
            AIRY_FREE(hint);
            return AIRY_EINVAL;
        }

        *out_preemptive_hint = hint;
        *out_hint_len = (size_t)written;
        mc->preemptive_corrections++;

        AIRY_LOG_INFO("MC preemptive: matched pattern '%s' (rate=%.2f)", pat->pattern_key,
                      pat->failure_rate);
        return 1;
    }

    return 0;
}

airy_err_t airy_mc_record_strategy_result(airy_metacognition_t *mc, const char *pattern_key,
                                          mc_correction_strategy_t strategy, int success)
{
    if (!mc || !pattern_key) {
        AIRY_LOG_ERROR("airy_mc_record_strategy_result: NULL params (mc=%p pattern_key=%p)",
                       (void *)mc, (void *)pattern_key);
        return AIRY_EINVAL;
    }

    size_t pidx = find_or_create_pattern(mc, pattern_key);
    if (pidx >= MC_MAX_PATTERNS) {
        AIRY_LOG_ERROR("airy_mc_record_strategy_result: pattern storage full, cannot record "
                       "(pattern_key=%s pattern_count=%zu)",
                       pattern_key, mc->pattern_count);
        return AIRY_ENOMEM;
    }

    mc_error_pattern_t *pat = &mc->patterns[pidx];
    pat->occurrence_count++;
    pat->last_seen_ns = mc_time_now();

    if (!success)
        pat->failure_count++;
    /* 失败率始终由计数派生：成功样本同样改变分母，只改分子会留下陈旧值。 */
    pat->failure_rate = (float)pat->failure_count / (float)pat->occurrence_count;

    float alpha = 0.3f;
    if (success) {
        pat->strategy_success_rate = alpha * 1.0f + (1.0f - alpha) * pat->strategy_success_rate;
        pat->best_strategy = strategy;
    } else {
        pat->strategy_success_rate = alpha * 0.0f + (1.0f - alpha) * pat->strategy_success_rate;
    }

    if (pat->occurrence_count > 5) {
        mc->learning_effectiveness = 1.0f - pat->failure_rate;
    }

    return AIRY_SUCCESS;
}

float airy_mc_adapt_threshold(airy_metacognition_t *mc)
{
    if (!mc) {
        AIRY_LOG_WARN("airy_mc_adapt_threshold: NULL mc parameter, returning default threshold");
        return 0.7f;
    }

    if (mc->adaptive_acceptance_threshold <= 0.0f)
        mc->adaptive_acceptance_threshold = mc->acceptance_threshold;

    if (mc->consecutive_accepts >= 5) {
        mc->adaptive_acceptance_threshold -= 0.02f;
        mc->consecutive_accepts = 0;
        if (mc->adaptive_acceptance_threshold < 0.55f)
            mc->adaptive_acceptance_threshold = 0.55f;
    }

    if (mc->consecutive_rejects >= 3) {
        mc->adaptive_acceptance_threshold += 0.03f;
        mc->consecutive_rejects = 0;
        if (mc->adaptive_acceptance_threshold > 0.90f)
            mc->adaptive_acceptance_threshold = 0.90f;
    }

    return mc->adaptive_acceptance_threshold;
}
