// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ml_planner.c
 * @brief ML-based planning strategy with rule-based primary path
 *
 * [DESIGN] Rule-based planning is the current production implementation.
 * ML runtime integration (ONNX/TFLite) is planned for a future release to enhance
 * planning quality with learned task decomposition patterns.
 *
 * M5-4 归位：原 atoms/coreloopthree/src/cognition/think/planner/ml_planner.c
 * 迁至 products/cognition（机制留核、策略迁生态层，台账 §262）。内容守恒；
 * 工厂签名与头声明根因对齐（原 void* 分裂修复），计划节点回收改引机制公共
 * 契约头 airy_plan_nodes.h。
 */

#include "cognition.h"
#include "logging.h"
#include "agent_vocab.h"
#include "airy_memory.h"
#include "airy_plan_nodes.h"
#include "plan_strategy.h"
#include "platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

/**
 * @brief ML model handle (for future ML runtime integration)
 */
typedef struct ml_model {
    void *handle;
    int (*predict)(void *handle, const float *input, int input_len, float *output, int output_len);
} ml_model_t;

typedef struct ml_planner_data {
    ml_model_t *model;
    char *model_path;
    void *llm;
    airy_mtx_t *lock;
    bool rule_based_active; /* True when using rule-based planning (current production path) */
} ml_planner_data_t;

static void ml_planner_destroy(airy_plan_strategy_t *strategy)
{
    if (!strategy)
        return;
    ml_planner_data_t *data = (ml_planner_data_t *)strategy->data;
    if (data) {
        if (data->model) {
            if (data->model->handle) {
                AIRY_LOG_INFO("ML planner: model handle %p released", data->model->handle);
                data->model->handle = NULL;
            }
            AIRY_FREE(data->model);
        }
        if (data->model_path)
            AIRY_FREE(data->model_path);
        if (data->lock)
            airy_mtx_free(data->lock);
        AIRY_FREE(data);
    }
    AIRY_FREE(strategy);
}

/**
 * @brief Initialize model handle (currently uses rule-based planning)
 *
 * This function prepares the planner for rule-based planning, which is
 * the current production-safe primary path. ML runtime integration
 * will be added in a future release.
 */
static bool ml_planner_try_load_model(ml_planner_data_t *data)
{
    if (!data)
        return false;

    /* [DESIGN] Rule-based planning is the current primary path.
     * ML runtime integration (ONNX/TFLite) is planned for a future release
     * to enhance planning quality with learned task decomposition patterns. */
    SAFE_MALLOC_ARRAY(data->model, 1, sizeof(ml_model_t));
    if (!data->model) {
        data->rule_based_active = true;
        return false;
    }

    data->model->handle = NULL;
    data->model->predict = NULL;
    data->rule_based_active = true;

    AIRY_LOG_INFO(
        "ML planner: rule-based planning initialized, ML integration planned for a future release");
    return true;
}

/**
 * @brief Rule-based task decomposition by intent type
 *
 * Maps intent types to structured task decompositions with
 * dependency chains and role assignments.
 */
typedef struct {
    const char *intent_type;
    int min_complexity;
    int max_complexity;
    const char **subtasks;
    size_t subtask_count;
    const char *primary_role;
    bool requires_verification;
} rule_decomposition_t;

/**
 * @brief Fill GRAD verification metadata (E-01 causality / E-03 resources / E-04 invariants).
 *
 * outputs = this node's artifact signature; inputs = the union of
 * dependency nodes' output signatures (dependencies must have been
 * built in earlier steps). On allocation failure, keep NULL/count=0 and
 * let the verifier skip conservatively (see grad_verifier.h).
 */
static void ml_planner_fill_grad_metadata(airy_task_plan_t *plan, airy_task_node_t *node,
                                          const char *node_id)
{
    if (!node || !node_id)
        return;

    char out_sig[192];
    snprintf(out_sig, sizeof(out_sig), "artifact:%s", node_id);
    node->task_node_outputs = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (node->task_node_outputs) {
        node->task_node_outputs[0] = AIRY_STRDUP(out_sig);
        if (node->task_node_outputs[0])
            node->task_node_outputs_count = 1;
    }

    if (node->task_node_depends_count > 0 && node->task_node_depends_on) {
        node->task_node_inputs =
            (char **)AIRY_CALLOC(node->task_node_depends_count, sizeof(char *));
        if (node->task_node_inputs) {
            for (size_t d = 0; d < node->task_node_depends_count; d++) {
                for (size_t p = 0; p < plan->task_plan_node_count; p++) {
                    airy_task_node_t *pnode = plan->task_plan_nodes[p];
                    if (pnode && pnode->task_node_id && node->task_node_depends_on[d] &&
                        strcmp(pnode->task_node_id, node->task_node_depends_on[d]) == 0 &&
                        pnode->task_node_outputs_count > 0 && pnode->task_node_outputs[0]) {
                        node->task_node_inputs[node->task_node_inputs_count] =
                            AIRY_STRDUP(pnode->task_node_outputs[0]);
                        if (node->task_node_inputs[node->task_node_inputs_count])
                            node->task_node_inputs_count++;
                        break;
                    }
                }
            }
            if (node->task_node_inputs_count == 0) {
                AIRY_FREE(node->task_node_inputs);
                node->task_node_inputs = NULL;
            }
        }
    }

    node->task_node_cost_time_ms = node->task_node_timeout_ms * 4 / 10;
    node->task_node_cost_mem_mb = 64;
    node->task_node_invariant_guard = 0;
}

static const char *QUERY_SUBTASKS[] = {"analyze_query_semantics", "retrieve_relevant_context",
                                       "formulate_search_strategy", "execute_information_gathering",
                                       "synthesize_results"};
static const char *ACTION_SUBTASKS[] = {
    "validate_action_parameters", "check_preconditions", "plan_execution_sequence",
    "execute_primary_action",     "verify_outcome",      "handle_exceptions"};
static const char *CREATIVE_SUBTASKS[] = {"understand_creative_constraints",
                                          "generate_initial_ideas", "evaluate_feasibility",
                                          "refine_best_option", "produce_final_output"};
static const char *ANALYSIS_SUBTASKS[] = {"collect_input_data",   "normalize_data_format",
                                          "apply_analysis_rules", "detect_patterns",
                                          "generate_insights",    "format_report"};

static const char *QUERY_FINE_SUBTASKS[] = {"extract_query_entities", "resolve_entity_ambiguity",
                                            "build_semantic_graph",   "identify_information_gaps",
                                            "rank_retrieval_sources", "execute_parallel_retrieval",
                                            "cross_validate_results", "synthesize_answer"};
static const char *ACTION_FINE_SUBTASKS[] = {
    "parse_action_specification", "validate_action_schema", "check_resource_availability",
    "estimate_execution_cost",    "create_execution_plan",  "acquire_locks_and_resources",
    "execute_atomic_actions",     "verify_state_change",    "commit_or_rollback",
    "notify_stakeholders"};
static const char *CREATIVE_FINE_SUBTASKS[] = {
    "analyze_creative_brief",   "research_domain_examples", "generate_divergent_ideas",
    "apply_constraints_filter", "score_ideas_by_criteria",  "select_top_candidates",
    "iterative_refinement",     "final_polish_and_format"};

#define MAX_DECOMPOSITION_DEPTH 3

typedef struct {
    const char **coarse_tasks;
    size_t coarse_count;
    const char **fine_tasks;
    size_t fine_count;
    int complexity_threshold; /* >= this value use fine-grained */
} multi_grain_decomp_t;

static const multi_grain_decomp_t MULTI_GRAIN_TABLE[] = {
    {QUERY_SUBTASKS, 5, QUERY_FINE_SUBTASKS, 8, 4},
    {ACTION_SUBTASKS, 6, ACTION_FINE_SUBTASKS, 10, 4},
    {CREATIVE_SUBTASKS, 4, CREATIVE_FINE_SUBTASKS, 8, 3},
    {ANALYSIS_SUBTASKS, 6, NULL, 0, 5}, /* analysis stays coarse */
};

static const rule_decomposition_t RULE_TABLE[] = {
    {"query", 1, 5, QUERY_SUBTASKS, 5, AGENT_VOCAB_ROLE_ANALYST, true},
    {"action", 1, 5, ACTION_SUBTASKS, 6, "executor", true},
    {"creative", 1, 5, CREATIVE_SUBTASKS, 4, "creator", false},
    {"analysis", 1, 5, ANALYSIS_SUBTASKS, 6, AGENT_VOCAB_ROLE_ANALYST, true},
};
static const size_t RULE_COUNT = sizeof(RULE_TABLE) / sizeof(RULE_TABLE[0]);

static airy_err_t ml_planner_rule_based_plan(const airy_intent_t *intent,
                                             airy_task_plan_t **out_plan)
{

    if (!intent || !out_plan)
        AIRY_RET_ERR(AIRY_EINVAL);

    int rule_index = -1;
    int complexity = (int)(intent->intent_flags & 0x07);
    for (size_t r = 0; r < RULE_COUNT; r++) {
        if (intent->intent_goal &&
            strstr((const char *)intent->intent_goal, RULE_TABLE[r].intent_type)) {
            if (complexity >= RULE_TABLE[r].min_complexity &&
                complexity <= RULE_TABLE[r].max_complexity) {
                rule_index = (int)r;
                break;
            }
        }
    }

    const char **subtasks = NULL;
    size_t subtask_count = 0;
    const char *primary_role = "default";
    bool needs_verify = true;
    int use_fine_grain = 0;

    if (rule_index >= 0) {
        const rule_decomposition_t *rule = &RULE_TABLE[rule_index];
        primary_role = rule->primary_role;
        needs_verify = rule->requires_verification;

        if (rule_index < (int)(sizeof(MULTI_GRAIN_TABLE) / sizeof(MULTI_GRAIN_TABLE[0])) &&
            MULTI_GRAIN_TABLE[rule_index].fine_tasks != NULL &&
            complexity >= MULTI_GRAIN_TABLE[rule_index].complexity_threshold) {
            use_fine_grain = 1;
            subtasks = MULTI_GRAIN_TABLE[rule_index].fine_tasks;
            subtask_count = MULTI_GRAIN_TABLE[rule_index].fine_count;
            AIRY_LOG_INFO("ML planner: using fine-grained decomposition (%zu steps)", subtask_count);
        } else {
            subtasks = rule->subtasks;
            subtask_count = rule->subtask_count;
        }
    } else {
        static const char *GENERIC_TASKS[] = {"process_intent", "generate_response"};
        subtasks = (const char **)GENERIC_TASKS;
        subtask_count = 2;
    }

    airy_task_plan_t *plan;
    SAFE_MALLOC_ARRAY(plan, 1, sizeof(airy_task_plan_t));
    if (!plan) {
        AIRY_LOG_ERROR("ML planner: plan allocation failed");
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    char plan_id[64];
    snprintf(plan_id, sizeof(plan_id), "rule_plan_%s_%d%s",
             intent->intent_goal ? (const char *)intent->intent_goal : "unknown", complexity,
             use_fine_grain ? "_fine" : "");
    plan->task_plan_id = AIRY_STRDUP(plan_id);
    if (!plan->task_plan_id) {
        AIRY_FREE(plan);
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    SAFE_MALLOC_ARRAY(plan->task_plan_nodes, subtask_count + 2, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes && subtask_count > 0) {
        AIRY_LOG_ERROR("ML planner: task_plan_nodes allocation failed (count=%zu)", subtask_count + 2);
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    typedef struct {
        int start;
        int count;
        int parallel_group;
    } parallel_group_t;
    parallel_group_t groups[MAX_DECOMPOSITION_DEPTH] = {{0}};
    int group_count = 0;

    if (use_fine_grain && subtask_count >= 4) {

        groups[0].start = 0;
        groups[0].count = (subtask_count >= 6) ? 3 : 2;
        groups[0].parallel_group = 0;
        group_count++;
        if (subtask_count > (size_t)groups[0].count + 2) {
            groups[1].start = groups[0].count;
            /* count 为 int；统一在 size_t 域做减法再收窄（C4267） */
            groups[1].count = (int)(subtask_count - (size_t)groups[0].count - 1);
            groups[1].parallel_group = 1;
            group_count++;
        }
    }

    for (size_t i = 0; i < subtask_count; i++) {
        airy_task_node_t *node;
        SAFE_MALLOC_ARRAY(node, 1, sizeof(airy_task_node_t));
        if (!node) {
            AIRY_LOG_ERROR("ML planner: node allocation failed at step %zu", i);
            goto cleanup_nodes;
        }

        char node_id[128];
        if (subtasks && i < subtask_count) {
            snprintf(node_id, sizeof(node_id), "%s_%s", plan_id, subtasks[i]);
        } else {
            snprintf(node_id, sizeof(node_id), "%s_step%zu", plan_id, i + 1);
        }
        node->task_node_id = AIRY_STRDUP(node_id);
        if (!node->task_node_id) {
            AIRY_LOG_ERROR("ML planner: node id STRDUP failed at step %zu", i);
            AIRY_FREE(node);
            goto cleanup_nodes;
        }
        node->task_node_agent_role = AIRY_STRDUP(primary_role);
        if (!node->task_node_agent_role) {
            AIRY_LOG_ERROR("ML planner: node role STRDUP failed at step %zu", i);
            AIRY_FREE(node->task_node_id);
            AIRY_FREE(node);
            goto cleanup_nodes;
        }

        int base_timeout = 15000;
        if (use_fine_grain) {

            base_timeout = (i < subtask_count / 2) ? 10000 : 25000;
        }
        node->task_node_timeout_ms = base_timeout + (int)i * 3000;
        node->task_node_priority = 128 - (int)i * 8;

        if (i > 0) {

            int in_parallel_group = 0;
            int my_group_start = 0;
            for (int g = 0; g < group_count; g++) {
                if ((int)i >= groups[g].start && (int)i < groups[g].start + groups[g].count) {
                    in_parallel_group = 1;
                    my_group_start = groups[g].start;
                    break;
                }
            }

            if (in_parallel_group && (int)i > my_group_start) {

                size_t dep_count = 1 + (size_t)my_group_start;
                SAFE_MALLOC_ARRAY(node->task_node_depends_on, dep_count, sizeof(char *));
                if (node->task_node_depends_on) {
                    node->task_node_depends_count = dep_count;

                    node->task_node_depends_on[0] =
                        AIRY_STRDUP(plan->task_plan_nodes[my_group_start]->task_node_id);
                    if (!node->task_node_depends_on[0]) {

                        AIRY_FREE(node->task_node_depends_on);
                        node->task_node_depends_on = NULL;
                        node->task_node_depends_count = 0;
                    } else {

                        for (size_t d = 1; d < dep_count; d++) {
                            node->task_node_depends_on[d] =
                                AIRY_STRDUP(plan->task_plan_nodes[d - 1]->task_node_id);
                            if (!node->task_node_depends_on[d]) {

                                for (size_t e = 0; e < d; e++)
                                    AIRY_FREE(node->task_node_depends_on[e]);
                                AIRY_FREE(node->task_node_depends_on);
                                node->task_node_depends_on = NULL;
                                node->task_node_depends_count = 0;
                                break;
                            }
                        }
                    }
                }
            } else {

                SAFE_MALLOC_ARRAY(node->task_node_depends_on, 1, sizeof(char *));
                if (node->task_node_depends_on) {
                    node->task_node_depends_count = 1;
                    node->task_node_depends_on[0] =
                        AIRY_STRDUP(plan->task_plan_nodes[i - 1]->task_node_id);
                    if (!node->task_node_depends_on[0]) {

                        AIRY_FREE(node->task_node_depends_on);
                        node->task_node_depends_on = NULL;
                        node->task_node_depends_count = 0;
                    }
                }
            }
        }

        ml_planner_fill_grad_metadata(plan, node, node_id);

        plan->task_plan_nodes[i] = node;
        plan->task_plan_node_count++;
    }

    SAFE_MALLOC_ARRAY(plan->task_plan_entry_points, 1, sizeof(char *));
    if (plan->task_plan_entry_points && plan->task_plan_node_count > 0) {
        plan->task_plan_entry_count = 1;
        plan->task_plan_entry_points[0] = AIRY_STRDUP(plan->task_plan_nodes[0]->task_node_id);
    }

    if (needs_verify && subtask_count > 0) {
        airy_task_node_t *verify_node;
        SAFE_MALLOC_ARRAY(verify_node, 1, sizeof(airy_task_node_t));
        if (verify_node) {
            char verify_id[128];
            snprintf(verify_id, sizeof(verify_id), "%s_verify", plan_id);
            verify_node->task_node_id = AIRY_STRDUP(verify_id);
            verify_node->task_node_agent_role = AIRY_STRDUP("verifier");
            verify_node->task_node_timeout_ms = 10000;
            verify_node->task_node_priority = 255;
            SAFE_MALLOC_ARRAY(verify_node->task_node_depends_on, 1, sizeof(char *));
            if (verify_node->task_node_depends_on) {
                verify_node->task_node_depends_count = 1;
                verify_node->task_node_depends_on[0] = AIRY_STRDUP(
                    plan->task_plan_nodes[plan->task_plan_node_count - 1]->task_node_id);
                if (!verify_node->task_node_depends_on[0]) {

                    AIRY_FREE(verify_node->task_node_depends_on);
                    verify_node->task_node_depends_on = NULL;
                    verify_node->task_node_depends_count = 0;
                }
            }

            ml_planner_fill_grad_metadata(plan, verify_node, verify_id);

            airy_task_node_t **expanded =
                (airy_task_node_t **)AIRY_REALLOC(plan->task_plan_nodes,
                                                  (plan->task_plan_node_count + 1) *
                                                      sizeof(airy_task_node_t *));
            if (expanded) {
                plan->task_plan_nodes = expanded;
                plan->task_plan_nodes[plan->task_plan_node_count++] = verify_node;
            } else {
                plan_node_free(verify_node);
            }
        }
    }

    if (complexity >= 5 && subtask_count >= 4) {
        airy_task_node_t *qa_node;
        SAFE_MALLOC_ARRAY(qa_node, 1, sizeof(airy_task_node_t));
        if (qa_node) {
            char qa_id[128];
            snprintf(qa_id, sizeof(qa_id), "%s_quality_gate", plan_id);
            qa_node->task_node_id = AIRY_STRDUP(qa_id);
            qa_node->task_node_agent_role = AIRY_STRDUP("quality_assurance");
            qa_node->task_node_timeout_ms = 8000;
            qa_node->task_node_priority = 254;
            SAFE_MALLOC_ARRAY(qa_node->task_node_depends_on, 1, sizeof(char *));
            if (qa_node->task_node_depends_on) {
                qa_node->task_node_depends_count = 1;
                qa_node->task_node_depends_on[0] = AIRY_STRDUP(
                    plan->task_plan_nodes[plan->task_plan_node_count - 1]->task_node_id);
                if (!qa_node->task_node_depends_on[0]) {

                    AIRY_FREE(qa_node->task_node_depends_on);
                    qa_node->task_node_depends_on = NULL;
                    qa_node->task_node_depends_count = 0;
                }
            }

            ml_planner_fill_grad_metadata(plan, qa_node, qa_id);

            airy_task_node_t **expanded =
                (airy_task_node_t **)AIRY_REALLOC(plan->task_plan_nodes,
                                                  (plan->task_plan_node_count + 1) *
                                                      sizeof(airy_task_node_t *));
            if (expanded) {
                plan->task_plan_nodes = expanded;
                plan->task_plan_nodes[plan->task_plan_node_count++] = qa_node;
            } else {
                plan_node_free(qa_node);
            }
        }
    }

    if (plan->task_plan_node_count > 0)
        plan->task_plan_nodes[plan->task_plan_node_count - 1]->task_node_invariant_guard = 1;

    *out_plan = plan;
    AIRY_LOG_INFO(
        "ML planner: generated %zu-step %splan for intent '%s' (complexity=%d, parallel_groups=%d)",
        plan->task_plan_node_count, use_fine_grain ? "fine-grained " : "",
        intent->intent_goal ? (const char *)intent->intent_goal : "?", complexity, group_count);
    return AIRY_SUCCESS;

cleanup_nodes:
    plan_nodes_reclaim(plan);
    AIRY_LOG_ERROR("ML planner: plan construction failed, cleaned up partial plan");
    AIRY_RET_ERR(AIRY_ENOMEM);
}

static airy_err_t ml_planner_plan(const airy_intent_t *intent, void *context,
                                  airy_task_plan_t **out_plan)
{

    ml_planner_data_t *data = (ml_planner_data_t *)context;
    if (!data || !intent || !out_plan)
        AIRY_RET_ERR(AIRY_EINVAL);

    // Primary path: rule-based planning (always available)
    // This replaces both the old fallback and the PHASE2-IMPLEMENTED stub
    return ml_planner_rule_based_plan(intent, out_plan);
}

airy_plan_strategy_t *airy_plan_ml_create(const char *model_path, airy_llm_service_t *llm)
{

    airy_plan_strategy_t *strat;
    SAFE_MALLOC_ARRAY(strat, 1, sizeof(airy_plan_strategy_t));
    if (!strat)
        return NULL;

    ml_planner_data_t *data;
    SAFE_MALLOC_ARRAY(data, 1, sizeof(ml_planner_data_t));
    if (!data) {
        AIRY_FREE(strat);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    data->model = NULL;
    data->model_path = model_path ? AIRY_STRDUP(model_path) : NULL;
    if (model_path && !data->model_path) {
        AIRY_FREE(data);
        AIRY_FREE(strat);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    data->llm = llm;
    data->rule_based_active = false;
    data->lock = airy_mtx_create();
    if (!data->lock) {
        if (data->model_path)
            AIRY_FREE(data->model_path);
        AIRY_FREE(data);
        AIRY_FREE(strat);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* Attempt to load model if path provided */
    if (model_path) {
        ml_planner_try_load_model(data);
    } else {
        data->rule_based_active = true;
        AIRY_LOG_INFO("ML planner: no model path, using rule-based planning");
    }

    strat->plan = ml_planner_plan;
    strat->destroy = ml_planner_destroy;
    strat->data = data;

    return strat;
}
