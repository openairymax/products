// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file coord_weighted.c
 * @brief Weighted fusion strategy (combine model outputs by weight).
 */

#include "airy_rt.h"
#include "logging_compat.h"
#include "coord_internal.h"

#include <stdlib.h>

/* Unified base library compatibility layer */
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief Weighted fusion private data.
 */
typedef struct weighted_data {
    char **model_names;
    float *weights;
    size_t model_count;
    airy_mtx_t *lock;
} weighted_data_t;

static void weighted_destroy(airy_coordinator_base_t *base)
{
    if (!base)
        return;
    weighted_data_t *data = (weighted_data_t *)base->data;
    if (data) {
        for (size_t i = 0; i < data->model_count; i++) {
            if (data->model_names[i])
                AIRY_FREE(data->model_names[i]);
        }
        AIRY_FREE(data->model_names);
        AIRY_FREE(data->weights);
        if (data->lock)
            airy_mtx_free(data->lock);
        AIRY_FREE(data);
    }
    AIRY_FREE(base);
}

/**
 * @brief Weighted fusion execution.
 *
 * Text outputs cannot be averaged numerically, so use "weighted selection +
 * consistency boost":
 *   1. Normalize weights (sum to 1.0)
 *   2. Pick the primary result by weight (highest-weight model output)
 *   3. Consistency analysis: count models matching the primary result,
 *      compute the consistency ratio
 *   4. Conflict detection: log WARN if a high-weight model output
 *      disagrees with the majority
 *   5. Attach a confidence marker on high consistency, or a conflict
 *      warning on low consistency
 */
static airy_err_t weighted_coordinate(airy_coordinator_base_t *base,
                                      const airy_coordination_context_t *
                                          context,
                                      const char **inputs, size_t input_count, char **out_result)
{
    if (!base || !out_result)
        return AIRY_EINVAL;

    weighted_data_t *data = (weighted_data_t *)base->data;
    if (!data || !inputs || input_count == 0) {
        *out_result = AIRY_STRDUP("invalid_input");
        return AIRY_EINVAL;
    }

    size_t count = input_count < data->model_count ? input_count : data->model_count;
    if (count == 0) {
        *out_result = AIRY_STRDUP("no_models");
        return AIRY_EINVAL;
    }

    float weight_sum = 0.0f;
    for (size_t i = 0; i < count; i++) {
        weight_sum += data->weights[i];
    }
    if (weight_sum <= 0.0f) {

        weight_sum = (float)count;
    }

    float max_norm_weight = 0.0f;
    size_t best_index = 0;
    for (size_t i = 0; i < count; i++) {
        float norm_w = data->weights[i] / weight_sum;
        if (norm_w > max_norm_weight) {
            max_norm_weight = norm_w;
            best_index = i;
        }
    }

    const char *primary = inputs[best_index];
    size_t agree_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (i == best_index)
            continue;
        if (inputs[i] && primary && strcmp(inputs[i], primary) == 0)
            agree_count++;
    }
    float consistency_ratio = (count > 1) ? (float)(agree_count + 1) / (float)count : 1.0f;

    if (count > 2 && consistency_ratio < 0.5f) {
        AIRY_LOG_WARN("weighted_coordinate: high-weight model output conflicts with "
                      "majority (best_idx=%zu weight=%.2f consistency=%.0f%% agree=%zu/%zu)",
                      best_index, (double)max_norm_weight, (double)(consistency_ratio * 100.0f),
                      agree_count + 1, count);
    }

    size_t primary_len = primary ? strlen(primary) : 0;

    size_t out_sz = primary_len + 128;
    char *result = (char *)AIRY_MALLOC(out_sz);
    if (!result)
        return AIRY_ENOMEM;

    int written =
        snprintf(result, out_sz,
                 "{\"result\":\"%.*s\",\"consistency\":%.2f,\"weight\":%.2f,"
                 "\"agree_count\":%zu,\"total\":%zu}",
                 (int)(primary_len > 80 ? 80 : primary_len), primary ? primary : "",
                 (double)consistency_ratio, (double)max_norm_weight, agree_count + 1, count);
    if (written <= 0 || (size_t)written >= out_sz) {

        AIRY_FREE(result);
        *out_result = AIRY_STRDUP(primary);
        if (!*out_result)
            return AIRY_ENOMEM;
        return AIRY_SUCCESS;
    }

    AIRY_LOG_DEBUG("weighted_coordinate: selected model[%zu] (weight=%.2f consistency=%.0f%%)",
                   best_index, (double)max_norm_weight, (double)(consistency_ratio * 100.0f));

    *out_result = result;
    return AIRY_SUCCESS;
}

/**
 * @brief Create a weighted fusion coordinator.
 */
airy_err_t airy_coord_weighted_create(const char **model_names, const float *weights,
                                      size_t model_count, airy_coordinator_base_t **out_base)
{
    if (!out_base || !model_names || !weights || model_count == 0) {
        return AIRY_EINVAL;
    }

    airy_coordinator_base_t *base =
        (airy_coordinator_base_t *)AIRY_CALLOC(1, sizeof(airy_coordinator_base_t));
    if (!base)
        return AIRY_ENOMEM;

    weighted_data_t *data = (weighted_data_t *)AIRY_CALLOC(1, sizeof(weighted_data_t));
    if (!data) {
        AIRY_FREE(base);
        return AIRY_ENOMEM;
    }

    data->model_count = model_count;
    data->lock = airy_mtx_create();
    if (!data->lock) {
        AIRY_FREE(data);
        AIRY_FREE(base);
        return AIRY_ENOMEM;
    }

    data->model_names = (char **)AIRY_CALLOC(model_count, sizeof(char *));
    if (!data->model_names) {
        airy_mtx_free(data->lock);
        AIRY_FREE(data);
        AIRY_FREE(base);
        return AIRY_ENOMEM;
    }

    for (size_t i = 0; i < model_count; i++) {
        data->model_names[i] = AIRY_STRDUP(model_names[i]);
        if (!data->model_names[i]) {
            for (size_t j = 0; j < i; j++) {
                AIRY_FREE(data->model_names[j]);
            }
            AIRY_FREE(data->model_names);
            airy_mtx_free(data->lock);
            AIRY_FREE(data);
            AIRY_FREE(base);
            return AIRY_ENOMEM;
        }
    }

    data->weights = (float *)AIRY_CALLOC(model_count, sizeof(float));
    if (!data->weights) {
        for (size_t i = 0; i < model_count; i++) {
            AIRY_FREE(data->model_names[i]);
        }
        AIRY_FREE(data->model_names);
        airy_mtx_free(data->lock);
        AIRY_FREE(data);
        AIRY_FREE(base);
        return AIRY_ENOMEM;
    }

    __builtin_memcpy(data->weights, weights, model_count * sizeof(float));

    base->data = data;
    base->coordinate = weighted_coordinate;
    base->destroy = weighted_destroy;

    *out_base = base;
    return AIRY_SUCCESS;
}
