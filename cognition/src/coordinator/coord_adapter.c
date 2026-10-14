// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file coord_adapter.c
 * @brief Coordinator public API adapter - bridges internal base_t to the
 * public strategy_t.
 */

#include "airy_rt.h"
#include "airy_memory.h"
#include "coord_internal.h"
#include "coord_strategy.h"
#include "string_compat.h"

#include <stdlib.h>
#include <string.h>
#include "error.h"

typedef struct {
    airy_coordinator_base_t *base;
} strategy_adapter_data_t;

static airy_err_t adapter_coordinate(const char **prompts, size_t count, void *context,
                                     char **out_result)
{
    if (!context || !out_result)
        AIRY_RET_ERR(AIRY_EINVAL);

    airy_coordinator_strategy_t *strategy = (airy_coordinator_strategy_t *)context;
    strategy_adapter_data_t *adapter = (strategy_adapter_data_t *)strategy->data;
    if (!adapter || !adapter->base || !adapter->base->coordinate)
        AIRY_RET_ERR(AIRY_EINVAL);

    airy_coordination_context_t ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx));

    return adapter->base->coordinate(adapter->base, &ctx, prompts, count, out_result);
}

static void adapter_destroy(airy_coordinator_strategy_t *strategy)
{
    if (!strategy)
        return;
    strategy_adapter_data_t *adapter = (strategy_adapter_data_t *)strategy->data;
    if (adapter) {
        if (adapter->base && adapter->base->destroy) {
            adapter->base->destroy(adapter->base);
        }
        AIRY_FREE(adapter);
    }
    AIRY_FREE(strategy);
}

static airy_coordinator_strategy_t *wrap_base_to_strategy(airy_coordinator_base_t *base)
{
    if (!base)
        return NULL;

    airy_coordinator_strategy_t *strategy =
        (airy_coordinator_strategy_t *)AIRY_CALLOC(1, sizeof(airy_coordinator_strategy_t));
    if (!strategy) {
        if (base->destroy)
            base->destroy(base);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    strategy_adapter_data_t *adapter =
        (strategy_adapter_data_t *)AIRY_CALLOC(1, sizeof(strategy_adapter_data_t));
    if (!adapter) {
        if (base->destroy)
            base->destroy(base);
        AIRY_FREE(strategy);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    adapter->base = base;
    strategy->coordinate = adapter_coordinate;
    strategy->destroy = adapter_destroy;
    strategy->data = adapter;

    return strategy;
}

airy_coordinator_strategy_t *airy_dmc_create(const char *primary_model, const char *secondary1,
                                             const char *secondary2, llm_service_t *llm)
{

    airy_coordinator_base_t *base = NULL;

    if (secondary2 && secondary2[0]) {
        const char *model_names[3] = {primary_model, secondary1, secondary2};
        float weights[3] = {0.5f, 0.3f, 0.2f};
        airy_err_t err = airy_coord_weighted_create(model_names, weights, 3, &base);
        if (err != AIRY_SUCCESS || !base)
            return NULL;
    } else {
        airy_err_t err = airy_coord_dual_create(primary_model, secondary1, 0.7f, 0.3f, &base);
        if (err != AIRY_SUCCESS || !base)
            return NULL;
    }

    base->llm = llm;

    return wrap_base_to_strategy(base);
}

airy_coordinator_strategy_t *airy_mcoord_create(const char **model_names, size_t model_count,
                                                llm_service_t *llm)
{

    airy_coordinator_base_t *base = NULL;
    airy_err_t err = airy_coord_majority_create(model_count, 0.5f, &base);
    if (err != AIRY_SUCCESS || !base)
        return NULL;

    base->llm = llm;

    return wrap_base_to_strategy(base);
}

airy_coordinator_strategy_t *airy_wcoord_create(const char **model_names, const float *weights,
                                                size_t model_count, llm_service_t *llm)
{
    if (!llm || !model_names || !weights || model_count == 0)
        return NULL;

    airy_coordinator_base_t *base = NULL;
    airy_err_t err = airy_coord_weighted_create(model_names, weights, model_count, &base);
    if (err != AIRY_SUCCESS || !base)
        return NULL;

    return wrap_base_to_strategy(base);
}

airy_coordinator_strategy_t *airy_arbiter_model_create(const char *arbiter_model,
                                                       llm_service_t *llm)
{
    if (!llm || !arbiter_model)
        return NULL;

    airy_coordinator_base_t *base = NULL;
    airy_err_t err = airy_coord_arbiter_create(arbiter_model, NULL, &base);
    if (err != AIRY_SUCCESS || !base)
        return NULL;

    /* P2.7: inject the LLM handle into base->llm so arbiter_coordinate can
     * call the LLM through the injected ops table. */
    base->llm = llm;

    return wrap_base_to_strategy(base);
}

airy_coordinator_strategy_t *airy_arbiter_human_create(void (*callback)(const char *question,
                                                                        char *answer,
                                                                        size_t max_len))
{
    airy_coordinator_base_t *base = NULL;
    airy_err_t err = airy_coord_arbiter_create(NULL, callback, &base);
    if (err != AIRY_SUCCESS || !base)
        return NULL;

    return wrap_base_to_strategy(base);
}
