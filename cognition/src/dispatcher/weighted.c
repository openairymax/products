/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file weighted.c
 * @brief 加权分发策略——按成本/性能/信任三因子加权打分择优。
 *
 * M5-4 归位：原 atoms/coreloopthree/src/cognition/think/dispatcher/weighted.c
 * 迁至 products/cognition（机制留核、策略迁生态层）。内容守恒，仅符号族
 * 统一为 airy_dispatching_*，配置类型消歧为 airy_dispatching_weighted_config_t。
 */

#include "agent_registry.h"
#include "airy_rt.h"
#include "cognition.h"
#include "dispatch_strategy.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unified base library compatibility layer */
#include "airy_memory.h"
#include "string_compat.h"
#include "error.h"

/**
 * @brief Internal data structure of the weighted dispatch strategy.
 */
typedef struct weighted_data {
    airy_dispatching_weighted_config_t config;
    void *registry_ctx;
    agent_registry_get_agents_func get_agents;
    airy_mtx_t *lock;
} weighted_data_t;

/**
 * @brief Compute a single agent's combined weighted score.
 *
 * Combines the configured cost, performance, and trust weights.
 *
 * @param agent [in] Agent info pointer
 * @param data [in] Weighted dispatch context
 * @return Combined score (higher is better)
 */
static float compute_weighted_score(const agent_info_t *agent, const weighted_data_t *data)
{
    if (!agent || !data)
        return 0.0f;

    float cost_score = 1.0f;
    float perf_score = 1.0f;
    float trust_score = 1.0f;

    if (agent->cost_estimate > 0) {
        cost_score = 1.0f / (1.0f + agent->cost_estimate);
    }

    perf_score = agent->success_rate;

    trust_score = agent->trust_score;

    float total = data->config.cost_weight * cost_score + data->config.perf_weight * perf_score +
                  data->config.trust_weight * trust_score;

    return total;
}

/**
 * @brief Destroy a weighted dispatch strategy instance.
 * @param strategy [in] Strategy instance to destroy
 */
static void weighted_destroy(airy_dispatching_strategy_t *strategy)
{
    if (!strategy)
        return;
    weighted_data_t *data = (weighted_data_t *)strategy->data;
    if (data) {
        if (data->lock)
            airy_mtx_free(data->lock);
        AIRY_FREE(data);
    }
    AIRY_FREE(strategy);
}

/**
 * @brief Select the best agent with the weighted strategy.
 *
 * @param task [in] Task description
 * @param candidates [in] Candidate agent list
 * @param count [in] Candidate count
 * @param context [in] Strategy context (weighted_data_t*)
 * @param out_agent_id [out] Selected agent ID
 * @return AIRY_SUCCESS on success
 */
static airy_err_t weighted_select(const airy_task_node_t *task, const void **candidates,
                                  size_t count, void *context, char **out_agent_id)
{
    weighted_data_t *data = (weighted_data_t *)context;
    if (!data || !task || !out_agent_id)
        AIRY_RET_ERR(AIRY_EINVAL);

    agent_info_t **agents = NULL;
    size_t agent_count = 0;
    airy_err_t err;

    if (candidates && count > 0) {
        agents = (agent_info_t **)candidates;
        agent_count = count;
    } else {
        err =
            data->get_agents(data->registry_ctx, task->task_node_agent_role, &agents, &agent_count);
        if (err != AIRY_SUCCESS)
            return err;
        if (agent_count == 0)
            AIRY_RET_ERR(AIRY_ENOENT);
    }

    float best_score = -FLT_MAX;
    int best_index = -1;

    for (size_t i = 0; i < agent_count; i++) {
        agent_info_t *agent = agents[i];
        if (!agent)
            continue;
        float score = compute_weighted_score(agent, data);
        if (score > best_score) {
            best_score = score;
            best_index = (int)i;
        }
    }

    if (best_index >= 0 && agents[best_index]) {
        *out_agent_id = AIRY_STRDUP(agents[best_index]->agent_id);
        return *out_agent_id ? AIRY_SUCCESS : AIRY_ENOMEM;
    }

    AIRY_RET_ERR(AIRY_ENOENT);
}

/**
 * @brief Create a weighted dispatch strategy instance.
 *
 * @param config [in] Weight config (defaults used when NULL)
 * @param registry_ctx [in] Registry context
 * @param get_agents_func [in] Function to fetch the candidate agent list
 * @return Strategy object, or NULL on failure
 */
airy_dispatching_strategy_t *airy_dispatching_weighted_create(
    const airy_dispatching_weighted_config_t *config, void *registry_ctx,
    agent_registry_get_agents_func get_agents_func)
{
    if (!registry_ctx || !get_agents_func) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    weighted_data_t *data = (weighted_data_t *)AIRY_CALLOC(1, sizeof(weighted_data_t));
    if (!data)
        return NULL;

    if (config) {
        data->config = *config;
    } else {
        data->config.cost_weight = 0.3f;
        data->config.perf_weight = 0.4f;
        data->config.trust_weight = 0.3f;
    }

    data->registry_ctx = registry_ctx;
    data->get_agents = get_agents_func;
    data->lock = airy_mtx_create();
    if (!data->lock) {
        AIRY_FREE(data);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_dispatching_strategy_t *strategy =
        (airy_dispatching_strategy_t *)AIRY_CALLOC(1, sizeof(*strategy));
    if (!strategy) {
        if (data->lock)
            airy_mtx_free(data->lock);
        AIRY_FREE(data);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    strategy->data = data;
    strategy->dispatch = weighted_select;
    strategy->destroy = weighted_destroy;

    return strategy;
}
