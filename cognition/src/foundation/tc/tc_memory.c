// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_memory.c
 * @brief Thinking-chain memory integration domain: provider single-track.
 *
 * 0.1.19 C3：记忆双栈塌缩后，思考链长期记忆统一经 memory provider vtable
 * （registry 活跃件，生产态为 memrpc→mem_d）。写侧以 write_raw + JSON
 * metadata 承载重要性/来源/追踪字段；读侧 query + get_raw 组合还原
 * prepopulate 所需的记录内容。
 */

#include "tc_internal.h"

#include "provider.h"
#include "metacognition.h"

void airy_tc_chain_set_memory(airy_thinking_chain_t *chain, airy_memory_provider_t *memory)
{
    if (!chain)
        return;
    chain->memory = memory;
}

/* provider 写路径单源：组装 metadata JSON 后落 write_raw，record_id 由
 * 调用方释放。importance/source/trace 三元组是各接入点唯一的差异字段。 */
static airy_err_t chain_mem_write(airy_memory_provider_t *provider, const void *data, size_t len,
                                  float importance, const char *source, const char *trace,
                                  char **out_record_id)
{
    char meta_json[256];
    snprintf(meta_json, sizeof(meta_json),
             "{\"type\":\"text\",\"importance\":%.3f,\"source\":\"%s\",\"trace_id\":\"%s\"}",
             importance, source ? source : "unknown", trace ? trace : "");
    return provider->write_raw(provider, data, len, meta_json, out_record_id);
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

    uint32_t want = limit > 0 ? limit : 5;
    char **ids = NULL;
    float *scores = NULL;
    size_t count = 0;
    airy_err_t err = chain->memory->query(chain->memory, query_text, want, &ids, &scores, &count);
    if (err != AIRY_SUCCESS || count == 0) {
        AIRY_LOG_WARN("airy_tc_context_window_prepop: memory query failed or empty (err=%d "
                      "count=%zu)",
                      (int)err, count);
        airy_memory_provider_free_query_results(ids, scores, count);
        return err == AIRY_SUCCESS ? AIRY_ENOENT : err;
    }

    for (size_t i = 0; i < count; i++) {
        if (!ids[i])
            continue;
        void *raw = NULL;
        size_t raw_len = 0;
        if (chain->memory->get_raw(chain->memory, ids[i], &raw, &raw_len) != AIRY_SUCCESS ||
            !raw || raw_len == 0) {
            continue;
        }

        char prefix[128];
        int plen = snprintf(prefix, sizeof(prefix), "[Memory#%zu score=%.2f] ", i, scores[i]);

        size_t total_len = (size_t)plen + raw_len + 1;
        char *buf = (char *)AIRY_MALLOC(total_len);
        if (!buf) {
            AIRY_FREE(raw);
            continue;
        }

        __builtin_memcpy(buf, prefix, (size_t)plen);
        __builtin_memcpy(buf + plen, raw, raw_len);
        buf[total_len - 1] = '\n';

        airy_tc_context_window_append(chain->ctx_window, buf, total_len);
        AIRY_FREE(buf);
        AIRY_FREE(raw);
    }

    airy_memory_provider_free_query_results(ids, scores, count);
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
            char *record_id = NULL;
            airy_err_t err = chain_mem_write(
                chain->memory, e->value, e->value_size, min_importance > 0.5f ? 0.8f : 0.6f,
                "thinking_chain_wm", chain->session_goal, &record_id);
            if (err == AIRY_SUCCESS && record_id) {
                synced++;
                AIRY_FREE(record_id);
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

    char trace_id[64];
    snprintf(trace_id, sizeof(trace_id), "step_%u", step->step_id);

    char *record_id = NULL;
    airy_err_t err = chain_mem_write(chain->memory, step->content, step->content_len,
                                     step->confidence, step->role ? step->role : "t2-generator",
                                     trace_id, &record_id);
    if (err == AIRY_SUCCESS && record_id) {
        if (chain->memory->mount) {
            chain->memory->mount(chain->memory, record_id,
                                 chain->session_goal ? chain->session_goal : "");
        }
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
        char *record_id = NULL;
        airy_err_t err = chain_mem_write(chain->memory, eval_typed->critique_text,
                                         eval_typed->critique_len, importance, "s1-metacognition",
                                         chain->session_goal, &record_id);
        if (err == AIRY_SUCCESS && record_id) {
            AIRY_FREE(record_id);
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
