/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cognitive_evolution.h
 * @brief CognitiveEvolution: AgentRT cognitive evolution system.
 *
 * Adaptive self-improvement of agent cognition, designed around the
 * cognitive view (C dimension) of MCIS theory. Supports experience
 * accumulation, strategy optimization, knowledge transfer and cognitive
 * level transition.
 *
 * Core capabilities:
 * 1. Experience memory - extract patterns from past interactions
 * 2. Strategy evolution - auto-optimize decision strategies from feedback
 * 3. Knowledge transfer - cross-domain knowledge reuse and adaptation
 * 4. Cognitive levels - transition from perception to reasoning
 * 5. Adaptive learning - adjust cognitive parameters to the environment
 */

#ifndef AIRY_RT_COGNITIVE_EVOLUTION_H
#define AIRY_RT_COGNITIVE_EVOLUTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COG_EVO_MAX_EXPERIENCES 10000
#define COG_EVO_MAX_STRATEGIES 256
#define COG_EVO_MAX_PATTERNS 4096
#define COG_EVO_MAX_KNOWLEDGE 8192

typedef enum {
    COG_LEVEL_PERCEPTION = 0,
    COG_LEVEL_REACTION,
    COG_LEVEL_LEARNING,
    COG_LEVEL_REASONING,
    COG_LEVEL_CREATION
} cog_level_t;

typedef enum {
    COG_FEEDBACK_POSITIVE = 0,
    COG_FEEDBACK_NEGATIVE,
    COG_FEEDBACK_NEUTRAL
} cog_feedback_t;

typedef struct {
    char id[64];
    char domain[128];
    char *context_json;
    char *action_json;
    char *outcome_json;
    cog_feedback_t feedback;
    double reward;
    uint64_t timestamp;
    cog_level_t cognitive_level;
} cog_experience_t;

typedef struct {
    char id[64];
    char name[128];
    char domain[128];
    char *condition_json;
    char *action_json;
    double fitness;
    uint64_t use_count;
    uint64_t success_count;
    uint64_t last_used;
    cog_level_t min_level;
} cog_strategy_t;

typedef struct {
    char id[64];
    char pattern_type[64];
    char *trigger_json;
    char *response_json;
    double confidence;
    uint64_t occurrence_count;
    uint64_t last_seen;
} cog_pattern_t;

typedef struct {
    char id[64];
    char source_domain[128];
    char target_domain[128];
    char *knowledge_json;
    char *adaptation_json;
    double transfer_score;
    bool validated;
} cog_knowledge_t;

typedef struct cog_evolution_s cog_evolution_t;

typedef double (*cog_reward_fn)(const cog_experience_t *experience, void *user_data);
typedef void (*cog_level_change_fn)(cog_level_t old_level, cog_level_t new_level, void *user_data);

cog_evolution_t *cog_evolution_create(cog_level_t initial_level);
void cog_evolution_destroy(cog_evolution_t *evo);

int cog_evolution_record_experience(cog_evolution_t *evo, const cog_experience_t *experience);
int cog_evolution_extract_patterns(cog_evolution_t *evo, size_t *pattern_count);
int cog_evolution_evolve_strategies(cog_evolution_t *evo, size_t *strategy_count);

int cog_evolution_select_strategy(cog_evolution_t *evo, const char *domain,
                                  const char *context_json, cog_strategy_t **strategy);

int cog_evolution_transfer_knowledge(cog_evolution_t *evo, const char *source_domain,
                                     const char *target_domain, cog_knowledge_t **knowledge);

cog_level_t cog_evolution_get_level(cog_evolution_t *evo);
int cog_evolution_evaluate_level(cog_evolution_t *evo, cog_level_t *new_level);

int cog_evolution_set_reward_fn(cog_evolution_t *evo, cog_reward_fn fn, void *user_data);
int cog_evolution_set_level_change_fn(cog_evolution_t *evo, cog_level_change_fn fn,
                                      void *user_data);

size_t cog_evolution_get_experience_count(cog_evolution_t *evo);
size_t cog_evolution_get_strategy_count(cog_evolution_t *evo);
size_t cog_evolution_get_pattern_count(cog_evolution_t *evo);
double cog_evolution_get_fitness(cog_evolution_t *evo);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_COGNITIVE_EVOLUTION_H */
