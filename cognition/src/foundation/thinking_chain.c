// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file thinking_chain.c
 * @brief Thinking-chain core domain: chain orchestration and time utilities.
 */

#include "thinking_chain.h"

#include "airy_rt.h"
#include "logging.h"
#include "airy_memory.h"
#include "platform.h"
#include "tc/tc_internal.h"
#include "string_compat.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

uint64_t tc_time_now_ns(void)
{
    return airy_time_ns();
}
/* ============================================================================
 * Thinking Chain orchestration implementation
 * ============================================================================ */

airy_err_t airy_tc_chain_create(const char *goal, size_t max_tokens, size_t wm_capacity,
                                airy_thinking_chain_t **out_chain)
{

    if (!out_chain) {
        AIRY_LOG_ERROR("airy_tc_chain_create: NULL out_chain parameter");
        return AIRY_EINVAL;
    }

    airy_thinking_chain_t *chain =
        (airy_thinking_chain_t *)AIRY_CALLOC(1, sizeof(airy_thinking_chain_t));
    if (!chain) {
        AIRY_LOG_ERROR(
            "airy_tc_chain_create: chain allocation failed (max_tokens=%zu wm_capacity=%zu)",
            max_tokens, wm_capacity);
        return AIRY_ENOMEM;
    }

    chain->session_id = tc_time_now_ns();
    chain->session_goal = goal ? AIRY_STRDUP(goal) : AIRY_STRDUP("");
    chain->active = 0;
    chain->next_step_id = 0;
    chain->created_ns = tc_time_now_ns();
    chain->last_activity_ns = chain->created_ns;
    chain->on_step_completed = NULL;
    chain->on_correction = NULL;
    chain->callback_user_data = NULL;

    airy_err_t err = airy_tc_context_window_create(max_tokens, &chain->ctx_window);
    if (err != AIRY_SUCCESS) {
        AIRY_LOG_ERROR(
            "airy_tc_chain_create: context_window creation failed (err=%d max_tokens=%zu)",
            (int)err, max_tokens);
        if (chain->session_goal)
            AIRY_FREE(chain->session_goal);
        AIRY_FREE(chain);
        return err;
    }

    err = airy_tc_working_memory_create(wm_capacity, &chain->working_mem);
    if (err != AIRY_SUCCESS) {
        AIRY_LOG_ERROR(
            "airy_tc_chain_create: working_memory creation failed (err=%d wm_capacity=%zu)",
            (int)err, wm_capacity);
        airy_tc_context_window_destroy(chain->ctx_window);
        if (chain->session_goal)
            AIRY_FREE(chain->session_goal);
        AIRY_FREE(chain);
        return err;
    }

    chain->step_capacity = 32;
    chain->steps =
        (airy_thinking_step_t **)AIRY_CALLOC(chain->step_capacity, sizeof(airy_thinking_step_t *));
    if (!chain->steps) {
        AIRY_LOG_ERROR("airy_tc_chain_create: steps allocation failed (step_capacity=%zu)",
                       chain->step_capacity);
        airy_tc_working_memory_destroy(chain->working_mem);
        airy_tc_context_window_destroy(chain->ctx_window);
        if (chain->session_goal)
            AIRY_FREE(chain->session_goal);
        AIRY_FREE(chain);
        return AIRY_ENOMEM;
    }
    chain->step_count = 0;

    *out_chain = chain;
    return AIRY_SUCCESS;
}

void airy_tc_chain_destroy(airy_thinking_chain_t *chain)
{
    if (!chain)
        return;

    for (size_t i = 0; i < chain->step_count; i++) {
        airy_thinking_step_t *s = chain->steps[i];
        if (!s)
            continue;
        if (s->raw_input)
            AIRY_FREE(s->raw_input);
        if (s->content)
            AIRY_FREE(s->content);
        if (s->critique)
            AIRY_FREE(s->critique);
        if (s->role)
            AIRY_FREE(s->role);
        if (s->depends_on)
            AIRY_FREE(s->depends_on);
        if (s->dependents)
            AIRY_FREE(s->dependents);
        if (s->correction_history) {
            for (size_t c = 0; c < s->correction_history_count; c++)
                AIRY_FREE(s->correction_history[c]);
            AIRY_FREE(s->correction_history);
        }
        AIRY_FREE(s);
        chain->steps[i] = NULL;
    }
    AIRY_FREE(chain->steps);

    airy_tc_context_window_destroy(chain->ctx_window);
    airy_tc_working_memory_destroy(chain->working_mem);
    if (chain->session_goal)
        AIRY_FREE(chain->session_goal);
    AIRY_FREE(chain);
}

airy_err_t airy_tc_chain_start(airy_thinking_chain_t *chain)
{
    if (!chain) {
        AIRY_LOG_ERROR("airy_tc_chain_start: NULL chain parameter");
        return AIRY_EINVAL;
    }
    chain->active = 1;
    chain->last_activity_ns = tc_time_now_ns();
    return AIRY_SUCCESS;
}

void airy_tc_chain_stop(airy_thinking_chain_t *chain)
{
    if (!chain)
        return;
    chain->active = 0;
}

airy_err_t airy_tc_chain_next_ready_step(airy_thinking_chain_t *chain,
                                         airy_thinking_step_t **out_step)
{

    if (!chain || !out_step) {
        AIRY_LOG_ERROR("airy_tc_chain_next_ready_step: NULL params (chain=%p out_step=%p)",
                       (void *)chain, (void *)out_step);
        return AIRY_EINVAL;
    }

    for (size_t i = 0; i < chain->step_count; i++) {
        airy_thinking_step_t *s = chain->steps[i];
        if (s->status == TC_STATUS_PENDING) {
            int ready = airy_tc_step_is_ready(s, chain);
            if (ready > 0) {
                s->status = TC_STATUS_EXECUTING;
                s->start_time_ns = tc_time_now_ns();
                s->chain_ref = chain;
                *out_step = s;
                chain->last_activity_ns = tc_time_now_ns();

                if (chain->ctx_window && s->raw_input) {
                    airy_tc_context_window_append(chain->ctx_window, s->raw_input,
                                                  s->raw_input_len);
                }

                return AIRY_SUCCESS;
            }
        }
    }

    *out_step = NULL;
    AIRY_LOG_WARN("airy_tc_chain_next_ready_step: no ready step found (step_count=%zu)",
                  chain->step_count);
    return AIRY_ENOENT;
}

airy_err_t airy_tc_chain_stats(airy_thinking_chain_t *chain, char **out_json, size_t *out_len)
{

    if (!chain || !out_json) {
        AIRY_LOG_ERROR("airy_tc_chain_stats: NULL params (chain=%p out_json=%p)", (void *)chain,
                       (void *)out_json);
        return AIRY_EINVAL;
    }

    char *cw_json = NULL;
    airy_tc_context_window_stats(chain->ctx_window, &cw_json);

    uint32_t pending = 0, executing = 0, completed = 0, corrected = 0, failed = 0;
    for (size_t i = 0; i < chain->step_count; i++) {
        if (!chain->steps[i])
            continue;
        switch (chain->steps[i]->status) {
        case TC_STATUS_PENDING:
            pending++;
            break;
        case TC_STATUS_EXECUTING:
            executing++;
            break;
        case TC_STATUS_COMPLETED:
            completed++;
            break;
        case TC_STATUS_CORRECTED:
            corrected++;
            break;
        case TC_STATUS_FAILED:
            failed++;
            break;
        default:
            break;
        }
    }

#ifdef AIRY_HAS_CJSON
    /* cJSON 路径：自动 JSON 转义（goal 含 " / \ 不产生非法 JSON），
     * 输出长度由 PrintUnformatted 动态分配，无固定缓冲截断越界读。 */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        if (cw_json)
            AIRY_FREE(cw_json);
        return AIRY_ENOMEM;
    }
    cJSON_AddNumberToObject(root, "session_id", (double)(uint64_t)chain->session_id);
    cJSON_AddStringToObject(root, "goal", chain->session_goal ? chain->session_goal : "");
    cJSON_AddBoolToObject(root, "active", chain->active);
    cJSON *steps = cJSON_CreateObject();
    if (steps) {
        cJSON_AddNumberToObject(steps, "total", (double)chain->step_count);
        cJSON_AddNumberToObject(steps, "pending", (double)pending);
        cJSON_AddNumberToObject(steps, "executing", (double)executing);
        cJSON_AddNumberToObject(steps, "completed", (double)completed);
        cJSON_AddNumberToObject(steps, "corrected", (double)corrected);
        cJSON_AddNumberToObject(steps, "failed", (double)failed);
        cJSON_AddItemToObject(root, "steps", steps);
    }
    if (cw_json) {
        cJSON *cw = cJSON_Parse(cw_json);
        if (cw)
            cJSON_AddItemToObject(root, "context_window", cw);
        else
            cJSON_AddStringToObject(root, "context_window", cw_json);
    } else {
        cJSON *cw_empty = cJSON_CreateObject();
        if (cw_empty)
            cJSON_AddItemToObject(root, "context_window", cw_empty);
    }
    if (chain->working_mem) {
        cJSON *wm = cJSON_CreateObject();
        if (wm) {
            cJSON_AddNumberToObject(wm, "capacity", (double)chain->working_mem->capacity);
            cJSON_AddNumberToObject(wm, "count", (double)chain->working_mem->count);
            cJSON_AddNumberToObject(wm, "hits", (double)chain->working_mem->hits);
            cJSON_AddNumberToObject(wm, "misses", (double)chain->working_mem->misses);
            cJSON_AddItemToObject(root, "working_memory", wm);
        }
    }
    if (cw_json) {
        AIRY_FREE(cw_json);
        cw_json = NULL;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return AIRY_ENOMEM;
    *out_json = json;
    if (out_len)
        *out_len = strlen(json);
    return AIRY_SUCCESS;
#else
    /* 无 cJSON 回退：动态扩容循环，杜绝固定缓冲截断后的越界读
     * （此前 buf[1024] + memcpy(result, buf, len+1) 在 len>1023 时越界）。 */
    size_t cap = 1024;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf) {
        if (cw_json)
            AIRY_FREE(cw_json);
        return AIRY_ENOMEM;
    }
    for (;;) {
        int len = snprintf(
            buf, cap,
            "{\"session_id\":%llu,"
            "\"goal\":\"%s\","
            "\"active\":%d,"
            "\"steps\":{\"total\":%zu,\"pending\":%u,\"executing\":%u,"
            "\"completed\":%u,\"corrected\":%u,\"failed\":%u},"
            "\"context_window\":%s,"
            "\"working_memory\":{\"capacity\":%zu,\"count\":%zu,\"hits\":%llu,\"misses\":%llu}}",
            (unsigned long long)chain->session_id, chain->session_goal ? chain->session_goal : "",
            chain->active, chain->step_count, pending, executing, completed, corrected, failed,
            cw_json ? cw_json : "{}", chain->working_mem ? chain->working_mem->capacity : 0,
            chain->working_mem ? chain->working_mem->count : 0,
            chain->working_mem ? (unsigned long long)chain->working_mem->hits : 0ULL,
            chain->working_mem ? (unsigned long long)chain->working_mem->misses : 0ULL);
        if (len < 0) {
            AIRY_FREE(buf);
            if (cw_json)
                AIRY_FREE(cw_json);
            return AIRY_EUNKNOWN;
        }
        if ((size_t)len < cap)
            break;
        cap = (size_t)len + 1;
        char *nb = (char *)AIRY_REALLOC(buf, cap);
        if (!nb) {
            AIRY_FREE(buf);
            if (cw_json)
                AIRY_FREE(cw_json);
            return AIRY_ENOMEM;
        }
        buf = nb;
    }
    if (cw_json) {
        AIRY_FREE(cw_json);
        cw_json = NULL;
    }
    *out_json = buf;
    if (out_len)
        *out_len = strlen(buf);
    return AIRY_SUCCESS;
#endif
}

void airy_tc_chain_set_step_cb(airy_thinking_chain_t *chain,
                               void (*on_step_completed)(airy_thinking_step_t *, void *),
                               void (*on_correction)(airy_thinking_step_t *, const char *, void *),
                               void *user_data)
{

    if (!chain)
        return;
    chain->on_step_completed = on_step_completed;
    chain->on_correction = on_correction;
    chain->callback_user_data = user_data;
}

