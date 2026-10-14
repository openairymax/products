/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file round_robin.c
 * @brief 轮询分发策略——按序循环择取候选 agent。
 *
 * M5-4 归位：原 atoms/coreloopthree/src/cognition/think/dispatcher/round_robin.c
 * 迁至 products/cognition（机制留核、策略迁生态层）。内容守恒，仅符号族
 * 统一为 airy_dispatching_*。
 */

#include "agent_registry.h"
#include "cognition.h"
#include "dispatch_strategy.h"

#include <stdlib.h>
#include <string.h>

/* Unified base library compatibility layer */
#include "airy_memory.h"
#include "string_compat.h"
#include "error.h"

/**
 * @brief Internal structure of the round-robin dispatch strategy.
 */
struct airy_round_robin_dispatch {
    void *registry_ctx;
    agent_registry_get_agents_func get_agents;
    size_t last_index;
    airy_mtx_t *lock;
};

/**
 * @brief Destroy a round-robin dispatch strategy instance.
 */
static void rr_destroy(airy_dispatching_strategy_t *strategy)
{
    if (!strategy)
        return;
    struct airy_round_robin_dispatch *rr = (struct airy_round_robin_dispatch *)strategy->data;
    if (rr) {
        airy_mtx_free(rr->lock);
        AIRY_FREE(rr);
    }
    AIRY_FREE(strategy);
}

/**
 * @brief Select the next agent with the round-robin strategy (cyclic selection).
 *
 * @param task [in] Task description
 * @param candidates [in] Candidate agent list
 * @param count [in] Candidate count
 * @param context [in] Strategy context (airy_round_robin_dispatch*)
 * @param out_agent_id [out] Selected agent ID
 * @return AIRY_SUCCESS on success
 */
static airy_err_t rr_select(const airy_task_node_t * task,
                            const void **candidates, size_t count, void *context,
                            char **out_agent_id)
{

    struct airy_round_robin_dispatch *rr = (struct airy_round_robin_dispatch *)context;
    if (!rr || !out_agent_id)
        AIRY_RET_ERR(AIRY_EINVAL);

    agent_info_t **agents = NULL;
    size_t agent_count = 0;
    airy_err_t err;

    if (candidates && count > 0) {
        agents = (agent_info_t **)candidates;
        agent_count = count;
    } else {
        err = rr->get_agents(rr->registry_ctx, NULL, &agents, &agent_count);
        if (err != AIRY_SUCCESS)
            return err;
        if (agent_count == 0)
            AIRY_RET_ERR(AIRY_ENOENT);
    }

    size_t next_index;
    airy_mtx_lock(rr->lock);
    next_index = (rr->last_index + 1) % agent_count;
    rr->last_index = next_index;
    airy_mtx_unlock(rr->lock);

    if (agents[next_index]) {
        *out_agent_id = AIRY_STRDUP(agents[next_index]->agent_id);
        return *out_agent_id ? AIRY_SUCCESS : AIRY_ENOMEM;
    }

    AIRY_RET_ERR(AIRY_ENOENT);
}

/**
 * @brief Create a round-robin dispatch strategy instance.
 *
 * @param registry_ctx [in] Registry context (non-NULL)
 * @param get_agents_func [in] Function pointer to fetch the candidate agent list (non-NULL)
 * @return Strategy object, or NULL on failure
 */
airy_dispatching_strategy_t *airy_dispatching_round_robin_create(
    void *registry_ctx, agent_registry_get_agents_func get_agents_func)
{
    if (!registry_ctx || !get_agents_func) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    struct airy_round_robin_dispatch *rr =
        (struct airy_round_robin_dispatch *)AIRY_CALLOC(1, sizeof(*rr));
    if (!rr) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    rr->registry_ctx = registry_ctx;
    rr->get_agents = get_agents_func;
    rr->last_index = (size_t)-1;
    rr->lock = airy_mtx_create();
    if (!rr->lock) {
        AIRY_FREE(rr);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_dispatching_strategy_t *strategy =
        (airy_dispatching_strategy_t *)AIRY_CALLOC(1, sizeof(*strategy));
    if (!strategy) {
        AIRY_FREE(rr);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    strategy->data = rr;
    strategy->dispatch = rr_select;
    strategy->destroy = rr_destroy;

    return strategy;
}
