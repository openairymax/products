// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file reflective_llm_plan.c
 * @brief Reflective planning strategy — LLM dynamic plan construction domain.
 *
 * 从 reflective.c 拆分（超 800 行文件拆分，代码质量提升 1.11.2）：
 * - llm_plan_node_t / parse_llm_plan_json：LLM JSON 计划解析
 * - llm_build_dynamic_plan：基于 LLM 响应的结构化任务计划构建（含审计失败
 *   correction 节点、对齐失败 realignment 节点）
 * - build_fallback_plan：LLM 不可用时的确定性 5 步回退计划
 *
 * 共享上下文与函数契约见 reflective_internal.h；管线主流程仍在 reflective.c。
 *
 * 0.1.19 M5-4 §271 自 atoms/coreloopthree src/cognition/think/planner/
 * 迁入 products/cognition（族内归格 src/planner/）。
 */

#include "reflective_internal.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * LLM plan JSON parsing — P2-B04
 * ============================================================================ */

typedef struct {
    char name[64];
    char role[64];
    int timeout_ms;
    int priority;
    int depends_on_count;
    char depends_on[4][64];
} llm_plan_node_t;

static airy_err_t parse_llm_plan_json(const char *json_text, llm_plan_node_t *nodes,
                                      size_t *node_count, size_t max_nodes)
{

    *node_count = 0;
    if (!json_text || !nodes)
        AIRY_RET_ERR(AIRY_EINVAL);

    const char *p = json_text;

    while (*p && *node_count < max_nodes) {
        const char *name_start = strstr(p, "\"name\"");
        if (!name_start)
            break;
        const char *name_val = strchr(name_start + 6, ':');
        if (!name_val)
            break;
        while (*name_val && (*name_val == ' ' || *name_val == '\t' || *name_val == '"'))
            name_val++;
        const char *name_end = name_val;
        while (*name_end && *name_end != '"' && *name_end != ',' && *name_end != '}')
            name_end++;

        const char *role_start = strstr(name_end, "\"role\"");
        const char *role_val = role_start ? strchr(role_start + 5, ':') : NULL;
        if (role_val) {
            while (*role_val && (*role_val == ' ' || *role_val == '\t' || *role_val == '"'))
                role_val++;
        }

        llm_plan_node_t *node = &nodes[*node_count];
        __builtin_memset(node, 0, sizeof(*node));
        size_t nlen = (size_t)(name_end - name_val);
        if (nlen >= sizeof(node->name))
            nlen = sizeof(node->name) - 1;
        AIRY_MEMCPY_SAFE(node->name, name_val, nlen, sizeof(node->name));

        if (role_val) {
            const char *role_end = role_val;
            while (*role_end && *role_end != '"' && *role_end != ',' && *role_end != '}')
                role_end++;
            size_t rlen = (size_t)(role_end - role_val);
            if (rlen >= sizeof(node->role))
                rlen = sizeof(node->role) - 1;
            AIRY_MEMCPY_SAFE(node->role, role_val, rlen, sizeof(node->role));
        } else {
            snprintf(node->role, sizeof(node->role), "worker");
        }
        node->timeout_ms = 30000;
        node->priority = 200 - (int)(*node_count) * 20;

        const char *deps_start = strstr(name_end, "\"depends\"");
        if (deps_start) {
            const char *dep_arr = strchr(deps_start + 8, '[');
            if (dep_arr) {
                const char *dep = dep_arr + 1;
                while (*dep && *dep != ']' && node->depends_on_count < 4) {
                    while (*dep && (*dep == ' ' || *dep == '"' || *dep == '\t'))
                        dep++;
                    const char *de = dep;
                    while (*de && *de != '"' && *de != ',' && *de != ']')
                        de++;
                    size_t dlen = (size_t)(de - dep);
                    if (dlen > 0 && dlen < sizeof(node->depends_on[0])) {
                        AIRY_MEMCPY_SAFE(node->depends_on[node->depends_on_count], dep, dlen,
                                         sizeof(node->depends_on[0]));
                        node->depends_on_count++;
                    }
                    dep = de;
                    if (*dep == ',')
                        dep++;
                }
            }
        }

        (*node_count)++;
        p = name_end;
    }

    return (*node_count > 0) ? AIRY_SUCCESS : AIRY_EINVAL;
}

static void reflective_destroy_single_node(airy_task_node_t *node)
{
    if (!node)
        return;
    AIRY_FREE(node->task_node_id);
    AIRY_FREE(node->task_node_agent_role);
    if (node->task_node_depends_on) {
        for (size_t d = 0; d < node->task_node_depends_count; d++)
            AIRY_FREE(node->task_node_depends_on[d]);
        AIRY_FREE(node->task_node_depends_on);
    }
    AIRY_FREE(node);
}

/* ============================================================================
 * LLM dynamic planning — call the LLM to generate a structured task plan
 * ============================================================================ */

airy_err_t llm_build_dynamic_plan(reflective_context_t *ctx, const airy_intent_t *intent,
                                  airy_task_plan_t **out_plan, int audit_passed, int aligned)
{

    if (!ctx || !intent || !out_plan)
        AIRY_RET_ERR(AIRY_EINVAL);

    const airy_tc_ops_t *tc = are_ops_get_tc();

    if (!ctx->llm || !airy_llm_service_is_available(ctx->llm)) {
        AIRY_RET_ERR(AIRY_ESERVICE);
    }

    char prompt[2048];
    int plen = snprintf(
        prompt, sizeof(prompt),
        "You are a task planning AI. Analyze this goal and create a structured execution plan.\n\n"
        "Goal: %s\n"
        "Flags: %u\n"
        "Audit Status: %s\n"
        "Alignment Status: %s\n\n"
        "Return ONLY valid JSON array of subtasks:\n"
        "[{\"name\":\"task_name\",\"role\":\"agent_role\","
        "\"depends\":[\"prev_task_id\"]}, ...]\n\n"
        "Rules:\n"
        "- Each task must have a unique name and role\n"
        "- Dependencies reference previous task names\n"
        "- Include: analysis, planning, execution, verification steps\n"
        "- If audit failed, add a correction step\n"
        "- If alignment failed, add a realignment step",
        intent->intent_goal ? (const char *)intent->intent_goal : "(none)", intent->intent_flags,
        audit_passed ? "passed" : "FAILED", aligned ? "aligned" : "DRIFTED");

    if (plen <= 0 || (size_t)plen >= sizeof(prompt))
        AIRY_RET_ERR(AIRY_EUNKNOWN);

    char *response = NULL;
    airy_err_t err = airy_llm_service_call(ctx->llm, prompt, &response);
    if (err != AIRY_SUCCESS || !response) {
        if (response)
            AIRY_FREE(response);
        return err ? err : AIRY_EUNKNOWN;
    }

    llm_plan_node_t parsed_nodes[16];
    size_t parsed_count = 0;
    err = parse_llm_plan_json(response, parsed_nodes, &parsed_count, 16);
    AIRY_FREE(response);

    if (err != AIRY_SUCCESS || parsed_count == 0) {
        return err;
    }

    airy_task_plan_t *plan;
    SAFE_MALLOC_ARRAY(plan, 1, sizeof(airy_task_plan_t));
    if (!plan) {
        AIRY_LOG_ERROR("reflective: plan allocation failed");
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    char plan_id[128];
    snprintf(plan_id, sizeof(plan_id), "llm_dynamic_%zu", ctx->session_count);
    plan->task_plan_id = AIRY_STRDUP(plan_id);
    if (!plan->task_plan_id) {
        AIRY_LOG_ERROR("reflective: plan id allocation failed");
        AIRY_FREE(plan);
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    size_t total = parsed_count;
    if (!audit_passed)
        total++;
    if (!aligned)
        total++;

    SAFE_MALLOC_ARRAY(plan->task_plan_nodes, total, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes && total > 0) {
        AIRY_LOG_ERROR("reflective: task_plan_nodes allocation failed (count=%zu)", total);
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    for (size_t i = 0; i < parsed_count; i++) {
        airy_task_node_t *node;
        SAFE_MALLOC_ARRAY(node, 1, sizeof(airy_task_node_t));
        if (!node) {
            AIRY_LOG_WARN("reflective: node allocation failed at step %zu, skipping", i);
            continue;
        }

        char nid[256];
        snprintf(nid, sizeof(nid), "%s_%s", plan_id,
                 parsed_nodes[i].name[0] ? parsed_nodes[i].name : "task");

        node->task_node_id = AIRY_STRDUP(nid);
        if (!node->task_node_id) {
            AIRY_LOG_WARN("reflective: node id STRDUP failed at step %zu, skipping", i);
            reflective_destroy_single_node(node);
            continue;
        }
        node->task_node_agent_role =
            AIRY_STRDUP(parsed_nodes[i].role[0] ? parsed_nodes[i].role : "worker");
        if (!node->task_node_agent_role) {
            AIRY_LOG_WARN("reflective: node role STRDUP failed at step %zu, skipping", i);
            reflective_destroy_single_node(node);
            continue;
        }
        node->task_node_timeout_ms =
            parsed_nodes[i].timeout_ms > 0 ? parsed_nodes[i].timeout_ms : 30000;
        node->task_node_priority =
            parsed_nodes[i].priority > 0 ? parsed_nodes[i].priority : (int)(200 - i * 15);

        if (parsed_nodes[i].depends_on_count > 0) {
            SAFE_MALLOC_ARRAY(node->task_node_depends_on, parsed_nodes[i].depends_on_count,
                              sizeof(char *));
            if (node->task_node_depends_on) {
                node->task_node_depends_count = (uint32_t)parsed_nodes[i].depends_on_count;
                for (int d = 0; d < parsed_nodes[i].depends_on_count; d++) {
                    char dep_id[256];
                    snprintf(dep_id, sizeof(dep_id), "%s_%s", plan_id,
                             parsed_nodes[i].depends_on[d]);
                    node->task_node_depends_on[d] = AIRY_STRDUP(dep_id);
                    if (!node->task_node_depends_on[d]) {

                        for (int e = 0; e < d; e++)
                            AIRY_FREE(node->task_node_depends_on[e]);
                        AIRY_FREE(node->task_node_depends_on);
                        node->task_node_depends_on = NULL;
                        node->task_node_depends_count = 0;
                        break;
                    }
                }
            }
        }

        plan->task_plan_nodes[plan->task_plan_node_count++] = node;
    }

    if (!audit_passed) {
        airy_task_node_t *cn;
        SAFE_MALLOC_ARRAY(cn, 1, sizeof(airy_task_node_t));
        if (cn) {
            char cid[256];
            snprintf(cid, sizeof(cid), "%s_correction", plan_id);
            cn->task_node_id = AIRY_STRDUP(cid);
            cn->task_node_agent_role = AIRY_STRDUP("corrector");
            cn->task_node_timeout_ms = 20000;
            cn->task_node_priority = 250;
            if (plan->task_plan_node_count > 2) {
                SAFE_MALLOC_ARRAY(cn->task_node_depends_on, 1, sizeof(char *));
                if (cn->task_node_depends_on) {
                    cn->task_node_depends_count = 1;
                    cn->task_node_depends_on[0] = AIRY_STRDUP(
                        plan->task_plan_nodes[plan->task_plan_node_count - 1]->task_node_id);
                    if (!cn->task_node_depends_on[0]) {

                        AIRY_FREE(cn->task_node_depends_on);
                        cn->task_node_depends_on = NULL;
                        cn->task_node_depends_count = 0;
                    }
                }
            }
            plan->task_plan_nodes[plan->task_plan_node_count++] = cn;
        }
    }

    if (!aligned) {
        airy_task_node_t *rn;
        SAFE_MALLOC_ARRAY(rn, 1, sizeof(airy_task_node_t));
        if (rn) {
            char rid[256];
            snprintf(rid, sizeof(rid), "%s_realignment", plan_id);
            rn->task_node_id = AIRY_STRDUP(rid);
            rn->task_node_agent_role = AIRY_STRDUP("realigner");
            rn->task_node_timeout_ms = 15000;
            rn->task_node_priority = 254;
            if (plan->task_plan_node_count > 2) {
                SAFE_MALLOC_ARRAY(rn->task_node_depends_on, 1, sizeof(char *));
                if (rn->task_node_depends_on) {
                    rn->task_node_depends_count = 1;
                    rn->task_node_depends_on[0] = AIRY_STRDUP(
                        plan->task_plan_nodes[plan->task_plan_node_count - 1]->task_node_id);
                    if (!rn->task_node_depends_on[0]) {
                        AIRY_FREE(rn->task_node_depends_on);
                        rn->task_node_depends_on = NULL;
                        rn->task_node_depends_count = 0;
                    }
                }
            }
            plan->task_plan_nodes[plan->task_plan_node_count++] = rn;
        }
    }

    SAFE_MALLOC_ARRAY(plan->task_plan_entry_points, 1, sizeof(char *));
    if (plan->task_plan_entry_points && plan->task_plan_node_count > 0) {
        plan->task_plan_entry_count = 1;
        plan->task_plan_entry_points[0] = AIRY_STRDUP(plan->task_plan_nodes[0]->task_node_id);
    }

    if (ctx->chain && tc)
        tc->chain_stop(ctx->chain);
    *out_plan = plan;
    return AIRY_SUCCESS;
}

airy_task_plan_t *build_fallback_plan(const airy_intent_t *intent, reflective_context_t *ctx,
                                      int audit_passed, int aligned, uint64_t session_count)
{

    airy_task_plan_t *plan;
    SAFE_MALLOC_ARRAY(plan, 1, sizeof(airy_task_plan_t));
    if (!plan)
        return NULL;

    char plan_id[128];
    snprintf(plan_id, sizeof(plan_id), "fallback_%zu", session_count);
    plan->task_plan_id = AIRY_STRDUP(plan_id);
    if (!plan->task_plan_id) {
        AIRY_LOG_ERROR("reflective: fallback plan id allocation failed");
        AIRY_FREE(plan);
        return NULL;
    }

    size_t nc = 5 + (audit_passed ? 0 : 1) + (aligned ? 0 : 1);
    SAFE_MALLOC_ARRAY(plan->task_plan_nodes, nc, sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes) {
        AIRY_FREE(plan->task_plan_id);
        AIRY_FREE(plan);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    static const struct {
        const char *name;
        const char *role;
        int to;
        int pri;
    } defaults[] = {{"decompose", "analyzer", 20000, 200},
                    {"plan", "planner", 30000, 180},
                    {"execute", "executor", 45000, 160},
                    {"audit", "auditor", 15000, 240},
                    {"align", "validator", 10000, 255}};

    for (int s = 0; s < 5; s++) {
        airy_task_node_t *node;
        SAFE_MALLOC_ARRAY(node, 1, sizeof(airy_task_node_t));
        if (!node) {
            AIRY_LOG_WARN("reflective: fallback node allocation failed at step %d", s);
            break;
        }

        char nid[256];
        snprintf(nid, sizeof(nid), "%s_%s", plan_id, defaults[s].name);
        node->task_node_id = AIRY_STRDUP(nid);
        if (!node->task_node_id) {
            AIRY_LOG_WARN("reflective: fallback node id STRDUP failed at step %d", s);
            AIRY_FREE(node);
            break;
        }
        node->task_node_agent_role = AIRY_STRDUP(defaults[s].role);
        if (!node->task_node_agent_role) {
            AIRY_LOG_WARN("reflective: fallback node role STRDUP failed at step %d", s);
            AIRY_FREE(node->task_node_id);
            AIRY_FREE(node);
            break;
        }
        node->task_node_timeout_ms = defaults[s].to;
        node->task_node_priority = defaults[s].pri;

        if (s > 0) {
            SAFE_MALLOC_ARRAY(node->task_node_depends_on, 1, sizeof(char *));
            if (node->task_node_depends_on) {
                node->task_node_depends_count = 1;
                node->task_node_depends_on[0] =
                    AIRY_STRDUP(plan->task_plan_nodes[s - 1]->task_node_id);
            }
        }
        plan->task_plan_nodes[plan->task_plan_node_count++] = node;
    }

    if (!audit_passed) {
        airy_task_node_t *cn;
        SAFE_MALLOC_ARRAY(cn, 1, sizeof(airy_task_node_t));
        if (cn) {
            char cid[256];
            snprintf(cid, sizeof(cid), "%s_reaudit", plan_id);
            cn->task_node_id = AIRY_STRDUP(cid);
            cn->task_node_agent_role = AIRY_STRDUP("corrector");
            cn->task_node_timeout_ms = 20000;
            cn->task_node_priority = 250;
            SAFE_MALLOC_ARRAY(cn->task_node_depends_on, 1, sizeof(char *));
            if (cn->task_node_depends_on && plan->task_plan_node_count > 0) {
                cn->task_node_depends_count = 1;
                cn->task_node_depends_on[0] = AIRY_STRDUP(
                    plan->task_plan_nodes[plan->task_plan_node_count - 1]->task_node_id);
            }
            plan->task_plan_nodes[plan->task_plan_node_count++] = cn;
        }
    }

    if (!aligned) {
        airy_task_node_t *rn;
        SAFE_MALLOC_ARRAY(rn, 1, sizeof(airy_task_node_t));
        if (rn) {
            char rid[256];
            snprintf(rid, sizeof(rid), "%s_realign", plan_id);
            rn->task_node_id = AIRY_STRDUP(rid);
            rn->task_node_agent_role = AIRY_STRDUP("realigner");
            rn->task_node_timeout_ms = 15000;
            rn->task_node_priority = 254;
            SAFE_MALLOC_ARRAY(rn->task_node_depends_on, 1, sizeof(char *));
            if (rn->task_node_depends_on && plan->task_plan_node_count > 0) {
                rn->task_node_depends_count = 1;
                rn->task_node_depends_on[0] = AIRY_STRDUP(
                    plan->task_plan_nodes[plan->task_plan_node_count - 1]->task_node_id);
            }
            plan->task_plan_nodes[plan->task_plan_node_count++] = rn;
        }
    }

    SAFE_MALLOC_ARRAY(plan->task_plan_entry_points, 1, sizeof(char *));
    if (plan->task_plan_entry_points && plan->task_plan_node_count > 0) {
        plan->task_plan_entry_count = 1;
        plan->task_plan_entry_points[0] = AIRY_STRDUP(plan->task_plan_nodes[0]->task_node_id);
    }

    const airy_tc_ops_t *tc = are_ops_get_tc();
    if (ctx && ctx->chain && tc)
        tc->chain_stop(ctx->chain);
    return plan;
}
