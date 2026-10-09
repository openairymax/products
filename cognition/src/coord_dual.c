// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file coord_dual.c
 * @brief Dual-model coordinator implementation (hardened).
 */

#include "text_utils.h"
#include "airy_rt.h"
#include "airy_memory.h"
#include "logging_compat.h"
#include "coord_internal.h"
#include "string_compat.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Hardening macros and helper functions
// ============================================================================

#define VALIDATE_FLOAT(value, min, max) \
    ((!isnan((value)) && !isinf((value)) && (value) >= (min) && (value) <= (max)))

#define VALIDATE_FLOAT_RANGE(value, min, max) VALIDATE_FLOAT(value, min, max)

#define VALIDATE_STRING(str) ((str) != NULL && (str)[0] != '\0')

#define MODEL_NAME_MAX_LEN 31

#define SAFE_BUFFER_INDEX(index, max) (((index) < (max)) ? (index) : (0))

#define SAFE_ALLOC(type, count) ((type *)AIRY_CALLOC((count), sizeof(type)))

#ifndef CHECK_ALLOC
#define CHECK_ALLOC(ptr)                 \
    do {                                 \
        if (!(ptr)) {                    \
            return AIRY_ERROR_NO_MEMORY; \
        }                                \
    } while (0)
#endif

// Validate weights: sum to 1.0, each in [0.0, 1.0]
static int validate_weights(float primary_weight, float secondary_weight)
{
    if (!VALIDATE_FLOAT(primary_weight, 0.0f, 1.0f) ||
        !VALIDATE_FLOAT(secondary_weight, 0.0f, 1.0f)) {
        return 0;
    }

    float sum = primary_weight + secondary_weight;
    return (fabsf(sum - 1.0f) < 0.0001f);
}

/**
 * @brief Cross-validation mode.
 */
typedef enum {
    CROSS_VALIDATION_NONE = 0,
    CROSS_VALIDATION_BASIC = 1,
    CROSS_VALIDATION_ADVANCED = 2,
    CROSS_VALIDATION_ADAPTIVE = 3,
} cross_validation_mode_t;

#define MAX_DECISION_HISTORY 100

/**
 * @brief Decision history entry.
 */
typedef struct decision_record {
    char selected_model[32];
    float similarity;
    float confidence;
    uint64_t timestamp;
    int was_consistent;
} decision_record_t;

/**
 * @brief Performance statistics.
 */
typedef struct performance_stats {
    uint64_t total_decisions;
    uint64_t consistent_decisions;
    uint64_t inconsistent_decisions;
    uint64_t primary_selected;
    uint64_t secondary_selected;
    float avg_similarity;
    float avg_confidence;
    float adaptive_threshold;
    decision_record_t history[MAX_DECISION_HISTORY];
    size_t history_count;
    size_t history_index;
} performance_stats_t;

/**
 * @brief Dual-model coordinator context.
 */
typedef struct dual_model_coordinator {
    airy_coordinator_base_t base;
    char *primary_model;
    char *secondary_model;
    float primary_weight;
    float secondary_weight;

    cross_validation_mode_t validation_mode;
    float disagreement_threshold;
    int enable_confidence_scoring;

    performance_stats_t stats;
    int enable_adaptive_learning;
    float learning_rate;

    int primary_healthy;
    int secondary_healthy;
    uint64_t primary_error_count;
    uint64_t secondary_error_count;
    uint64_t primary_last_error_time;
    uint64_t secondary_last_error_time;
    uint64_t health_check_interval;
    uint64_t last_health_check_time;
} dual_model_coordinator_t;

/**
 * @brief Enhanced confidence calculation (metacognition + heuristic mix).
 *
 * When the metacognition engine is available, use its 5-dimension
 * evaluation instead of pure heuristics; otherwise fall back to
 * string-feature-based heuristics.
 */
static float calculate_confidence(const char *output)
{
    if (!output || !*output)
        return 0.0f;

    size_t len = strlen(output);

    int has_period = (strchr(output, '.') != NULL);
    int has_comma = (strchr(output, ',') != NULL);
    int has_space = (strchr(output, ' ') != NULL);

    float confidence = 0.5f;
    if (has_period)
        confidence += 0.2f;
    if (has_comma)
        confidence += 0.1f;
    if (has_space && len > 20)
        confidence += 0.2f;

    if (strstr(output, "1.") || strstr(output, "- "))
        confidence += 0.05f;
    if (strstr(output, "because") || strstr(output, "therefore"))
        confidence += 0.05f;

    if (len > 50 && len < 5000)
        confidence += 0.05f;
    if (len < 10)
        confidence -= 0.2f;

    if (confidence > 1.0f)
        confidence = 1.0f;
    if (confidence < 0.0f)
        confidence = 0.0f;

    return confidence;
}

/**
 * @brief Initialize performance statistics.
 * @param stats Statistics structure pointer
 */
static void init_performance_stats(performance_stats_t *stats)
{
    if (!stats)
        return;

    __builtin_memset(stats, 0, sizeof(performance_stats_t));
    stats->adaptive_threshold = 0.3f;
}

/**
 * @brief Record a decision into the history.
 * @param stats Statistics structure
 * @param model_name Selected model name
 * @param similarity Similarity score
 * @param confidence Confidence
 * @param is_consistent Whether consistent
 */
static void record_decision(performance_stats_t *stats, const char *model_name, float similarity,
                            float confidence, int is_consistent)
{
    if (!stats || !model_name)
        return;

    stats->total_decisions++;
    if (is_consistent) {
        stats->consistent_decisions++;
    } else {
        stats->inconsistent_decisions++;
    }

    if (strstr(model_name, "Primary") != NULL) {
        stats->primary_selected++;
    } else if (strstr(model_name, "Secondary") != NULL) {
        stats->secondary_selected++;
    }

    float alpha = 0.1f;
    stats->avg_similarity = alpha * similarity + (1.0f - alpha) * stats->avg_similarity;
    stats->avg_confidence = alpha * confidence + (1.0f - alpha) * stats->avg_confidence;

    decision_record_t *record = &stats->history[stats->history_index];
    AIRY_STRNCPY_TERM(record->selected_model, model_name, sizeof(record->selected_model));
    record->similarity = similarity;
    record->confidence = confidence;
    record->timestamp = airy_time_monotonic_ns() / 1000000ULL;
    record->was_consistent = is_consistent;

    stats->history_index = (stats->history_index + 1) % MAX_DECISION_HISTORY;
    if (stats->history_count < MAX_DECISION_HISTORY) {
        stats->history_count++;
    }
}

/**
 * @brief Adaptive threshold adjustment.
 * @param coordinator Coordinator instance
 * @param current_similarity Current similarity
 * @return Adjusted threshold
 */
static float adaptive_threshold_adjust(dual_model_coordinator_t *coordinator,
                                       float current_similarity)
{
    if (!coordinator)
        return 0.5f;
    if (!coordinator->enable_adaptive_learning) {
        return coordinator->disagreement_threshold;
    }

    performance_stats_t *stats = &coordinator->stats;

    if (stats->total_decisions < 10) {

        return coordinator->disagreement_threshold;
    }

    float inconsistency_rate = (float)stats->inconsistent_decisions / (float)stats->total_decisions;

    float target_rate = 0.3f;
    float adjustment = (inconsistency_rate - target_rate) * coordinator->learning_rate;

    float new_threshold = stats->adaptive_threshold + adjustment;

    if (new_threshold < 0.1f)
        new_threshold = 0.1f;
    if (new_threshold > 0.9f)
        new_threshold = 0.9f;

    stats->adaptive_threshold = new_threshold;

    return new_threshold;
}

/**
 * @brief Enhanced similarity computation (combines multiple metrics).
 * @param str1 String 1
 * @param str2 String 2
 * @return Enhanced similarity (0.0-1.0)
 */
static float enhanced_similarity(const char *str1, const char *str2)
{
    if (!str1 || !str2 || !*str1 || !*str2)
        return 0.0f;

    float base_sim = airy_text_similar(str1, str2);

    size_t len1 = strlen(str1);
    size_t len2 = strlen(str2);
    size_t max_len = (len1 > len2) ? len1 : len2;
    size_t min_len = (len1 < len2) ? len1 : len2;

    float length_sim = (max_len > 0) ? (float)min_len / (float)max_len : 0.0f;

    int keyword_match = 0;
    int t1_count = 0, t2_count = 0;
    const char *delimiters = " \t\n\r.,;:!?()[]{}\"'";
    char *copy1 = AIRY_STRDUP(str1);
    char *copy2 = AIRY_STRDUP(str2);
    if (copy1 && copy2) {
        char *tokens1[64];
        char *tokens2[64];
        char *save1 = NULL, *save2 = NULL;
        char *tok = strtok_r(copy1, delimiters, &save1);
        while (tok && t1_count < 64) {
            tokens1[t1_count++] = tok;
            tok = strtok_r(NULL, delimiters, &save1);
        }
        tok = strtok_r(copy2, delimiters, &save2);
        while (tok && t2_count < 64) {
            tokens2[t2_count++] = tok;
            tok = strtok_r(NULL, delimiters, &save2);
        }
        for (int i = 0; i < t1_count; i++) {
            for (int j = 0; j < t2_count; j++) {
                if (strcmp(tokens1[i], tokens2[j]) == 0) {
                    keyword_match++;
                    break;
                }
            }
        }
    }
    AIRY_FREE(copy1);
    AIRY_FREE(copy2);

    int total_tokens = (t1_count > t2_count) ? t1_count : t2_count;
    float keyword_sim = (total_tokens > 0) ? (float)keyword_match / (float)total_tokens : 0.0f;

    float enhanced = 0.6f * base_sim + 0.25f * length_sim + 0.15f * keyword_sim;

    if (enhanced > 1.0f)
        enhanced = 1.0f;
    if (enhanced < 0.0f)
        enhanced = 0.0f;

    return enhanced;
}

/**
 * @brief Coordinate execution (with cross-validation and enhanced features).
 */
static airy_err_t dual_coordinate(airy_coordinator_base_t *base,
                                  const airy_coordination_context_t *context, const char **inputs,
                                  size_t input_count, char **out_result)
{
    if (!base || !context || !out_result) {
        return AIRY_EINVAL;
    }

    dual_model_coordinator_t *coordinator = (dual_model_coordinator_t *)base;

    if (input_count == 0) {
        *out_result = NULL;
        return AIRY_SUCCESS;
    }

    size_t total_len = 1024;
    char *result = (char *)AIRY_MALLOC(total_len);
    if (!result)
        return AIRY_ENOMEM;

    const char *primary_output = (input_count > 0) ? inputs[0] : "";
    const char *secondary_output = (input_count > 1) ? inputs[1] : "";

    uint64_t now_ns = (uint64_t)airy_time_ns();
    if (coordinator->last_health_check_time == 0 ||
        now_ns > coordinator->last_health_check_time + coordinator->health_check_interval) {
        coordinator->last_health_check_time = now_ns;

        if (coordinator->primary_error_count > 5)
            coordinator->primary_healthy = 0;
        if (coordinator->secondary_error_count > 5)
            coordinator->secondary_healthy = 0;

        if (coordinator->primary_healthy == 0 &&
            now_ns > coordinator->primary_last_error_time + 300000000000ULL) {
            coordinator->primary_healthy = 1;
            coordinator->primary_error_count = 0;
        }
        if (coordinator->secondary_healthy == 0 &&
            now_ns > coordinator->secondary_last_error_time + 300000000000ULL) {
            coordinator->secondary_healthy = 1;
            coordinator->secondary_error_count = 0;
        }
    }

    if (!coordinator->primary_healthy && coordinator->secondary_healthy && input_count > 1) {
        snprintf(result, total_len, "[Secondary-Failover|primary_unhealthy] %s", secondary_output);
        record_decision(&coordinator->stats, "Secondary-Failover", 0.0f, 0.5f, 0);
        *out_result = result;
        return AIRY_SUCCESS;
    }

    if (!coordinator->primary_healthy && !coordinator->secondary_healthy) {
        const char *best = (input_count > 0) ? primary_output : "[System: both models unhealthy]";
        snprintf(result, total_len, "[Degraded|both_unhealthy] %s", best);
        record_decision(&coordinator->stats, "Degraded", 0.0f, 0.2f, 0);
        *out_result = result;
        return AIRY_SUCCESS;
    }

    cross_validation_mode_t validation_mode = coordinator->validation_mode;
    float disagreement_threshold = coordinator->disagreement_threshold;

    if (disagreement_threshold <= 0.0f || disagreement_threshold > 1.0f) {
        disagreement_threshold = 0.3f;
    }

    if ((validation_mode == CROSS_VALIDATION_NONE && !coordinator->enable_adaptive_learning) ||
        input_count < 2) {
        if (coordinator->primary_weight >= coordinator->secondary_weight && input_count > 0) {
            snprintf(result, total_len, "[Primary] %s", primary_output);
        } else if (input_count > 1) {
            snprintf(result, total_len, "[Secondary] %s", secondary_output);
        } else if (input_count > 0) {
            snprintf(result, total_len, "[Fallback] %s", primary_output);
        } else {
            snprintf(result, total_len, "[Empty]");
        }

        if (coordinator->enable_adaptive_learning) {
            record_decision(&coordinator->stats,
                            (coordinator->primary_weight >= coordinator->secondary_weight) ?
                                "Primary" :
                                "Secondary",
                            1.0f, 1.0f, 1);
        }

        *out_result = result;
        return AIRY_SUCCESS;
    }

    float similarity = 0.0f;
    if (*primary_output && *secondary_output) {
        if (validation_mode >= CROSS_VALIDATION_ADVANCED || coordinator->enable_adaptive_learning) {
            similarity = enhanced_similarity(primary_output, secondary_output);
        } else {
            similarity = airy_text_similar(primary_output, secondary_output);
        }
    } else {

        similarity = 0.0f;
    }

    float effective_threshold = disagreement_threshold;
    if (coordinator->enable_adaptive_learning && validation_mode == CROSS_VALIDATION_ADAPTIVE) {
        effective_threshold = adaptive_threshold_adjust(coordinator, similarity);
    }

    float consistency_threshold = 1.0f - effective_threshold;
    int is_consistent = (similarity >= consistency_threshold);

    float primary_confidence = 1.0f;
    float secondary_confidence = 1.0f;
    if (coordinator->enable_confidence_scoring) {
        primary_confidence = calculate_confidence(primary_output);
        secondary_confidence = calculate_confidence(secondary_output);
    }

    char selected_model[64] = "Unknown";
    const char *selected_output = primary_output;
    float final_confidence = 0.0f;

    if (is_consistent) {

        if (coordinator->primary_weight >= coordinator->secondary_weight) {
            AIRY_STRNCPY_TERM(selected_model, "Primary", sizeof(selected_model));
            (selected_model)[sizeof(selected_model) - 1] = '\0';
            selected_output = primary_output;
            final_confidence = (primary_confidence + similarity) / 2.0f;
        } else {
            AIRY_STRNCPY_TERM(selected_model, "Secondary", sizeof(selected_model));
            (selected_model)[sizeof(selected_model) - 1] = '\0';
            selected_output = secondary_output;
            final_confidence = (secondary_confidence + similarity) / 2.0f;
        }
    } else {

        switch (validation_mode) {
        case CROSS_VALIDATION_BASIC:

            AIRY_STRNCPY_TERM(selected_model, "Primary (Basic)", sizeof(selected_model));
            (selected_model)[sizeof(selected_model) - 1] = '\0';
            selected_output = primary_output;
            final_confidence = primary_confidence * 0.7f;
            break;

        case CROSS_VALIDATION_ADVANCED:

            if (primary_confidence >= secondary_confidence) {
                AIRY_STRNCPY_TERM(selected_model, "Primary (Confidence)", sizeof(selected_model));
                (selected_model)[sizeof(selected_model) - 1] = '\0';
                selected_output = primary_output;
                final_confidence = primary_confidence;
            } else {
                AIRY_STRNCPY_TERM(selected_model, "Secondary (Confidence)", sizeof(selected_model));
                (selected_model)[sizeof(selected_model) - 1] = '\0';
                selected_output = secondary_output;
                final_confidence = secondary_confidence;
            }
            break;

        case CROSS_VALIDATION_ADAPTIVE:

        {
            performance_stats_t *stats = &coordinator->stats;

            if (stats->total_decisions > 20) {
                float primary_success_rate =
                    (float)(stats->total_decisions - stats->inconsistent_decisions) /
                    (float)stats->total_decisions;

                float combined_primary = 0.6f * primary_confidence + 0.4f * primary_success_rate;
                float combined_secondary =
                    0.6f * secondary_confidence + 0.4f * (1.0f - primary_success_rate);

                if (combined_primary >= combined_secondary) {
                    snprintf(selected_model, sizeof(selected_model), "Primary (Adaptive|Hist:%.2f)",
                             primary_success_rate);
                    selected_model[sizeof(selected_model) - 1] = '\0';
                    selected_output = primary_output;
                    final_confidence = combined_primary;
                } else {
                    snprintf(selected_model, sizeof(selected_model),
                             "Secondary (Adaptive|Hist:%.2f)", 1.0f - primary_success_rate);
                    selected_model[sizeof(selected_model) - 1] = '\0';
                    selected_output = secondary_output;
                    final_confidence = combined_secondary;
                }
            } else {

                if (primary_confidence >= secondary_confidence) {
                    AIRY_STRNCPY_TERM(selected_model, "Primary (Adaptive-Fallback)",
                                      sizeof(selected_model));
                    selected_output = primary_output;
                    final_confidence = primary_confidence;
                } else {
                    AIRY_STRNCPY_TERM(selected_model, "Secondary (Adaptive-Fallback)",
                                      sizeof(selected_model));
                    selected_output = secondary_output;
                    final_confidence = secondary_confidence;
                }
            }
        } break;

        default:

            AIRY_STRNCPY_TERM(selected_model, "Primary (Default)", sizeof(selected_model));
            (selected_model)[sizeof(selected_model) - 1] = '\0';
            selected_output = primary_output;
            final_confidence = primary_confidence * 0.5f;
            break;
        }
    }

    if (coordinator->enable_adaptive_learning || validation_mode != CROSS_VALIDATION_NONE) {
        record_decision(&coordinator->stats, selected_model, similarity, final_confidence,
                        is_consistent);
    }

    if (primary_confidence < 0.3f || (input_count > 0 && !*primary_output)) {
        coordinator->primary_error_count++;
        coordinator->primary_last_error_time = (uint64_t)airy_time_ns();
    }
    if (input_count > 1 && (secondary_confidence < 0.3f || !*secondary_output)) {
        coordinator->secondary_error_count++;
        coordinator->secondary_last_error_time = (uint64_t)airy_time_ns();
    }
    if (!is_consistent && final_confidence < 0.5f) {
        if (coordinator->primary_weight >= coordinator->secondary_weight) {
            coordinator->primary_error_count++;
            coordinator->primary_last_error_time = (uint64_t)airy_time_ns();
        } else {
            coordinator->secondary_error_count++;
            coordinator->secondary_last_error_time = (uint64_t)airy_time_ns();
        }
    }

    if (validation_mode != CROSS_VALIDATION_NONE || coordinator->enable_adaptive_learning) {
        if (validation_mode == CROSS_VALIDATION_ADAPTIVE && coordinator->enable_adaptive_learning) {
            snprintf(result, total_len, "[%s|相似度:%.2f|置信度:%.2f|阈值:%.2f|统计#%llu] %s",
                     selected_model, similarity, final_confidence,
                     coordinator->stats.adaptive_threshold,
                     (unsigned long long)coordinator->stats.total_decisions, selected_output);
        } else {
            snprintf(result, total_len, "[%s|相似度:%.2f|置信度:%.2f] %s", selected_model,
                     similarity, final_confidence, selected_output);
        }
    } else {
        snprintf(result, total_len, "[%s] %s", selected_model, selected_output);
    }

    *out_result = result;
    return AIRY_SUCCESS;
}

/**
 * @brief Destroy the coordinator.
 */
static void dual_destroy(airy_coordinator_base_t *base)
{
    if (!base)
        return;
    dual_model_coordinator_t *coordinator = (dual_model_coordinator_t *)base;
    if (coordinator->primary_model)
        AIRY_FREE(coordinator->primary_model);
    if (coordinator->secondary_model)
        AIRY_FREE(coordinator->secondary_model);

    AIRY_FREE(base);
}

/**
 * @brief Create a dual-model coordinator.
 */
airy_err_t airy_coord_dual_create(const char *primary_model, const char *secondary_model,
                                  float primary_weight, float secondary_weight,
                                  airy_coordinator_base_t **out_base)
{
    if (!out_base)
        return AIRY_EINVAL;

    if (!VALIDATE_STRING(primary_model)) {
        return AIRY_EINVAL;
    }
    if (!VALIDATE_STRING(secondary_model)) {
        return AIRY_EINVAL;
    }

    if (strlen(primary_model) > MODEL_NAME_MAX_LEN) {
        return AIRY_EINVAL;
    }
    if (strlen(secondary_model) > MODEL_NAME_MAX_LEN) {
        return AIRY_EINVAL;
    }

    if (!validate_weights(primary_weight, secondary_weight)) {
        return AIRY_EINVAL;
    }

    dual_model_coordinator_t *coordinator =
        (dual_model_coordinator_t *)AIRY_CALLOC(1, sizeof(dual_model_coordinator_t));
    if (!coordinator)
        return AIRY_ENOMEM;

    if (primary_model) {
        coordinator->primary_model = AIRY_STRDUP(primary_model);
        if (!coordinator->primary_model) {
            AIRY_FREE(coordinator);
            return AIRY_ENOMEM;
        }
    }

    if (secondary_model) {
        coordinator->secondary_model = AIRY_STRDUP(secondary_model);
        if (!coordinator->secondary_model) {
            if (coordinator->primary_model)
                AIRY_FREE(coordinator->primary_model);
            AIRY_FREE(coordinator);
            return AIRY_ENOMEM;
        }
    }

    coordinator->primary_weight = primary_weight;
    coordinator->secondary_weight = secondary_weight;

    coordinator->primary_healthy = 1;
    coordinator->secondary_healthy = 1;
    coordinator->primary_error_count = 0;
    coordinator->secondary_error_count = 0;
    coordinator->primary_last_error_time = 0;
    coordinator->secondary_last_error_time = 0;
    coordinator->health_check_interval = 60ULL * 1000000000ULL;
    coordinator->last_health_check_time = 0;

    coordinator->validation_mode = CROSS_VALIDATION_NONE;
    coordinator->disagreement_threshold = 0.3f;
    coordinator->enable_confidence_scoring = 0;

    init_performance_stats(&coordinator->stats);
    coordinator->enable_adaptive_learning = 0;
    coordinator->learning_rate = 0.1f;

    coordinator->base.coordinate = dual_coordinate;
    coordinator->base.destroy = dual_destroy;

    *out_base = &coordinator->base;
    return AIRY_SUCCESS;
}

/**
 * @brief Configure the cross-validation mode of the dual-model coordinator.
 * @param base Coordinator instance
 * @param mode Validation mode (NONE/BASIC/ADVANCED/ADAPTIVE)
 * @return Error code
 */
airy_err_t airy_coord_dual_set_valmode(airy_coordinator_base_t *base, cross_validation_mode_t mode)
{
    if (!base)
        return AIRY_EINVAL;

    dual_model_coordinator_t *coordinator = (dual_model_coordinator_t *)base;
    coordinator->validation_mode = mode;

    if (mode == CROSS_VALIDATION_ADAPTIVE) {
        coordinator->enable_adaptive_learning = 1;
    }

    return AIRY_SUCCESS;
}

/**
 * @brief Enable or disable adaptive learning.
 * @param base Coordinator instance
 * @param enable Whether to enable (1=enabled, 0=disabled)
 * @param learning_rate Learning rate (0.0-1.0), only meaningful when enabled
 * @return Error code
 */
airy_err_t airy_coord_dual_enable_adapt(airy_coordinator_base_t *base, int enable,
                                        float learning_rate)
{
    if (!base)
        return AIRY_EINVAL;

    dual_model_coordinator_t *coordinator = (dual_model_coordinator_t *)base;
    coordinator->enable_adaptive_learning = enable;

    if (enable) {

        if (learning_rate < 0.0f || learning_rate > 1.0f) {
            learning_rate = 0.1f;
        }
        coordinator->learning_rate = learning_rate;

        if (coordinator->validation_mode == CROSS_VALIDATION_NONE) {
            coordinator->validation_mode = CROSS_VALIDATION_ADAPTIVE;
        }
    }

    return AIRY_SUCCESS;
}

/**
 * @brief Get performance statistics.
 * @param base Coordinator instance
 * @param[out] stats Output statistics structure
 * @return Error code
 */
airy_err_t airy_coord_dual_get_stats(airy_coordinator_base_t *base, performance_stats_t **stats)
{
    if (!base || !stats)
        return AIRY_EINVAL;

    dual_model_coordinator_t *coordinator = (dual_model_coordinator_t *)base;
    *stats = &coordinator->stats;

    return AIRY_SUCCESS;
}

/**
 * @brief Reset performance statistics and history.
 * @param base Coordinator instance
 * @return Error code
 */
airy_err_t airy_coord_dual_reset_stats(airy_coordinator_base_t *base)
{
    if (!base)
        return AIRY_EINVAL;

    dual_model_coordinator_t *coordinator = (dual_model_coordinator_t *)base;
    init_performance_stats(&coordinator->stats);

    return AIRY_SUCCESS;
}
