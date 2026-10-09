/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file ml_based.c
 * @brief 机器学习多因子分发策略——按成本/成功率/信任/优先级四维打分，
 *        并依据任务结果在线自适应调整特征权重。
 *
 * M5-4 归位：原 atoms/coreloopthree/src/cognition/think/dispatcher/ml_based.c
 * 迁至 products/cognition（机制留核、策略迁生态层）。内容守恒，仅符号族
 * 统一为 airy_dispatching_*（含在线反馈与平均奖励两个扩展接口）。
 */

#include "airy_rt.h"
#include "cognition.h"
#include "airy_memory.h"
#include "logging_compat.h"
#include "platform.h"
#include "dispatch_strategy.h"
#include "string_compat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "error.h"

#define ML_FEATURE_COUNT 4
#define ML_MAX_HISTORY 256

typedef struct ml_dispatch_data {
    float feature_weights[ML_FEATURE_COUNT];
    float learning_rate;
    void *registry_ctx;
    agent_registry_get_agents_func get_agents_func;
    float reward_history[ML_MAX_HISTORY];
    size_t history_count;
    size_t history_pos;
    uint64_t last_dispatch_time_ns;
    char last_selected_agent_id[128];
} ml_dispatch_data_t;

enum ml_feature_index {
    ML_FEATURE_COST = 0,
    ML_FEATURE_SUCCESS_RATE = 1,
    ML_FEATURE_TRUST = 2,
    ML_FEATURE_PRIORITY = 3
};

static float normalize_cost(float cost)
{
    if (cost <= 0.0f)
        return 1.0f;
    return 1.0f / (1.0f + cost);
}

static float compute_agent_score(ml_dispatch_data_t *data, const agent_info_t *info)
{
    if (!info)
        return 0.0f;

    float features[ML_FEATURE_COUNT];
    features[ML_FEATURE_COST] = normalize_cost(info->cost_estimate);
    features[ML_FEATURE_SUCCESS_RATE] = info->success_rate;
    features[ML_FEATURE_TRUST] = info->trust_score;
    features[ML_FEATURE_PRIORITY] = info->priority > 0 ? (float)info->priority / 10.0f : 0.1f;

    for (size_t i = 0; i < ML_FEATURE_COUNT; i++) {
        if (features[i] < 0.0f)
            features[i] = 0.0f;
        if (features[i] > 1.0f)
            features[i] = 1.0f;
    }

    float score = 0.0f;
    for (size_t i = 0; i < ML_FEATURE_COUNT; i++) {
        score += data->feature_weights[i] * features[i];
    }

    return score;
}

static void ml_update_weights(ml_dispatch_data_t *data, float reward)
{
    float lr = data->learning_rate;
    for (size_t i = 0; i < ML_FEATURE_COUNT; i++) {
        data->feature_weights[i] += lr * reward * data->feature_weights[i] * 0.01f;
        if (data->feature_weights[i] < 0.01f)
            data->feature_weights[i] = 0.01f;
        if (data->feature_weights[i] > 10.0f)
            data->feature_weights[i] = 10.0f;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < ML_FEATURE_COUNT; i++) {
        sum += data->feature_weights[i];
    }
    if (sum > 0.0f) {
        for (size_t i = 0; i < ML_FEATURE_COUNT; i++) {
            data->feature_weights[i] /= sum;
        }
    }
}

static float ml_average_reward(ml_dispatch_data_t *data)
{
    if (data->history_count == 0)
        return 0.5f;
    float sum = 0.0f;
    size_t count = data->history_count < ML_MAX_HISTORY ? data->history_count : ML_MAX_HISTORY;
    for (size_t i = 0; i < count; i++) {
        sum += data->reward_history[i];
    }
    return sum / (float)count;
}

static airy_err_t ml_dispatch(const airy_task_node_t *task, const void **candidates, size_t count,
                              void *context, char **out_agent_id)
{
    if (!context || !out_agent_id)
        AIRY_RET_ERR(AIRY_EINVAL);

    ml_dispatch_data_t *data = (ml_dispatch_data_t *)context;

    if (data->registry_ctx && data->get_agents_func) {
        const char *role_filter = NULL;
        if (task && task->task_node_agent_role) {
            role_filter = task->task_node_agent_role;
        }

        agent_info_t **agents = NULL;
        size_t agent_count = 0;
        airy_err_t err =
            data->get_agents_func(data->registry_ctx, role_filter, &agents, &agent_count);

        if (err == AIRY_SUCCESS && agent_count > 0 && agents) {
            float best_score = -1.0f;
            const char *best_id = NULL;

            for (size_t i = 0; i < agent_count; i++) {
                if (!agents[i])
                    continue;
                float score = compute_agent_score(data, agents[i]);
                if (score > best_score) {
                    best_score = score;
                    best_id = agents[i]->agent_id;
                }
            }

            if (best_id) {
                *out_agent_id = AIRY_STRDUP(best_id);
                snprintf(data->last_selected_agent_id, sizeof(data->last_selected_agent_id), "%s",
                         best_id);
                data->last_dispatch_time_ns = airy_time_ns();
                return AIRY_SUCCESS;
            }
        }
    }

    if (candidates && count > 0) {
        float best_score = -1.0f;
        const char *best_id = NULL;

        for (size_t i = 0; i < count; i++) {
            const agent_info_t *info = (const agent_info_t *)candidates[i];
            if (!info)
                continue;
            float score = compute_agent_score(data, info);
            if (score > best_score) {
                best_score = score;
                best_id = info->agent_id;
            }
        }

        if (best_id) {
            *out_agent_id = AIRY_STRDUP(best_id);
            snprintf(data->last_selected_agent_id, sizeof(data->last_selected_agent_id), "%s",
                     best_id);
            data->last_dispatch_time_ns = airy_time_ns();
            return AIRY_SUCCESS;
        }
    }

    *out_agent_id = NULL;
    AIRY_RET_ERR(AIRY_ENOENT);
}

static void ml_destroy(airy_dispatching_strategy_t *strategy)
{
    if (!strategy)
        return;

    ml_dispatch_data_t *data = (ml_dispatch_data_t *)strategy->data;
    if (data) {
        AIRY_FREE(data);
    }
    AIRY_FREE(strategy);
}

airy_dispatching_strategy_t *airy_dispatching_ml_create(
    const char * model_path, void *registry_ctx,
    agent_registry_get_agents_func get_agents_func)
{

    ml_dispatch_data_t *data = (ml_dispatch_data_t *)AIRY_CALLOC(1, sizeof(ml_dispatch_data_t));
    if (!data)
        return NULL;

    data->feature_weights[ML_FEATURE_COST] = 0.2f;
    data->feature_weights[ML_FEATURE_SUCCESS_RATE] = 0.35f;
    data->feature_weights[ML_FEATURE_TRUST] = 0.25f;
    data->feature_weights[ML_FEATURE_PRIORITY] = 0.2f;
    data->learning_rate = 0.1f;
    data->registry_ctx = registry_ctx;
    data->get_agents_func = get_agents_func;
    data->history_count = 0;
    data->history_pos = 0;
    data->last_dispatch_time_ns = 0;
    __builtin_memset(data->last_selected_agent_id, 0, sizeof(data->last_selected_agent_id));

    if (model_path && model_path[0] != '\0') {
        FILE *f = fopen(model_path, "rb");
        if (f) {
            float loaded_weights[ML_FEATURE_COUNT];
            size_t n = fread(loaded_weights, sizeof(float), ML_FEATURE_COUNT, f);
            fclose(f);
            if (n == ML_FEATURE_COUNT) {
                float sum = 0.0f;
                for (size_t i = 0; i < ML_FEATURE_COUNT; i++) {
                    if (loaded_weights[i] < 0.01f)
                        loaded_weights[i] = 0.01f;
                    if (loaded_weights[i] > 10.0f)
                        loaded_weights[i] = 10.0f;
                    sum += loaded_weights[i];
                }
                if (sum > 0.0f) {
                    for (size_t i = 0; i < ML_FEATURE_COUNT; i++) {
                        data->feature_weights[i] = loaded_weights[i] / sum;
                    }
                }
                AIRY_LOG_INFO("dispatch.ml: Loaded ML weights from %s", model_path);
            } else {
                AIRY_LOG_WARN("dispatch.ml: Invalid ML model file %s, using defaults", model_path);
            }
        } else {
            AIRY_LOG_INFO("dispatch.ml: ML model file not found: %s, using default weights",
                          model_path);
        }
    }

    airy_dispatching_strategy_t *strategy =
        (airy_dispatching_strategy_t *)AIRY_CALLOC(1, sizeof(airy_dispatching_strategy_t));
    if (!strategy) {
        AIRY_FREE(data);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    strategy->dispatch = ml_dispatch;
    strategy->destroy = ml_destroy;
    strategy->data = data;

    return strategy;
}

airy_err_t airy_dispatching_ml_report_outcome(airy_dispatching_strategy_t *strategy, float reward)
{
    if (!strategy || !strategy->data)
        AIRY_RET_ERR(AIRY_EINVAL);

    ml_dispatch_data_t *data = (ml_dispatch_data_t *)strategy->data;
    if (reward < 0.0f)
        reward = 0.0f;
    if (reward > 1.0f)
        reward = 1.0f;

    data->reward_history[data->history_pos] = reward;
    data->history_pos = (data->history_pos + 1) % ML_MAX_HISTORY;
    if (data->history_count < ML_MAX_HISTORY) {
        data->history_count++;
    }

    ml_update_weights(data, reward - ml_average_reward(data));

    return AIRY_SUCCESS;
}

float airy_dispatching_ml_avg_reward(const airy_dispatching_strategy_t *strategy)
{
    if (!strategy || !strategy->data)
        return 0.0f;
    ml_dispatch_data_t *data = (ml_dispatch_data_t *)strategy->data;
    return ml_average_reward(data);
}
