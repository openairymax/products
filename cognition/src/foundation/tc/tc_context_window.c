// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_context_window.c
 * @brief Thinking-chain Context Window domain: token-budget management and sliding window.
 */

#include "tc_internal.h"

static size_t estimate_token_count(const char *data, size_t len)
{
    if (!data || len == 0)
        return 0;
    size_t tokens = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == ' ' || data[i] == '\n' || data[i] == '\t')
            tokens++;
    }
    return (tokens > 0) ? tokens : (len / 4);
}

/* ============================================================================
 * Context Window implementation
 * ============================================================================ */

#define CW_BUFFER_MIN_SIZE 65536

airy_err_t airy_tc_context_window_create(size_t max_tokens, airy_context_window_t **out_window)
{

    if (!out_window) {
        AIRY_LOG_ERROR("airy_tc_context_window_create: NULL out_window parameter");
        return AIRY_EINVAL;
    }

    airy_context_window_t *w =
        (airy_context_window_t *)AIRY_CALLOC(1, sizeof(airy_context_window_t));
    if (!w) {
        AIRY_LOG_ERROR(
            "airy_tc_context_window_create: allocation failed for context window (max_tokens=%zu)",
            max_tokens);
        return AIRY_ENOMEM;
    }

    w->max_tokens = (max_tokens > 0) ? max_tokens : TC_MAX_TOKENS_DEFAULT;
    w->used_tokens = 0;
    w->chunk_size = TC_CHUNK_SIZE_DEFAULT;
    w->buffer_capacity = w->max_tokens * 8;
    if (w->buffer_capacity < CW_BUFFER_MIN_SIZE)
        w->buffer_capacity = CW_BUFFER_MIN_SIZE;

    w->buffer = (char *)AIRY_CALLOC(1, w->buffer_capacity);
    if (!w->buffer) {
        AIRY_LOG_ERROR("airy_tc_context_window_create: buffer allocation failed (capacity=%zu)",
                       w->buffer_capacity);
        AIRY_FREE(w);
        return AIRY_ENOMEM;
    }

    w->buffer_head = 0;
    w->buffer_tail = 0;
    w->buffer_used = 0;
    w->total_tokens_generated = 0;
    w->total_corrections = 0;
    w->total_steps = 0;
    w->completed_steps = 0;
    w->enable_dynamic_chunk = 1;
    w->low_confidence_threshold = 0.6f;
    w->high_confidence_threshold = 0.9f;
    w->max_corrections_per_chunk = TC_MAX_CORRECTIONS_DEFAULT;

    *out_window = w;
    return AIRY_SUCCESS;
}

void airy_tc_context_window_destroy(airy_context_window_t *window)
{
    if (!window)
        return;
    if (window->buffer)
        AIRY_FREE(window->buffer);
    AIRY_FREE(window);
}

ssize_t airy_tc_context_window_append(airy_context_window_t *window, const char *data, size_t len)
{

    if (!window || !data || len == 0) {
        AIRY_LOG_ERROR(
            "airy_tc_context_window_append: NULL/invalid params (window=%p data=%p len=%zu)",
            (void *)window, (void *)data, len);
        return (ssize_t)AIRY_EINVAL;
    }

    size_t new_tokens = estimate_token_count(data, len);

    if (window->used_tokens + new_tokens > window->max_tokens) {
        AIRY_LOG_WARN("airy_tc_context_window_append: token limit exceeded (used=%zu + new=%zu > "
                      "max=%zu), sliding window eviction triggered",
                      window->used_tokens, new_tokens, window->max_tokens);

        size_t to_evict = (window->used_tokens + new_tokens) - window->max_tokens;
        size_t evicted_bytes = 0;
        size_t evicted_ws = 0;
        size_t evicted_other = 0;
        while (evicted_bytes < to_evict * 4 && window->buffer_used > 0) {
            char c = window->buffer[window->buffer_tail];
            window->buffer_tail = (window->buffer_tail + 1) % window->buffer_capacity;
            window->buffer_used--;
            evicted_bytes++;
            if (c == ' ' || c == '\n' || c == '\t')
                evicted_ws++;
            else
                evicted_other++;
        }
        /* 记账与 estimate_token_count 同构：按被淘汰内容的同一估算口径
         * 回扣 used_tokens。此前只按空白字节递减，中文等无空白内容淘汰后
         * used_tokens 几乎不降，叠加追加时无条件累加，导致只增不减。 */
        size_t evicted_tokens = (evicted_ws > 0) ? evicted_ws : evicted_other / 4;
        window->used_tokens =
            (window->used_tokens > evicted_tokens) ? window->used_tokens - evicted_tokens : 0;
    }

    for (size_t i = 0; i < len; i++) {
        window->buffer[window->buffer_head] = data[i];
        window->buffer_head = (window->buffer_head + 1) % window->buffer_capacity;
        window->buffer_used++;
        if (window->buffer_used >= window->buffer_capacity) {
            AIRY_LOG_WARN("airy_tc_context_window_append: buffer capacity reached, truncating "
                          "(buffer_used=%zu capacity=%zu input_len=%zu)",
                          window->buffer_used, window->buffer_capacity, len);
            break;
        }
    }

    window->used_tokens += new_tokens;
    /* 单段超窗（new_tokens > max_tokens）：窗口无法容纳，按上限封顶，
     * 保证 used_tokens 恒不超过 max_tokens。 */
    if (window->used_tokens > window->max_tokens)
        window->used_tokens = window->max_tokens;
    window->total_tokens_generated += new_tokens;
    return (ssize_t)(window->used_tokens);
}

airy_err_t airy_tc_context_window_get_recent(airy_context_window_t *window, size_t token_count,
                                         char **out_data, size_t *out_len)
{

    if (!window || !out_data) {
        AIRY_LOG_ERROR("airy_tc_context_window_get_recent: NULL params (window=%p out_data=%p)",
                       (void *)window, (void *)out_data);
        return AIRY_EINVAL;
    }

    size_t avail =
        (token_count > 0 && token_count < window->used_tokens) ? token_count : window->used_tokens;
    if (avail == 0) {
        *out_data = AIRY_STRDUP("");
        if (out_len)
            *out_len = 0;
        return AIRY_SUCCESS;
    }

    size_t est_bytes = avail * 4;
    char *result = (char *)AIRY_MALLOC(est_bytes + 1);
    if (!result) {
        AIRY_LOG_ERROR("airy_tc_context_window_get_recent: allocation failed (est_bytes=%zu)",
                       est_bytes + 1);
        return AIRY_ENOMEM;
    }

    size_t read_pos = (window->buffer_head > avail * 4) ? (window->buffer_head - avail * 4) : 0;
    if (read_pos >= window->buffer_capacity)
        read_pos = 0;

    size_t count = 0;
    size_t written = 0;
    size_t start = read_pos;

    do {
        if (written >= est_bytes)
            break;
        result[written++] = window->buffer[read_pos];
        read_pos = (read_pos + 1) % window->buffer_capacity;
        count++;
        if (window->buffer[read_pos - 1] == ' ' || window->buffer[read_pos - 1] == '\n')
            avail--;
    } while (count < window->buffer_used && read_pos != start && avail > 0);

    result[written] = '\0';
    *out_data = result;
    if (out_len)
        *out_len = written;
    return AIRY_SUCCESS;
}

int airy_tc_ctx_window_has_space(airy_context_window_t *window, size_t needed_tokens)
{

    if (!window)
        return 0;
    return (int)(window->used_tokens + needed_tokens <= window->max_tokens);
}

airy_err_t airy_tc_context_window_stats(airy_context_window_t *window, char **out_json)
{

    if (!window || !out_json) {
        AIRY_LOG_ERROR("airy_tc_context_window_stats: NULL params (window=%p out_json=%p)",
                       (void *)window, (void *)out_json);
        return AIRY_EINVAL;
    }

    char buf[512];
    snprintf(
        buf, sizeof(buf),
        "{\"max_tokens\":%zu,\"used_tokens\":%zu,"
        "\"generated\":%llu,\"corrections\":%llu,"
        "\"steps\":{\"total\":%u,\"completed\":%u},"
        "\"utilization_pct\":%.1f}",
        window->max_tokens, window->used_tokens, (unsigned long long)window->total_tokens_generated,
        (unsigned long long)window->total_corrections, window->total_steps, window->completed_steps,
        window->max_tokens > 0 ? (float)window->used_tokens / (float)window->max_tokens * 100.0f :
                                 0.0f);

    char *result = AIRY_STRDUP(buf);
    if (!result) {
        AIRY_LOG_ERROR("airy_tc_context_window_stats: STRDUP failed for stats JSON");
        return AIRY_ENOMEM;
    }
    *out_json = result;
    return AIRY_SUCCESS;
}
