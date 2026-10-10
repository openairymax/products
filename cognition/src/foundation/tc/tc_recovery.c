// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_recovery.c
 * @brief Thinking-chain anomaly-recovery domain: retry/degrade/rollback and checkpoints.
 */

#include "tc_internal.h"

/* ============================================================================
 * Anomaly-recovery implementation
 * ============================================================================ */

#define TC_MAX_RECOVERY_ATTEMPTS 3
#define TC_RETRY_BACKOFF_BASE_MS 500
#define TC_RETRY_BACKOFF_MAX_MS 8000

typedef struct tc_checkpoint {
    uint32_t checkpoint_id;
    size_t step_snapshot_count;
    uint64_t timestamp_ns;
} tc_checkpoint_t;

#define TC_MAX_CHECKPOINTS 16

static tc_checkpoint_t *get_checkpoint_storage(airy_thinking_chain_t *chain)
{
    static tc_checkpoint_t s_checkpoints[TC_MAX_CHECKPOINTS] = {{0}};
    return s_checkpoints;
}

airy_err_t airy_tc_step_recover(airy_thinking_chain_t *chain, airy_thinking_step_t *failed_step,
                                const tc_monitor_result_t *monitor_result,
                                airy_err_t (*corrector_fn)(const char *, size_t, char **, size_t *,
                                                           void *),
                                void *user_data, tc_recovery_result_t *out_result)
{
    if (!chain || !failed_step || !out_result) {
        AIRY_LOG_ERROR("airy_tc_step_recover: NULL params (chain=%p failed_step=%p out_result=%p)",
                       (void *)chain, (void *)failed_step, (void *)out_result);
        return AIRY_EINVAL;
    }

    __builtin_memset(out_result, 0, sizeof(tc_recovery_result_t));
    out_result->strategy_used = TC_RECOVER_ABORT;
    out_result->success = 0;

    tc_monitor_result_t local_mon;
    if (!monitor_result) {
        airy_tc_step_monitor(failed_step, NULL, &local_mon);
        monitor_result = &local_mon;
    }

    size_t log_buf_size = 512;
    char *log_buf = (char *)AIRY_MALLOC(log_buf_size);
    if (!log_buf) {
        AIRY_LOG_ERROR("airy_tc_step_recover: log buffer allocation failed (size=%zu)",
                       log_buf_size);
        return AIRY_ENOMEM;
    }
    int log_pos = 0;

    log_pos += snprintf(log_buf + log_pos, log_buf_size - log_pos,
                        "Recovery for step#%u (anomaly=%d): ", failed_step->step_id,
                        monitor_result->anomaly);

    if (corrector_fn && failed_step->raw_input && failed_step->raw_input_len > 0) {
        for (uint32_t attempt = 0; attempt < TC_MAX_RECOVERY_ATTEMPTS; attempt++) {
            out_result->attempts_made++;

            char *new_content = NULL;
            size_t new_len = 0;
            airy_err_t err = corrector_fn(failed_step->raw_input, failed_step->raw_input_len,
                                          &new_content, &new_len, user_data);

            if (err == AIRY_SUCCESS && new_content && new_len > 0) {
                airy_tc_step_correct(failed_step, new_content, new_len);
                AIRY_FREE(new_content);
                new_content = NULL;

                tc_monitor_result_t post_mon;
                airy_tc_step_monitor(failed_step, NULL, &post_mon);
                if (post_mon.anomaly == TC_ANOMALY_NONE || post_mon.severity_score < 0.4f) {
                    out_result->strategy_used = TC_RECOVER_RETRY;
                    out_result->success = 1;
                    log_pos += snprintf(log_buf + log_pos, log_buf_size - log_pos,
                                        "retry#%u succeeded", attempt + 1);
                    if (post_mon.description)
                        AIRY_FREE(post_mon.description);
                    goto recovery_done;
                }
                if (post_mon.description)
                    AIRY_FREE(post_mon.description);
            }

            unsigned backoff = TC_RETRY_BACKOFF_BASE_MS << attempt;
            if (backoff > TC_RETRY_BACKOFF_MAX_MS)
                backoff = TC_RETRY_BACKOFF_MAX_MS;
            airy_sleep_ms(backoff);
        }
        log_pos += snprintf(log_buf + log_pos, log_buf_size - log_pos, ", %u retries exhausted",
                            out_result->attempts_made);
    }

    if (failed_step->content && failed_step->content_len > 0) {
        failed_step->status = TC_STATUS_SKIPPED;
        out_result->strategy_used = TC_RECOVER_DEGRADE;
        out_result->success = 1;
        log_pos += snprintf(log_buf + log_pos, log_buf_size - log_pos,
                            ", degraded to skip (kept existing content)");
        goto recovery_done;
    }

    tc_checkpoint_t *checkpoints = get_checkpoint_storage(chain);
    uint32_t best_cp = 0;
    for (int c = TC_MAX_CHECKPOINTS - 1; c >= 0; c--) {
        if (checkpoints[c].checkpoint_id > 0 &&
            checkpoints[c].step_snapshot_count < chain->step_count) {
            best_cp = checkpoints[c].checkpoint_id;
            break;
        }
    }

    if (best_cp > 0) {
        size_t removed = airy_tc_chain_rollback(chain, best_cp);
        out_result->strategy_used = TC_RECOVER_ROLLBACK;
        out_result->success = (removed > 0) ? 1 : 0;
        log_pos += snprintf(log_buf + log_pos, log_buf_size - log_pos,
                            ", rolled back to cp#%u (%zu steps removed)", best_cp, removed);
        goto recovery_done;
    }

    log_pos +=
        snprintf(log_buf + log_pos, log_buf_size - log_pos, ", all strategies failed -> ABORT");
    failed_step->status = TC_STATUS_FAILED;

recovery_done:
    out_result->recovery_log = log_buf;
    out_result->recovery_log_len = (size_t)log_pos;

    AIRY_LOG_INFO("TC Recovery: step#%u strategy=%d success=%d attempts=%u", failed_step->step_id,
                  out_result->strategy_used, out_result->success, out_result->attempts_made);
    return AIRY_SUCCESS;
}

uint32_t airy_tc_chain_checkpoint(airy_thinking_chain_t *chain)
{
    if (!chain)
        return 0;

    tc_checkpoint_t *checkpoints = get_checkpoint_storage(chain);

    uint32_t next_id = 0;
    for (int c = 0; c < TC_MAX_CHECKPOINTS; c++) {
        if (checkpoints[c].checkpoint_id >= next_id)
            next_id = checkpoints[c].checkpoint_id + 1;
    }
    if (next_id == 0)
        next_id = 1;

    int slot = -1;
    for (int c = 0; c < TC_MAX_CHECKPOINTS; c++) {
        if (checkpoints[c].checkpoint_id == 0) {
            slot = c;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
        __builtin_memmove(&checkpoints[0], &checkpoints[1],
                          (TC_MAX_CHECKPOINTS - 1) * sizeof(tc_checkpoint_t));
        checkpoints[TC_MAX_CHECKPOINTS - 1].checkpoint_id = 0;
    }

    checkpoints[slot].checkpoint_id = next_id;
    checkpoints[slot].step_snapshot_count = chain->step_count;
    checkpoints[slot].timestamp_ns = tc_time_now_ns();

    AIRY_LOG_INFO("TC Checkpoint#%u created at step_count=%zu", next_id, chain->step_count);
    return next_id;
}

size_t airy_tc_chain_rollback(airy_thinking_chain_t *chain, uint32_t checkpoint_id)
{
    if (!chain || checkpoint_id == 0)
        return 0;

    tc_checkpoint_t *checkpoints = get_checkpoint_storage(chain);
    size_t target_steps = 0;

    for (int c = 0; c < TC_MAX_CHECKPOINTS; c++) {
        if (checkpoints[c].checkpoint_id == checkpoint_id) {
            target_steps = checkpoints[c].step_snapshot_count;
            break;
        }
    }

    if (target_steps == 0 || target_steps >= chain->step_count)
        return 0;

    size_t removed = chain->step_count - target_steps;

    for (size_t i = target_steps; i < chain->step_count; i++) {
        airy_thinking_step_t *step = chain->steps[i];
        if (!step)
            continue;
        if (step->content)
            AIRY_FREE(step->content);
        if (step->raw_input)
            AIRY_FREE(step->raw_input);
        if (step->critique)
            AIRY_FREE(step->critique);
        if (step->role)
            AIRY_FREE(step->role);
        if (step->depends_on) {
            AIRY_FREE(step->depends_on);
        }
        if (step->dependents)
            AIRY_FREE(step->dependents);
        if (step->correction_history) {
            for (size_t h = 0; h < step->correction_history_count; h++)
                AIRY_FREE(step->correction_history[h]);
            AIRY_FREE(step->correction_history);
        }
        AIRY_FREE(step);
        chain->steps[i] = NULL;
    }

    chain->step_count = target_steps;
    chain->next_step_id = (uint32_t)target_steps + 1;

    AIRY_LOG_INFO("TC Rollback to cp#%u: removed %zu steps (now %zu)", checkpoint_id, removed,
                  chain->step_count);
    return removed;
}
