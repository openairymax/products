// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file coord_majority.c
 * @brief Majority-vote coordinator implementation.
 */

#include "airy_rt.h"
#include "coord_internal.h"

#include <stdlib.h>

/* Unified base library compatibility layer */
#include "airy_memory.h"
#include "string_compat.h"

#include <string.h>

/**
 * @brief Majority-vote coordinator context.
 */
typedef struct majority_coordinator {
    airy_coordinator_base_t base;
    size_t min_voters;
    float threshold;
} majority_coordinator_t;

/**
 * @brief Vote record.
 */
typedef struct vote_record {
    char *result;
    int count;
} vote_record_t;

/**
 * @brief Coordinate execution (majority voting).
 */
static airy_err_t majority_coordinate(airy_coordinator_base_t *base,
                                      const airy_coordination_context_t *
                                          context,
                                      const char **inputs, size_t input_count, char **out_result)
{
    if (!base || !out_result) {
        return AIRY_EINVAL;
    }

    majority_coordinator_t *coordinator = (majority_coordinator_t *)base;

    if (!inputs || input_count < coordinator->min_voters) {
        *out_result = AIRY_STRDUP("insufficient_voters");
        if (!*out_result)
            return AIRY_ENOMEM;
        return AIRY_SUCCESS;
    }

    if (input_count == 0) {
        *out_result = AIRY_STRDUP("no_votes");
        if (!*out_result)
            return AIRY_ENOMEM;
        return AIRY_SUCCESS;
    }

    vote_record_t *votes = (vote_record_t *)AIRY_CALLOC(input_count, sizeof(vote_record_t));
    if (!votes)
        return AIRY_ENOMEM;

    size_t unique_count = 0;

    for (size_t i = 0; i < input_count; i++) {
        if (!inputs[i])
            continue;

        int found = 0;
        for (size_t j = 0; j < unique_count; j++) {
            if (votes[j].result && strcmp(votes[j].result, inputs[i]) == 0) {
                votes[j].count++;
                found = 1;
                break;
            }
        }

        if (!found) {
            votes[unique_count].result = AIRY_STRDUP(inputs[i]);
            votes[unique_count].count = 1;
            unique_count++;
        }
    }

    char *best_result = NULL;
    int max_votes = 0;

    for (size_t i = 0; i < unique_count; i++) {
        if (votes[i].count > max_votes) {
            if (best_result)
                AIRY_FREE(best_result);
            best_result = AIRY_STRDUP(votes[i].result);
            if (!best_result) {
                for (size_t j = 0; j < unique_count; j++)
                    AIRY_FREE(votes[j].result);
                AIRY_FREE(votes);
                return AIRY_ENOMEM;
            }
            max_votes = votes[i].count;
        }
    }

    float vote_ratio = (float)max_votes / (float)input_count;
    if (vote_ratio >= coordinator->threshold) {
        *out_result = best_result;
    } else {
        *out_result = AIRY_STRDUP("no_majority");
        if (!*out_result) {
            for (size_t j = 0; j < unique_count; j++)
                AIRY_FREE(votes[j].result);
            AIRY_FREE(votes);
            return AIRY_ENOMEM;
        }
        if (best_result)
            AIRY_FREE(best_result);
    }

    for (size_t i = 0; i < unique_count; i++) {
        if (votes[i].result)
            AIRY_FREE(votes[i].result);
    }
    AIRY_FREE(votes);

    return AIRY_SUCCESS;
}

/**
 * @brief Destroy the coordinator.
 */
static void majority_destroy(airy_coordinator_base_t *base)
{
    if (!base)
        return;
    AIRY_FREE(base);
}

/**
 * @brief Create a majority-vote coordinator.
 */
airy_err_t airy_coord_majority_create(size_t min_voters, float threshold,
                                      airy_coordinator_base_t **out_base)
{
    if (!out_base)
        return AIRY_EINVAL;

    majority_coordinator_t *coordinator =
        (majority_coordinator_t *)AIRY_CALLOC(1, sizeof(majority_coordinator_t));
    if (!coordinator)
        return AIRY_ENOMEM;

    coordinator->min_voters = min_voters;
    coordinator->threshold = threshold;

    coordinator->base.coordinate = majority_coordinate;
    coordinator->base.destroy = majority_destroy;

    *out_base = &coordinator->base;
    return AIRY_SUCCESS;
}
