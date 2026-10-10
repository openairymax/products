// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_internal.h
 * @brief Thinking-chain internal shared declarations (internal contract after
 *        domain split; not a public API).
 *
 * thinking_chain.c is split into multiple translation units by
 * responsibility, keeping the public API (airy_tc_* in thinking_chain.h)
 * and ABI completely unchanged:
 *   - thinking_chain.c      core domain: chain orchestration (create/destroy/start/stop/
 *                            next_ready_step/stats/set_step_cb) + time utilities
 *   - tc_context_window.c   Context Window domain: token-budget management and sliding window
 *   - tc_working_memory.c   Working Memory domain: short-term key-value cache with LRU eviction
 *   - tc_step.c             Thinking Step domain: reasoning-step lifecycle and dependency chain
 *   - tc_memory.c           memory integration domain: seven MemoryRovol connection points
 *   - tc_monitor.c          execution-monitoring domain: anomaly detection and chain health check
 *   - tc_recovery.c         anomaly-recovery domain: retry/degrade/rollback and checkpoints
 *   - tc_attention.c        attention-allocation domain: dynamic budget and priority computation
 *
 * Shared time utilities are declared here (defined once in thinking_chain.c)
 * and not promoted to the public API. This header is for internal use by
 * the airy_cognition library only and is not installed with the public
 * include set.
 */

#ifndef AIRY_RT_TC_INTERNAL_H
#define AIRY_RT_TC_INTERNAL_H

#include "thinking_chain.h"

#include "airy_rt.h"
#include "logging.h"
#include "airy_memory.h"
#include "platform.h"
#include "string_compat.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Core domain (thinking_chain.c) */
uint64_t tc_time_now_ns(void);

/* Memory-integration domain (tc_memory.c) and Context Window domain
 * (tc_context_window.c): the three functions below are the implementation
 * names actually called by the engine pipeline (engine_process.c /
 * engine_phase0.c). The declared names in thinking_chain.h differ for
 * historical reasons; callers must use the implementation names declared
 * here to avoid implicit declarations. */
airy_err_t airy_tc_context_window_prepop(airy_thinking_chain_t *chain, const char *query_text,
                                         size_t query_len, uint32_t limit);
airy_err_t airy_tc_context_window_get_recent(airy_context_window_t *window, size_t token_count,
                                         char **out_data, size_t *out_len);
airy_err_t airy_tc_meta_inform_mem(airy_thinking_chain_t *chain, const void *eval,
                                   airy_thinking_step_t *step);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TC_INTERNAL_H */
