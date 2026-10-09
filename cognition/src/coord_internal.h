/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file coord_internal.h
 * @brief 协调策略内部共享契约（协调策略载荷私有）。
 *
 * 仅在 products/cognition/src/coord_*.c 之间共享：协调上下文与协调器基类
 * 的"继承"语义，以及各具象协调器的基类构造器声明。公共策略工厂契约见
 * include/coord_strategy.h；机制核 atoms/coreloopthree/include/cognition.h
 * 仅提供 airy_coordinator_strategy_t 对象契约。
 */

#ifndef AIRY_RT_COORD_INTERNAL_H
#define AIRY_RT_COORD_INTERNAL_H

#include "cognition.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Coordination context (passed to the coordinate function).
 *
 * Holds metadata and state needed during coordination.
 * Internal structure; not exposed to API users directly.
 */
typedef struct airy_coordination_context {
    uint32_t context_flags;
    void *context_user_data;
    size_t context_timeout_ms;
} airy_coordination_context_t;

/**
 * @brief Coordinator base structure (for internal inheritance).
 *
 * All concrete coordinator implementations build on this base to get
 * "inheritance" semantics. Internal structure; implementation details
 * are hidden from external users.
 */
typedef struct airy_coordinator_base {
    void *data;
    llm_service_t *llm;

    /**
     * @brief Coordinate execution function.
     * @param base Base structure pointer
     * @param context Coordination context
     * @param inputs Input string array (outputs from multiple models)
     * @param input_count Number of inputs
     * @param out_result Coordination result output (caller frees)
     * @return Error code
     */
    airy_err_t (*coordinate)(struct airy_coordinator_base *base,
                             const airy_coordination_context_t *context, const char **inputs,
                             size_t input_count, char **out_result);

    /**
     * @brief Destroy function.
     * @param base Base structure pointer
     */
    void (*destroy)(struct airy_coordinator_base *base);
} airy_coordinator_base_t;

airy_err_t airy_coord_dual_create(const char *primary_model, const char *secondary_model,
                                  float primary_weight, float secondary_weight,
                                  airy_coordinator_base_t **out_base);

airy_err_t airy_coord_majority_create(size_t min_voters, float threshold,
                                      airy_coordinator_base_t **out_base);

airy_err_t airy_coord_weighted_create(const char **model_names, const float *weights,
                                      size_t model_count, airy_coordinator_base_t **out_base);

airy_err_t airy_coord_arbiter_create(const char *arbiter_model,
                                     void (*human_callback)(const char *question, char *answer,
                                                            size_t max_len),
                                     airy_coordinator_base_t **out_base);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_COORD_INTERNAL_H */
