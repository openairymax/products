/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file priority.c
 * @brief 优先级分发策略——择取 priority 最高的候选 agent。
 *
 * M5-4 归位：原 atoms/coreloopthree/src/cognition/think/dispatcher/priority.c
 * 迁至 products/cognition（机制留核、策略迁生态层）。内容守恒，仅符号族
 * 统一为 airy_dispatching_*。
 */

#include "agent_registry.h"
#include "cognition.h"
#include "dispatch_strategy.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Unified base library compatibility layer */
#include "airy_memory.h"
#include "string_compat.h"

/**
 * @brief Internal structure of the priority dispatch strategy.
 */
struct airy_priority_dispatch {
    void *registry_ctx;
    agent_registry_get_agents_func get_agents;
};

/**
 * @brief Destroy a priority dispatch strategy instance.
 */
static void priority_destroy(airy_dispatching_strategy_t *strategy)
{
    if (!strategy)
        return;
    struct airy_priority_dispatch *priority = (struct airy_priority_dispatch *)strategy->data;
    if (priority)
        AIRY_FREE(priority);
    AIRY_FREE(strategy);
}

/**
 * @brief Select the best agent with the priority strategy (highest priority).
 *
 * @param task [in] Task description
 * @param candidates [in] Candidate agent list
 * @param count [in] Candidate count
 * @param context [in] Strategy context (airy_priority_dispatch*)
 * @param out_agent_id [out] Selected agent ID
 * @return AIRY_SUCCESS on success
 */
static airy_err_t priority_select(const airy_task_node_t * task,
                                  const void **candidates, size_t count, void *context,
                                  char **out_agent_id)
{

    struct airy_priority_dispatch *priority = (struct airy_priority_dispatch *)context;
    if (!priority || !out_agent_id)
        return AIRY_EINVAL;

    agent_info_t **agents = NULL;
    size_t agent_count = 0;
    airy_err_t err;

    if (candidates && count > 0) {
        agents = (agent_info_t **)candidates;
        agent_count = count;
    } else {
        err = priority->get_agents(priority->registry_ctx, NULL, &agents, &agent_count);
        if (err != AIRY_SUCCESS)
            return err;
        if (agent_count == 0)
            return AIRY_ENOENT;
    }

    int best_index = -1;
    int best_priority = INT_MIN;

    for (size_t i = 0; i < agent_count; i++) {
        agent_info_t *agent = agents[i];
        if (!agent)
            continue;
        if (agent->priority > best_priority) {
            best_priority = agent->priority;
            best_index = (int)i;
        }
    }

    if (best_index >= 0 && agents[best_index]) {
        *out_agent_id = AIRY_STRDUP(agents[best_index]->agent_id);
        return *out_agent_id ? AIRY_SUCCESS : AIRY_ENOMEM;
    }

    return AIRY_ENOENT;
}

/**
 * @brief Create a priority dispatch strategy instance.
 *
 * @param registry_ctx [in] Registry context (non-NULL)
 * @param get_agents_func [in] Function pointer to fetch the candidate agent list (non-NULL)
 * @param out_strategy [out] Output strategy instance
 * @return AIRY_SUCCESS on success
 * @return AIRY_EINVAL invalid arguments
 * @return AIRY_ENOMEM allocation failure
 */
airy_err_t airy_dispatching_priority_create(void *registry_ctx,
                                            agent_registry_get_agents_func get_agents_func,
                                            airy_dispatching_strategy_t **out_strategy)
{
    if (!registry_ctx || !get_agents_func || !out_strategy) {
        return AIRY_EINVAL;
    }

    struct airy_priority_dispatch *priority =
        (struct airy_priority_dispatch *)AIRY_CALLOC(1, sizeof(*priority));
    if (!priority) {
        return AIRY_ENOMEM;
    }

    priority->registry_ctx = registry_ctx;
    priority->get_agents = get_agents_func;

    airy_dispatching_strategy_t *strategy =
        (airy_dispatching_strategy_t *)AIRY_CALLOC(1, sizeof(*strategy));
    if (!strategy) {
        AIRY_FREE(priority);
        return AIRY_ENOMEM;
    }

    strategy->data = priority;
    strategy->dispatch = priority_select;
    strategy->destroy = priority_destroy;

    *out_strategy = strategy;
    return AIRY_SUCCESS;
}
