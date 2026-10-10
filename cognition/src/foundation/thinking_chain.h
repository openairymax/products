/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file thinking_chain.h
 * @brief TC 策略载荷内部头——宿主对象布局与全量实现 API（products 侧）。
 *
 * 机制/策略分离（0.1.19 M5-4）：机制核消费面（不透明句柄、状态枚举、
 * step 值载体、monitor/recovery 值载体、17 项 ops 表）已收口至契约头
 * tc.h（atoms/coreloopthree/include）；本头文件是策略载荷的**内部头**
 * ——chain/context_window/working_memory 三个宿主对象的布局、注意力
 * 配置与全量实现 API 仅限载荷实现单元（foundation/）触达，机制侧不得
 * include 本头。
 *
 * Design basis: AgentRT Thinkdual comprehensive design.
 * - Phase 0: instruction decomposition (S1) → Context Window initialization
 * - Phase 2: execution-verification loop (streaming critique) → thinking step chaining
 * - Working Memory: short-term context cache supporting cross-step info passing
 */

#ifndef AIRY_RT_THINKING_CHAIN_H
#define AIRY_RT_THINKING_CHAIN_H

#include "tc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TC_MAX_TOKENS_DEFAULT 8192
#define TC_CHUNK_SIZE_DEFAULT 15
#define TC_MAX_CORRECTIONS_DEFAULT 3
#define TC_WORKING_MEM_CAPACITY 64
#define TC_MAX_THINKING_STEPS 256
#define TC_SENTENCE_BOUNDARY_CHARS 256

typedef struct {
    float decomposition_weight;
    float planning_weight;
    float generation_weight;
    float verification_weight;
    float audit_weight;
    float alignment_weight;
} tc_attention_weights_t;

#define TC_ATTENTION_DEFAULTS                   \
    {.base_tokens = 8192,                       \
     .weights = {.decomposition_weight = 0.15f, \
                 .planning_weight = 0.20f,      \
                 .generation_weight = 0.30f,    \
                 .verification_weight = 0.15f,  \
                 .audit_weight = 0.12f,         \
                 .alignment_weight = 0.08f},    \
     .pressure_threshold = 0.80f,               \
     .enable_dynamic_adjustment = 1,            \
     .min_step_tokens = 256,                    \
     .max_step_tokens = 4096}

/**
 * @brief Context Window.
 *
 * Manages the token budget of the current reasoning session, using a
 * sliding-window mechanism to keep the most recent N tokens in view.
 */
struct airy_context_window {

    size_t max_tokens;
    size_t used_tokens;
    size_t chunk_size;


    char *buffer;
    size_t buffer_capacity;
    size_t buffer_head;
    size_t buffer_tail;
    size_t buffer_used;

    uint64_t total_tokens_generated;
    uint64_t total_corrections;
    uint32_t total_steps;
    uint32_t completed_steps;

    int enable_dynamic_chunk;
    float low_confidence_threshold;
    float high_confidence_threshold;
    int max_corrections_per_chunk;
};

/**
 * @brief Working Memory.
 *
 * Short-term memory cache storing intermediate results of the current
 * reasoning process. Supports key-based access for cross-step info
 * passing. Corresponds to the "Working Memory" component in the Thinkdual
 * design document.
 */
struct airy_working_memory {

    struct wm_entry {
        char *key;
        void *value;
        size_t value_size;
        char *type;
        uint64_t created_ns;
        uint64_t last_accessed_ns;
        uint32_t access_count;
        int pinned;
    } *entries;
    size_t capacity;
    size_t count;


    uint32_t *lru_order;
    size_t lru_index;

    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
};

typedef struct {
    size_t base_tokens;
    tc_attention_weights_t weights;
    float pressure_threshold;
    int enable_dynamic_adjustment;
    uint32_t min_step_tokens;
    uint32_t max_step_tokens;
} tc_attention_config_t;

/**
 * @brief Thinking chain (complete structure).
 *
 * Integrates Context Window + Thinking Steps + Working Memory into a
 * unified Thinkdual reasoning pipeline.
 */
struct airy_thinking_chain {
    uint64_t session_id;
    char *session_goal;
    airy_context_window_t *ctx_window; /**< Context Window */
    airy_working_memory_t *working_mem; /**< Working Memory */
    airy_memory_engine_t *memory;


    airy_thinking_step_t **steps; /**< step pointer array (each step is allocated
                                       independently; realloc grows only the pointer
                                       array, so step object addresses stay stable and
                                       external step pointers never dangle) */
    size_t step_capacity;
    size_t step_count;
    uint32_t next_step_id;


    int active;
    uint64_t created_ns;
    uint64_t last_activity_ns;

    void (*on_step_completed)(airy_thinking_step_t *step, void *user_data);
    void (*on_correction)(airy_thinking_step_t *step, const char *critique, void *user_data);
    void *callback_user_data;


    tc_attention_config_t attention_config;
    int attention_configured;
};

/* ==================== Context Window API ==================== */
/**
 * @brief Create a Context Window.
 * @param max_tokens Maximum token capacity (0 uses default 8192)
 * @param out_window Output handle
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_tc_context_window_create(size_t max_tokens,
                                                  airy_context_window_t **out_window);

/**
 * @brief Destroy a Context Window.
 */
AIRY_API void airy_tc_context_window_destroy(airy_context_window_t *window);

/**
 * @brief Append content to the Context Window.
 * @return Estimated total token count after appending, or an error code (<0)
 */
AIRY_API ssize_t airy_tc_context_window_append(airy_context_window_t *window, const char *data,
                                               size_t len);

/**
 * @brief Get the most recent N tokens of the Context Window.
 * @param token_count Requested token count (0 = all)
 * @param out_data Output data (caller frees)
 * @param out_len Output length
 */
AIRY_API airy_err_t airy_tc_context_window_get_recent(airy_context_window_t *window,
                                                      size_t token_count, char **out_data,
                                                      size_t *out_len);

/**
 * @brief Check whether the Context Window has enough space.
 * @param needed_tokens Tokens needed
 * @return 1=has space, 0=insufficient space
 */
AIRY_API int airy_tc_context_window_has_space(airy_context_window_t *window, size_t needed_tokens);

/**
 * @brief Get Context Window statistics.
 */
AIRY_API airy_err_t airy_tc_context_window_stats(airy_context_window_t *window, char **out_json);

/* ==================== Working Memory API ==================== */
/**
 * @brief Create a Working Memory.
 * @param capacity Maximum entry count (0 uses default 64)
 */
AIRY_API airy_err_t airy_tc_working_memory_create(size_t capacity, airy_working_memory_t **out_mem);

/**
 * @brief Destroy a Working Memory.
 */
AIRY_API void airy_tc_working_memory_destroy(airy_working_memory_t *mem);

/**
 * @brief Store a key-value pair into the Working Memory.
 * @param key Key name (must not be NULL)
 * @param value Value data (copied)
 * @param value_size Value size
 * @param type Type tag (may be NULL)
 * @param pin Whether to pin against LRU eviction
 */
AIRY_API airy_err_t airy_tc_working_memory_store(airy_working_memory_t *mem, const char *key,
                                                 const void *value, size_t value_size,
                                                 const char *type, int pin);

/**
 * @brief Retrieve a value from the Working Memory.
 * @param key Key name
 * @param out_value Output value (caller does not free; invalid after next store/destroy)
 * @param out_size Output size
 * @return AIRY_SUCCESS or AIRY_ENOTFOUND
 */
AIRY_API airy_err_t airy_tc_working_memory_retrieve(airy_working_memory_t *mem, const char *key,
                                                    void **out_value, size_t *out_size);

/**
 * @brief Remove an entry from the Working Memory.
 */
AIRY_API airy_err_t airy_tc_working_memory_remove(airy_working_memory_t *mem, const char *key);

/**
 * @brief Clear all unpinned entries of the Working Memory.
 */
AIRY_API void airy_tc_working_memory_clear_unpinned(airy_working_memory_t *mem);

/* ==================== Thinking Step API ==================== */
/**
 * @brief Create a new thinking step.
 * @param chain Owning thinking chain
 * @param type Step type
 * @param input Input prompt
 * @param input_len Input length
 * @param depends_on Array of dependency step IDs (may be NULL)
 * @param depends_count Dependency count
 * @param out_step Output step pointer (managed by chain)
 */
AIRY_API airy_err_t airy_tc_step_create(airy_thinking_chain_t *chain, tc_step_type_t type,
                                        const char *input, size_t input_len,
                                        const uint32_t *depends_on, size_t depends_count,
                                        airy_thinking_step_t **out_step);

/**
 * @brief Mark a step completed and set its output content.
 * @param step Step pointer
 * @param content Output content (copied)
 * @param content_len Content length
 * @param confidence Confidence (0.0-1.0)
 * @param role Executor role tag
 */
AIRY_API airy_err_t airy_tc_step_complete(airy_thinking_step_t *step, const char *content,
                                          size_t content_len, float confidence, const char *role);

/**
 * @brief Run S1 verification on a step (streaming critique).
 * @param step Step to verify
 * @param is_valid Verification result (true=pass)
 * @param critique Critique text (NULL means no critique)
 * @param critique_len Critique length
 */
AIRY_API airy_err_t airy_tc_step_verify(airy_thinking_step_t *step, int *is_valid,
                                        const char *critique, size_t critique_len);

/**
 * @brief Apply a correction to a step.
 * @param step Step to correct
 * @param corrected_content Corrected content
 * @param corrected_len Content length
 */
AIRY_API airy_err_t airy_tc_step_correct(airy_thinking_step_t *step, const char *corrected_content,
                                         size_t corrected_len);

/**
 * @brief Check step executability (all dependencies completed).
 * @return 1=executable, 0=has unfinished dependencies, -1=error
 */
AIRY_API int airy_tc_step_is_ready(const airy_thinking_step_t *step,
                                   const airy_thinking_chain_t *chain);

/* ==================== Thinking Chain API ==================== */
/**
 * @brief Create a thinking chain instance.
 * @param goal Session goal description
 * @param max_tokens Context Window max tokens (0 = default)
 * @param wm_capacity Working Memory capacity (0 = default)
 * @param out_chain Output handle
 */
AIRY_API airy_err_t airy_tc_chain_create(const char *goal, size_t max_tokens, size_t wm_capacity,
                                         airy_thinking_chain_t **out_chain);

/**
 * @brief Destroy the thinking chain and all its sub-components.
 */
AIRY_API void airy_tc_chain_destroy(airy_thinking_chain_t *chain);

/**
 * @brief Start the thinking chain (mark active, initialize stats).
 */
AIRY_API airy_err_t airy_tc_chain_start(airy_thinking_chain_t *chain);

/**
 * @brief Stop the thinking chain.
 */
AIRY_API void airy_tc_chain_stop(airy_thinking_chain_t *chain);

/**
 * @brief Get the next executable thinking step.
 * @return AIRY_SUCCESS with a step, AIRY_ENOENT (no executable step), or other error
 */
AIRY_API airy_err_t airy_tc_chain_next_ready_step(airy_thinking_chain_t *chain,
                                                  airy_thinking_step_t **out_step);

/**
 * @brief Get the thinking chain's full execution statistics (JSON format).
 */
AIRY_API airy_err_t airy_tc_chain_stats(airy_thinking_chain_t *chain, char **out_json,
                                        size_t *out_len);

/**
 * @brief Set the step-completion callbacks.
 */
AIRY_API void airy_tc_chain_set_step_callback(
    airy_thinking_chain_t *chain, void (*on_step_completed)(airy_thinking_step_t *, void *),
    void (*on_correction)(airy_thinking_step_t *, const char *, void *), void *user_data);


AIRY_API void airy_tc_chain_set_memory(airy_thinking_chain_t *chain, airy_memory_engine_t *memory);

AIRY_API airy_err_t airy_tc_context_window_prepop(airy_thinking_chain_t *chain,
                                                  const char *query_text, size_t query_len,
                                                  uint32_t limit);

AIRY_API airy_err_t airy_tc_working_memory_sync_to_persistent(airy_thinking_chain_t *chain,
                                                              float min_importance);

AIRY_API airy_err_t airy_tc_step_write_to_memory(airy_thinking_chain_t *chain,
                                                 airy_thinking_step_t *step);

AIRY_API airy_err_t airy_tc_meta_inform_mem(airy_thinking_chain_t *chain, const void *eval,
                                            airy_thinking_step_t *step);

#define TC_MONITOR_DEFAULTS             \
    {.default_timeout_ms = 30000,       \
     .min_output_chars = 10,            \
     .max_output_chars = 100000,        \
     .repetition_threshold = 0.7f,      \
     .confidence_drop_threshold = 0.3f, \
     .enable_quality_gate = 1,          \
     .quality_gate_threshold = 0.5f}

/**
 * @brief Monitor a single thinking step's execution state.
 *
 * Core function: checks for anomalies such as timeout, empty
 * output, repetition, and confidence drops.
 *
 * @param step Step to monitor
 * @param config Monitor config (NULL uses defaults)
 * @param out_result Output monitor result
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_tc_step_monitor(const airy_thinking_step_t *step,
                                         const tc_monitor_config_t *config,
                                         tc_monitor_result_t *out_result);

/**
 * @brief Run a health check over the whole chain.
 *
 * Scans all completed/failed steps and aggregates anomalies.
 *
 * @param chain Thinking chain
 * @param out_anomaly_count Output anomaly count
 * @param out_has_critical Whether critical anomalies exist
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_tc_chain_health_check(const airy_thinking_chain_t *chain,
                                               size_t *out_anomaly_count, int *out_has_critical);

/**
 * @brief Automatically recover a failed thinking step.
 *
 * Core function: when a step fails or is flagged anomalous, try
 * multiple recovery strategies by priority:
 * 1. Retry (max 3 times, exponential backoff)
 * 2. Retry with hint (inject contextual hints)
 * 3. Degrade (accept lower-quality output)
 * 4. Roll back to the last known-good state
 *
 * @param chain Thinking chain
 * @param failed_step Failed step
 * @param monitor_result Monitor result (may be NULL)
 * @param corrector_fn Correction callback (regenerates on retry)
 * @param user_data User data passed to corrector_fn
 * @param out_result Output recovery result
 * @return AIRY_SUCCESS or an error code
 */
AIRY_API airy_err_t airy_tc_step_recover(airy_thinking_chain_t *chain,
                                         airy_thinking_step_t *failed_step,
                                         const tc_monitor_result_t *monitor_result,
                                         airy_err_t (*corrector_fn)(const char *, size_t, char **,
                                                                    size_t *, void *),
                                         void *user_data, tc_recovery_result_t *out_result);

/**
 * @brief Create a recovery checkpoint (save current state snapshot).
 *
 * Called after key steps complete, for later rollback.
 *
 * @param chain Thinking chain
 * @return Checkpoint ID (>0), or 0 on failure
 */
AIRY_API uint32_t airy_tc_chain_checkpoint(airy_thinking_chain_t *chain);

/**
 * @brief Roll back to the specified checkpoint.
 *
 * @param chain Thinking chain
 * @param checkpoint_id Checkpoint ID
 * @return Number of steps successfully removed
 */
AIRY_API size_t airy_tc_chain_rollback(airy_thinking_chain_t *chain, uint32_t checkpoint_id);


typedef struct {
    size_t allocated_tokens;
    float priority_score;
    float urgency_score;
    int is_elevated;
} tc_allocation_result_t;

AIRY_API airy_err_t airy_tc_set_attention_config(airy_thinking_chain_t *chain,
                                                 const tc_attention_config_t *config);

AIRY_API airy_err_t airy_tc_allocate_attention(airy_thinking_chain_t *chain,
                                               airy_thinking_step_t *step,
                                               tc_allocation_result_t *out_alloc);

AIRY_API airy_err_t airy_tc_adjust_dynamic_budget(airy_thinking_chain_t *chain,
                                                  tc_step_type_t step_type,
                                                  float performance_score);

AIRY_API float airy_tc_compute_priority(const airy_thinking_step_t *step,
                                        const airy_thinking_chain_t *chain);

AIRY_API airy_err_t airy_tc_wm_set_priority(airy_working_memory_t *wm, const char *key,
                                            float priority);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_THINKING_CHAIN_H */
