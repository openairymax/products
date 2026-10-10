// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file mc_evaluate.c
 * @brief Metacognition evaluation domain: five-dimension scoring
 *        (relevance/accuracy/completeness/consistency/clarity) and the
 *        evaluation entry point.
 */

#include "metacognition_internal.h"

static const char *dim_name(mc_dimension_t d)
{
    static const char *names[] = {"relevance", "accuracy", "completeness", "consistency",
                                  "clarity"};
    return names[(int)d < MC_DIM_COUNT ? (int)d : 0];
}

/* 文本启发式必须大小写不敏感。评分关键词在自然语言里多为句首词，首字母
 * 必然大写（"First"、"According to"、"Computer"），若按字节精确比较会系统性
 * 漏配，把合格输出误判为低质并触发无谓的自动纠正。这里只做 ASCII 折叠：
 * 不依赖 locale 与 tolower，跨平台结果一致，非 ASCII 字节原样返回。 */
static unsigned char mc_fold(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c - 'A' + 'a');
    return c;
}

/* 大小写不敏感子串查找：找到返回首次出现的指针，否则返回 NULL。 */
static const char *mc_find(const char *hay, const char *needle)
{
    if (!hay || !needle || *needle == '\0')
        return NULL;

    size_t nlen = strlen(needle);
    for (const char *p = hay; *p != '\0'; p++) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               mc_fold((unsigned char)p[i]) == mc_fold((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return p;
    }
    return NULL;
}

/* 大小写不敏感的前 n 字节比较，用于词边界确认后的整词匹配。 */
static int mc_word_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (mc_fold((unsigned char)a[i]) != mc_fold((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* ============================================================================
 * Core evaluation logic
 * ============================================================================ */

static float score_relevance(const char *input, size_t in_len, const char *output, size_t out_len)
{
    if (!input || !output || in_len == 0 || out_len == 0)
        return 0.3f;

    int match_count = 0;
    int check_words = 0;

    for (size_t i = 0; i < in_len && check_words < 10; i++) {
        if (input[i] == ' ' || i == in_len - 1) {
            check_words++;
            size_t word_start = i;
            while (word_start > 0 && input[word_start - 1] != ' ')
                word_start--;
            size_t word_len = i - word_start + (i == in_len - 1 && input[i] != ' ' ? 1 : 0);

            if (word_len >= 3 && word_len <= 30) {
                char found = 0;
                for (size_t j = 0; j + word_len <= out_len && !found; j++) {
                    if (mc_word_eq(output + j, input + word_start, word_len) &&
                        (j == 0 || output[j - 1] == ' ') &&
                        (j + word_len >= out_len || output[j + word_len] == ' ' ||
                         output[j + word_len] == '.' || output[j + word_len] == ',')) {
                        match_count++;
                        found = 1;
                    }
                }
            }
        }
    }

    if (check_words == 0)
        return 0.8f;
    return clampf((float)match_count / (float)(check_words > 0 ? check_words : 1), 0.05f, 1.0f);
}

static float score_accuracy(const char *content, size_t len)
{
    if (!content || len == 0)
        return 0.3f;

    int suspicious_patterns = 0;
    int total_checks = 4;

    const char *patterns[] = {"I think maybe possibly", "I am not sure but",
                              "it could be that perhaps", "I guess"};

    for (int p = 0; p < total_checks; p++) {
        if (mc_find(content, patterns[p]))
            suspicious_patterns++;
    }

    float base_score = 0.9f - (float)suspicious_patterns * 0.15f;

    int has_evidence = mc_find(content, "because") || mc_find(content, "since") ||
                       mc_find(content, "according to") || mc_find(content, "evidence");
    if (has_evidence)
        base_score += 0.05f;

    return clampf(base_score, 0.1f, 1.0f);
}

static float score_completeness(const char *content, size_t len)
{
    if (!content || len == 0)
        return 0.2f;

    int aspects = 0;
    if (mc_find(content, "first") || mc_find(content, "initially"))
        aspects++;
    if (mc_find(content, "second") || mc_find(content, "then"))
        aspects++;
    if (mc_find(content, "finally") || mc_find(content, "conclusion") ||
        mc_find(content, "in summary"))
        aspects++;
    if (len > 100)
        aspects++;

    return clampf(0.2f + (float)aspects * 0.2f, 0.2f, 1.0f);
}

static float score_consistency(const char *context, size_t ctx_len, const char *content,
                               size_t cnt_len)
{
    if (!context || !content || ctx_len == 0 || cnt_len == 0)
        return 0.7f;

    int contradictions = 0;
    const char *contra_patterns[] = {"however", "but on the other hand", "contrary to",
                                     "despite this"};

    for (int p = 0; p < 4; p++) {
        if (mc_find(content, contra_patterns[p]))
            contradictions++;
    }

    float score = 0.85f - (float)contradictions * 0.15f;

    if (ctx_len > 20 && cnt_len > 20) {
        size_t min_len = ctx_len < cnt_len ? ctx_len : cnt_len;
        int matches = 0;
        for (size_t i = 0; i + 5 < min_len; i += 6) {
            if (context[i] == content[i])
                matches++;
        }
        if (matches > (int)(min_len / 12))
            score += 0.05f;
    }

    return clampf(score, 0.2f, 1.0f);
}

static float score_clarity(const char *content, size_t len)
{
    if (!content || len == 0)
        return 0.3f;

    int sentences = 0;
    int long_sentences = 0;
    int current_sentence_len = 0;

    for (size_t i = 0; i < len; i++) {
        current_sentence_len++;
        if (content[i] == '.' || content[i] == '!' || content[i] == '?') {
            sentences++;
            if (current_sentence_len > 50)
                long_sentences++;
            current_sentence_len = 0;
        }
    }

    if (sentences == 0)
        return 0.4f;

    float clarity = 1.0f - (float)long_sentences / (float)sentences * 0.3f;

    int has_structure = (mc_find(content, "first") || mc_find(content, "1.")) &&
                        (mc_find(content, "second") || mc_find(content, "2."));
    if (has_structure)
        clarity += 0.1f;

    return clampf(clarity, 0.2f, 1.0f);
}

airy_err_t airy_mc_evaluate_step(airy_metacognition_t *mc, airy_thinking_step_t *step,
                                 const char *context, size_t context_len,
                                 mc_evaluation_result_t *out_result)
{

    if (!mc || !step || !out_result) {
        AIRY_LOG_ERROR("airy_mc_evaluate_step: NULL/invalid params (mc=%p step=%p out_result=%p)",
                       (void *)mc, (void *)step, (void *)out_result);
        return AIRY_EINVAL;
    }

    __builtin_memset(out_result, 0, sizeof(mc_evaluation_result_t));
    mc->total_evaluations++;

    const char *input = step->raw_input ? step->raw_input : "";
    size_t in_len = step->raw_input_len;

    const char *content = step->content ? step->content : context;
    size_t cnt_len = step->content_len ? step->content_len : context_len;

    out_result->dimensions[MC_DIM_RELEVANCE].dimension = MC_DIM_RELEVANCE;
    out_result->dimensions[MC_DIM_RELEVANCE].score =
        score_relevance(input, in_len, content, cnt_len);

    out_result->dimensions[MC_DIM_ACCURACY].dimension = MC_DIM_ACCURACY;
    out_result->dimensions[MC_DIM_ACCURACY].score = score_accuracy(content, cnt_len);

    out_result->dimensions[MC_DIM_COMPLETENESS].dimension = MC_DIM_COMPLETENESS;
    out_result->dimensions[MC_DIM_COMPLETENESS].score = score_completeness(content, cnt_len);

    out_result->dimensions[MC_DIM_CONSISTENCY].dimension = MC_DIM_CONSISTENCY;
    out_result->dimensions[MC_DIM_CONSISTENCY].score =
        score_consistency(context, context_len, content, cnt_len);

    out_result->dimensions[MC_DIM_CLARITY].dimension = MC_DIM_CLARITY;
    out_result->dimensions[MC_DIM_CLARITY].score = score_clarity(content, cnt_len);

    float weights[MC_DIM_COUNT] = {0.25f, 0.25f, 0.15f, 0.2f, 0.15f};
    float weighted_sum = 0.0f;
    float weight_total = 0.0f;
    for (int d = 0; d < MC_DIM_COUNT; d++) {
        weighted_sum += out_result->dimensions[d].score * weights[d];
        weight_total += weights[d];
    }
    out_result->overall_score = (weight_total > 0.0f) ? weighted_sum / weight_total : 0.0f;

    out_result->calibrated_confidence = airy_mc_calibrate_confidence(mc, step->confidence);

    out_result->is_acceptable = (out_result->overall_score >= mc->acceptance_threshold) ? 1 : 0;

    if (out_result->is_acceptable) {
        mc->consecutive_accepts++;
        mc->consecutive_rejects = 0;
    } else {
        mc->consecutive_rejects++;
        mc->consecutive_accepts = 0;
    }

    if (out_result->overall_score >= mc->acceptance_threshold) {
        out_result->strategy = MC_CORRECT_NONE;
        out_result->severity = MC_SEV_INFO;
    } else if (out_result->overall_score >= mc->auto_correct_threshold) {
        out_result->strategy = MC_CORRECT_AUTO;
        out_result->severity = MC_SEV_WARNING;
    } else if (out_result->overall_score >= 0.3f) {
        out_result->strategy = MC_CORRECT_RERUN;
        out_result->severity = MC_SEV_ERROR;
    } else {
        out_result->strategy = MC_CORRECT_ESCALATE;
        out_result->severity = MC_SEV_CRITICAL;
    }

    char buf[MC_MAX_CRITIQUE_LEN];
    int pos = snprintf(buf, sizeof(buf), "[S1 Evaluation] step#%u: ", step->step_id);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "overall=%.2f [", out_result->overall_score);
    for (int d = 0; d < MC_DIM_COUNT; d++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s=%.2f%s", dim_name((mc_dimension_t)d),
                        out_result->dimensions[d].score, (d < MC_DIM_COUNT - 1) ? "," : "]");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, " strategy=%d severity=%d", out_result->strategy,
                    out_result->severity);

    out_result->critique_text = (char *)AIRY_MALLOC(pos + 1);
    if (out_result->critique_text) {
        __builtin_memcpy(out_result->critique_text, buf, pos + 1);
        out_result->critique_len = (size_t)pos;
    }

    if (mc->record_capacity > 0) {
        mc_evaluation_record_t *rec = &mc->records[mc->record_head % mc->record_capacity];
        if (rec->result.critique_text)
            AIRY_FREE((void *)rec->result.critique_text);
        rec->step_id = step->step_id;
        rec->timestamp_ns = mc_time_now();
        __builtin_memcpy(&rec->result, out_result, sizeof(mc_evaluation_result_t));
        if (out_result->critique_text) {
            rec->result.critique_text = (char *)AIRY_MALLOC(out_result->critique_len + 1);
            if (rec->result.critique_text) {
                __builtin_memcpy((char *)rec->result.critique_text, out_result->critique_text,
                                 out_result->critique_len + 1);
            }
        }
        rec->original_content = content;
        rec->corrected_content = NULL;

        if (mc->record_count < mc->record_capacity)
            mc->record_count++;
        mc->record_head++;
    }

    return AIRY_SUCCESS;
}

airy_err_t airy_mc_evaluate_quick(airy_metacognition_t *mc, airy_thinking_step_t *step,
                                  float *out_score, int *out_acceptable)
{

    if (!mc || !step || !out_score || !out_acceptable) {
        AIRY_LOG_ERROR(
            "airy_mc_evaluate_quick: NULL params (mc=%p step=%p out_score=%p out_acceptable=%p)",
            (void *)mc, (void *)step, (void *)out_score, (void *)out_acceptable);
        return AIRY_EINVAL;
    }

    mc_evaluation_result_t result;
    airy_err_t err = airy_mc_evaluate_step(mc, step, NULL, 0, &result);
    if (err != AIRY_SUCCESS)
        return err;

    *out_score = result.overall_score;
    *out_acceptable = result.is_acceptable;

    if (result.critique_text)
        AIRY_FREE(result.critique_text);
    return AIRY_SUCCESS;
}
