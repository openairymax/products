// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_memory.c
 * @brief Thinking-chain memory integration domain: seven MemoryRovol connection points.
 */

#include "tc_internal.h"

/* ============================================================================
 * P2-B03: MemoryRovol integration - 7 connection points
 * ============================================================================ */

#include "memory.h"
#include "metacognition.h"

void airy_tc_chain_set_memory(airy_thinking_chain_t *chain, airy_memory_engine_t *memory)
{
    if (!chain)
        return;
    chain->memory = memory;
}

airy_err_t airy_tc_context_window_prepop(airy_thinking_chain_t *chain, const char *query_text,
                                         size_t query_len, uint32_t limit)
{

    if (!chain || !query_text || !chain->memory || !chain->ctx_window) {
        AIRY_LOG_ERROR("airy_tc_context_window_prepop: NULL/invalid params (chain=%p query_text=%p "
                       "memory=%p ctx_window=%p)",
                       (void *)chain, (void *)query_text, (void *)(chain ? chain->memory : NULL),
                       (void *)(chain ? chain->ctx_window : NULL));
        return AIRY_EINVAL;
    }

    airy_memory_query_t query;
    __builtin_memset(&query, 0, sizeof(query));
    query.memory_query_text = (char *)query_text;
    query.memory_query_text_len = query_len;
    query.memory_query_limit = limit > 0 ? limit : 5;
    /* P3.11-C1: the query must fill memory_result_item_record (including
     * record content), otherwise prepopulate gets no rec->memory_record_data
     * and memory content cannot be injected into the context window. */
    query.memory_query_include_raw = 1;

    airy_memory_result_ext_t *result = NULL;
    airy_err_t err = airy_memory_query(chain->memory, &query, &result);
    if (err != AIRY_SUCCESS || !result || result->memory_result_count == 0) {
        AIRY_LOG_WARN("airy_tc_context_window_prepop: memory query failed or empty (err=%d "
                      "result=%p count=%zu)",
                      (int)err, (void *)result, result ? result->memory_result_count : 0);
        if (result)
            airy_memory_result_free(result);
        return err == AIRY_SUCCESS ? AIRY_ENOENT : err;
    }

    for (size_t i = 0; i < result->memory_result_count; i++) {
        airy_memory_record_t *rec = result->memory_result_items[i]->memory_result_item_record;
        if (!rec || !rec->memory_record_data)
            continue;

        char prefix[128];
        int plen = snprintf(prefix, sizeof(prefix), "[Memory#%zu score=%.2f] ", i,
                            result->memory_result_items[i]->memory_result_item_score);

        size_t total_len = (size_t)plen + rec->memory_record_data_len + 1;
        char *buf = (char *)AIRY_MALLOC(total_len);
        if (!buf)
            continue;

        __builtin_memcpy(buf, prefix, (size_t)plen);
        __builtin_memcpy(buf + plen, rec->memory_record_data, rec->memory_record_data_len);
        buf[total_len - 1] = '\n';

        airy_tc_context_window_append(chain->ctx_window, buf, total_len);
        AIRY_FREE(buf);
    }

    airy_memory_result_free(result);
    return AIRY_SUCCESS;
}

airy_err_t airy_tc_working_memory_sync_to_persistent(airy_thinking_chain_t *chain,
                                                     float min_importance)
{

    if (!chain || !chain->working_mem || !chain->memory) {
        AIRY_LOG_ERROR(
            "airy_tc_working_memory_sync_to_persistent: NULL params (chain=%p working_mem=%p "
            "memory=%p)",
            (void *)chain, (void *)(chain ? chain->working_mem : NULL),
            (void *)(chain ? chain->memory : NULL));
        return AIRY_EINVAL;
    }

    uint32_t synced = 0;
    for (size_t i = 0; i < chain->working_mem->count; i++) {
        struct wm_entry *e = &chain->working_mem->entries[i];
        if (e->pinned && e->value && e->value_size > 0) {
            airy_memory_record_t rec;
            __builtin_memset(&rec, 0, sizeof(rec));
            rec.memory_record_type = AIRY_MEMTYPE_TEXT;
            rec.memory_record_data = e->value;
            rec.memory_record_data_len = e->value_size;
            rec.memory_record_importance = min_importance > 0.5f ? 0.8f : 0.6f;
            rec.memory_record_source_agent = "thinking_chain_wm";
            rec.memory_record_trace_id = chain->session_goal ? chain->session_goal : "unknown";

            char *record_id = NULL;
            airy_err_t err = airy_memory_write(chain->memory, &rec, &record_id);
            if (err == AIRY_SUCCESS && record_id) {
                synced++;
                AIRY_FREE(record_id);
                record_id = NULL;
            }
        }
    }

    return (synced > 0) ? AIRY_SUCCESS : AIRY_ENOENT;
}

airy_err_t airy_tc_step_write_to_memory(airy_thinking_chain_t *chain, airy_thinking_step_t *step)
{

    if (!chain || !step || !chain->memory) {
        AIRY_LOG_ERROR("airy_tc_step_write_to_memory: NULL params (chain=%p step=%p memory=%p)",
                       (void *)chain, (void *)step, (void *)(chain ? chain->memory : NULL));
        return AIRY_EINVAL;
    }
    if (!step->content || step->content_len == 0) {
        AIRY_LOG_WARN("airy_tc_step_write_to_memory: step has no content (step_id=%u)",
                      step->step_id);
        return AIRY_EINVAL;
    }
    if (step->confidence < 0.6f) {
        AIRY_LOG_WARN(
            "airy_tc_step_write_to_memory: step confidence too low (step_id=%u confidence=%.2f)",
            step->step_id, step->confidence);
        return AIRY_EINVAL;
    }

    airy_memory_record_t rec;
    __builtin_memset(&rec, 0, sizeof(rec));
    rec.memory_record_type = AIRY_MEMTYPE_TEXT;
    rec.memory_record_data = step->content;
    rec.memory_record_data_len = step->content_len;
    rec.memory_record_importance = step->confidence;
    rec.memory_record_source_agent = step->role ? step->role : "t2-generator";
    rec.memory_record_access_count = (uint32_t)(step->correction_count + 1);

    char trace_id[64];
    snprintf(trace_id, sizeof(trace_id), "step_%u", step->step_id);
    rec.memory_record_trace_id = trace_id;

    char *record_id = NULL;
    airy_err_t err = airy_memory_write(chain->memory, &rec, &record_id);
    if (err == AIRY_SUCCESS && record_id && chain->memory) {
        airy_memory_mount(chain->memory, record_id, chain->session_goal ? chain->session_goal : "");
        AIRY_FREE(record_id);
    }

    return err;
}

airy_err_t airy_tc_meta_inform_mem(airy_thinking_chain_t *chain, const void *eval,
                                   airy_thinking_step_t *step)
{

    if (!chain || !eval || !step || !chain->memory) {
        AIRY_LOG_ERROR("airy_tc_meta_inform_mem: NULL params (chain=%p eval=%p step=%p memory=%p)",
                       (void *)chain, (void *)eval, (void *)step,
                       (void *)(chain ? chain->memory : NULL));
        return AIRY_EINVAL;
    }
    const mc_evaluation_result_t *eval_typed = (const mc_evaluation_result_t *)eval;
    if (eval_typed->strategy == MC_CORRECT_NONE)
        return AIRY_SUCCESS;

    float importance = eval_typed->overall_score;
    if (importance > 0.9f)
        importance = 0.95f;
    else if (importance > 0.7f)
        importance = 0.8f;
    else
        importance = 0.5f;

    if (eval_typed->critique_text && eval_typed->critique_len > 0) {
        airy_memory_record_t rec;
        __builtin_memset(&rec, 0, sizeof(rec));
        rec.memory_record_type = AIRY_MEMTYPE_TEXT;
        rec.memory_record_data = (void *)eval_typed->critique_text;
        rec.memory_record_data_len = eval_typed->critique_len;
        rec.memory_record_importance = importance;
        rec.memory_record_source_agent = "s1-metacognition";
        rec.memory_record_trace_id = chain->session_goal ? chain->session_goal : "";

        char *record_id = NULL;
        airy_err_t err = airy_memory_write(chain->memory, &rec, &record_id);
        if (err == AIRY_SUCCESS && record_id) {
            AIRY_FREE(record_id);
            record_id = NULL;
        }
    }

    if (chain->working_mem && step->confidence >= 0.7f) {
        char key[64];
        snprintf(key, sizeof(key), "eval_%u", step->step_id);
        char val_buf[32];
        int vlen = snprintf(val_buf, sizeof(val_buf), "%.3f", eval_typed->overall_score);
        airy_tc_working_memory_store(chain->working_mem, key, val_buf, (size_t)vlen + 1,
                                     "evaluation_score", 1);
    }

    return AIRY_SUCCESS;
}
