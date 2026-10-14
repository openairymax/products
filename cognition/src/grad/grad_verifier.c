// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_verifier.c
 * @brief GRAD deterministic verifier implementation (model C hard-logic
 *        verification, zero generative tokens).
 *
 * Implements three deterministic checks — E-01 causal sufficiency,
 * E-02 deadlock, E-03 resource conservation — plus E-04 invariant_guard
 * counting. Core algorithms:
 *   - Kahn topological sort (E-02 cycle detection + topological order
 *     reused by E-01)
 *   - Type-signature set coverage (E-01, inputs ⊆ ∪ predecessor outputs)
 *   - Per-component cost summation vs budget (E-03)
 *
 * Conservative policy: checks with missing metadata are skipped
 * (tracked by checked_nodes/skipped_nodes).
 */

#include "grad_verifier.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

static int grad_strarr_contains(const char **arr, size_t count, const char *target)
{
    if (!arr || !target)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (arr[i] && strcmp(arr[i], target) == 0)
            return 1;
    }
    return 0;
}

static int grad_find_node_index(const airy_task_plan_t *plan, const char *node_id)
{
    if (!plan || !node_id)
        return -1;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        if (plan->task_plan_nodes[i] && plan->task_plan_nodes[i]->task_node_id &&
            strcmp(plan->task_plan_nodes[i]->task_node_id, node_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Kahn topological sort (deterministic deadlock detection).
 *
 * @param plan [in] Task plan
 * @param topo_order [out] Topological-order node index array (capacity=n, caller AIRY_FREE)
 * @param topo_count [out] Topological-order length (< n means a cycle exists)
 * @param cycle_path [out] Cycle path string (filled when a cycle is found; may be NULL)
 * @return 0 acyclic; -1 cycle exists
 */
static int grad_kahn_topo(const airy_task_plan_t *plan, int **out_topo_order,
                          size_t *out_topo_count, char *cycle_path, size_t cycle_cap)
{
    size_t n = plan->task_plan_node_count;
    if (n == 0) {
        *out_topo_order = NULL;
        *out_topo_count = 0;
        return 0;
    }

    size_t *indeg = (size_t *)AIRY_CALLOC(n, sizeof(size_t));
    size_t **adj = (size_t **)AIRY_CALLOC(n, sizeof(size_t *));
    size_t *adj_cap = (size_t *)AIRY_CALLOC(n, sizeof(size_t));
    size_t *adj_cnt = (size_t *)AIRY_CALLOC(n, sizeof(size_t));
    int *topo = (int *)AIRY_MALLOC(n * sizeof(int));
    if (!indeg || !adj || !adj_cap || !adj_cnt || !topo) {
        if (indeg)
            AIRY_FREE(indeg);
        if (adj)
            AIRY_FREE(adj);
        if (adj_cap)
            AIRY_FREE(adj_cap);
        if (adj_cnt)
            AIRY_FREE(adj_cnt);
        if (topo)
            AIRY_FREE(topo);
        return -2; /* ENOMEM */
    }

    for (size_t v = 0; v < n; v++) {
        airy_task_node_t *node = plan->task_plan_nodes[v];
        if (!node)
            continue;
        for (size_t d = 0; d < node->task_node_depends_count; d++) {
            int pred_idx = grad_find_node_index(plan, node->task_node_depends_on[d]);
            if (pred_idx < 0)
                continue;
            indeg[v]++;
            if (adj_cnt[pred_idx] >= adj_cap[pred_idx]) {
                size_t new_cap = adj_cap[pred_idx] ? adj_cap[pred_idx] * 2 : 4;
                size_t *new_adj = (size_t *)AIRY_REALLOC(adj[pred_idx], new_cap * sizeof(size_t));
                if (!new_adj) {

                    for (size_t k = 0; k < n; k++)
                        if (adj[k])
                            AIRY_FREE(adj[k]);
                    AIRY_FREE(adj);
                    AIRY_FREE(adj_cap);
                    AIRY_FREE(adj_cnt);
                    AIRY_FREE(indeg);
                    AIRY_FREE(topo);
                    return -2;
                }
                adj[pred_idx] = new_adj;
                adj_cap[pred_idx] = new_cap;
            }
            adj[pred_idx][adj_cnt[pred_idx]++] = v;
        }
    }

    size_t queue_cap = n;
    size_t *queue = (size_t *)AIRY_MALLOC(queue_cap * sizeof(size_t));
    if (!queue) {
        for (size_t k = 0; k < n; k++)
            if (adj[k])
                AIRY_FREE(adj[k]);
        AIRY_FREE(adj);
        AIRY_FREE(adj_cap);
        AIRY_FREE(adj_cnt);
        AIRY_FREE(indeg);
        AIRY_FREE(topo);
        return -2;
    }
    size_t head = 0, tail = 0;
    for (size_t v = 0; v < n; v++) {
        if (indeg[v] == 0)
            queue[tail++] = v;
    }

    size_t processed = 0;
    while (head < tail) {
        size_t u = queue[head++];
        topo[processed++] = (int)u;
        for (size_t k = 0; k < adj_cnt[u]; k++) {
            size_t v = adj[u][k];
            if (--indeg[v] == 0)
                queue[tail++] = v;
        }
    }

    int has_cycle = (processed < n) ? -1 : 0;
    if (has_cycle && cycle_path && cycle_cap > 0) {

        size_t cycle_node = 0;
        for (size_t v = 0; v < n; v++) {
            if (indeg[v] > 0) {
                cycle_node = v;
                break;
            }
        }

        char tmp[1024];
        size_t off = 0;
        size_t cur = cycle_node;
        size_t visited_guard = 0;
        int cycle_closed = 0;
        while (visited_guard++ < n) {
            airy_task_node_t *node = plan->task_plan_nodes[cur];
            if (!node || !node->task_node_id)
                break;
            int wrote = snprintf(tmp + off, sizeof(tmp) - off, "%s%s", off ? " -> " : "",
                                 node->task_node_id);
            if (wrote <= 0 || off + (size_t)wrote >= sizeof(tmp))
                break;
            off += (size_t)wrote;

            int next = -1;
            for (size_t d = 0; d < node->task_node_depends_count; d++) {
                int pi = grad_find_node_index(plan, node->task_node_depends_on[d]);
                if (pi >= 0 && indeg[pi] > 0) {
                    next = pi;
                    break;
                }
            }
            if (next < 0)
                break;
            if (next == (int)cycle_node) {

                char tmp2[1536];
                snprintf(tmp2, sizeof(tmp2), "%s -> %s", tmp,
                         plan->task_plan_nodes[cycle_node]->task_node_id);
                if (strlen(tmp2) < cycle_cap) {
                    snprintf(cycle_path, cycle_cap, "%s", tmp2);
                    cycle_closed = 1;
                }
                break;
            }
            cur = (size_t)next;
        }
        /* 兜底仅当闭环未闭合时使用（walk 路径缺回边，不得覆盖完整闭环） */
        if (!cycle_closed && off < cycle_cap)
            snprintf(cycle_path, cycle_cap, "%s", tmp);
    }

    for (size_t k = 0; k < n; k++)
        if (adj[k])
            AIRY_FREE(adj[k]);
    AIRY_FREE(adj);
    AIRY_FREE(adj_cap);
    AIRY_FREE(adj_cnt);
    AIRY_FREE(indeg);
    AIRY_FREE(queue);

    if (has_cycle) {
        AIRY_FREE(topo);
        *out_topo_order = NULL;
        *out_topo_count = 0;
        return -1;
    }

    *out_topo_order = topo;
    *out_topo_count = processed;
    return 0;
}

static int grad_in_closure(const int *closure, size_t closure_count, int idx);

/**
 * @brief E-01 causal sufficiency check: for each node (inside the closure),
 *        inputs ⊆ outputs of all predecessors.
 *
 * Maintains the "produced signature set" (union of all predecessor
 * outputs) along topological order and checks that every input of
 * closure nodes is covered. Strict judgment applies only to closure
 * nodes with complete input metadata; out-of-closure nodes are treated
 * as "proven theorems" and skipped (differential entropy reduction).
 *
 * @param plan [in] Task plan
 * @param topo [in] Topological-order node index array
 * @param topo_count [in] Topological-order length
 * @param closure [in] Differential closure (NULL = full verification)
 * @param closure_count [in] Closure size
 * @param report [out] Report (fills E-01 result)
 * @return 0 pass/skip; -1 logic break found (report filled)
 */
static int grad_check_causal(const airy_task_plan_t *plan, const int *topo, size_t topo_count,
                             const int *closure, size_t closure_count, airy_grad_report_t *report)
{

    size_t prod_cap = 32;
    char **produced = (char **)AIRY_CALLOC(prod_cap, sizeof(char *));
    if (!produced)
        return -2;
    size_t produced_count = 0;

    int result = 0;

    for (size_t t = 0; t < topo_count && result == 0; t++) {
        airy_task_node_t *node = plan->task_plan_nodes[topo[t]];
        if (!node)
            continue;

        if (node->task_node_outputs && node->task_node_outputs_count > 0) {
            for (size_t o = 0; o < node->task_node_outputs_count; o++) {
                const char *sig = node->task_node_outputs[o];
                if (!sig)
                    continue;
                if (!grad_strarr_contains((const char **)produced, produced_count, sig)) {
                    if (produced_count >= prod_cap) {
                        size_t new_cap = prod_cap * 2;
                        char **new_arr = (char **)AIRY_REALLOC(produced, new_cap * sizeof(char *));
                        if (!new_arr) {
                            for (size_t k = 0; k < produced_count; k++)
                                AIRY_FREE(produced[k]);
                            AIRY_FREE(produced);
                            return -2;
                        }
                        produced = new_arr;
                        prod_cap = new_cap;
                    }
                    produced[produced_count] = AIRY_STRDUP(sig);
                    if (produced[produced_count])
                        produced_count++;
                }
            }
        }

        if (closure && closure_count > 0 && !grad_in_closure(closure, closure_count, topo[t])) {
            report->skipped_nodes++;
            continue;
        }

        if (!node->task_node_inputs || node->task_node_inputs_count == 0) {
            report->skipped_nodes++;
            continue;
        }
        report->checked_nodes++;

        for (size_t i = 0; i < node->task_node_inputs_count; i++) {
            const char *input_sig = node->task_node_inputs[i];
            if (!input_sig)
                continue;
            if (!grad_strarr_contains((const char **)produced, produced_count, input_sig)) {

                snprintf(report->affected_node, sizeof(report->affected_node), "%s",
                         node->task_node_id ? node->task_node_id : "?");
                snprintf(report->missing_artifact, sizeof(report->missing_artifact), "%s",
                         input_sig);
                report->error = AIRY_GRAD_E01_CAUSAL_BREAK;
                result = -1;
                break;
            }
        }
    }

    for (size_t k = 0; k < produced_count; k++)
        AIRY_FREE(produced[k]);
    AIRY_FREE(produced);
    return result;
}

/**
 * @brief E-03 resource conservation check: Σ cost(v_i) ≤ R_total (per component).
 *
 * Sums only nodes with cost > 0 (cost=0 is treated as unknown; excluded
 * from the sum but counted as skipped).
 *
 * @param plan [in] Task plan
 * @param budget [in] Resource budget
 * @param report [out] Report (fills E-03 result)
 * @return 0 pass/skip; -1 over budget (report filled)
 */
static int grad_check_resource(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                               airy_grad_report_t *report)
{
    if (!budget || (budget->time_ms <= 0 && budget->mem_mb <= 0)) {

        return 0;
    }

    int64_t sum_ms = 0, sum_mb = 0;
    uint32_t costed_nodes = 0, unknown_nodes = 0;

    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        airy_task_node_t *node = plan->task_plan_nodes[i];
        if (!node)
            continue;
        int has_cost = (node->task_node_cost_time_ms > 0) || (node->task_node_cost_mem_mb > 0);
        if (!has_cost) {
            unknown_nodes++;
            continue;
        }
        if (node->task_node_cost_time_ms > 0)
            sum_ms += node->task_node_cost_time_ms;
        if (node->task_node_cost_mem_mb > 0)
            sum_mb += node->task_node_cost_mem_mb;
        costed_nodes++;
    }

    report->cost_sum_ms = sum_ms;
    report->cost_sum_mb = sum_mb;
    report->checked_nodes += costed_nodes;
    report->skipped_nodes += unknown_nodes;

    if (budget->time_ms > 0 && sum_ms > budget->time_ms) {
        snprintf(report->affected_node, sizeof(report->affected_node), "aggregate");
        report->error = AIRY_GRAD_E03_RESOURCE_OVER;
        return -1;
    }
    if (budget->mem_mb > 0 && sum_mb > budget->mem_mb) {
        snprintf(report->affected_node, sizeof(report->affected_node), "aggregate");
        report->error = AIRY_GRAD_E03_RESOURCE_OVER;
        return -1;
    }
    return 0;
}

airy_err_t airy_grad_verify_plan(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                                 airy_grad_report_t *report)
{
    return airy_grad_verify_scope(plan, budget, NULL, 0, report);
}

static int grad_in_closure(const int *closure, size_t closure_count, int idx)
{
    for (size_t i = 0; i < closure_count; i++) {
        if (closure[i] == idx)
            return 1;
    }
    return 0;
}

/**
 * @brief Compute the differential verification closure: scope nodes +
 *        their dependency predecessors + their direct successors.
 *
 * Closure rules (GRAD V3.0 §6.4 first-order closure):
 *   - the changed nodes themselves
 *   - all dependency predecessors of changed nodes (verify input source chain)
 *   - direct successors of changed nodes (verify downstream consumers
 *     are not broken)
 * Used only to trim the node scope for E-01/E-03; E-02 cycle detection
 * needs the whole graph (kept full).
 *
 * @param plan [in] Task plan
 * @param scope_nodes [in] Changed node ID array
 * @param scope_count [in] Number of changed nodes
 * @param out_closure [out] Closure node index array (AIRY_MALLOC, caller frees)
 * @param out_count [out] Closure size
 * @return 0 success; -1 invalid arguments
 */
static int grad_compute_closure(const airy_task_plan_t *plan, const char *const *scope_nodes,
                                size_t scope_count, int **out_closure, size_t *out_count)
{
    size_t n = plan->task_plan_node_count;
    if (n == 0) {
        *out_closure = NULL;
        *out_count = 0;
        return 0;
    }

    int *closure = (int *)AIRY_CALLOC(n, sizeof(int));
    int *in_set = (int *)AIRY_CALLOC(n, sizeof(int));
    if (!closure || !in_set) {
        if (closure)
            AIRY_FREE(closure);
        if (in_set)
            AIRY_FREE(in_set);
        return -2;
    }
    size_t count = 0;

    for (size_t s = 0; s < scope_count; s++) {
        int idx = grad_find_node_index(plan, scope_nodes[s]);
        if (idx < 0 || in_set[idx])
            continue;
        in_set[idx] = 1;
        closure[count++] = idx;
    }

    int changed = 1;
    while (changed) {
        changed = 0;
        for (size_t i = 0; i < count; i++) {
            airy_task_node_t *node = plan->task_plan_nodes[closure[i]];
            if (!node)
                continue;
            for (size_t d = 0; d < node->task_node_depends_count; d++) {
                int pi = grad_find_node_index(plan, node->task_node_depends_on[d]);
                if (pi >= 0 && !in_set[pi]) {
                    in_set[pi] = 1;
                    closure[count++] = pi;
                    changed = 1;
                }
            }
        }
    }

    for (size_t v = 0; v < n; v++) {
        airy_task_node_t *node = plan->task_plan_nodes[v];
        if (!node || in_set[v])
            continue;
        for (size_t d = 0; d < node->task_node_depends_count; d++) {
            int pi = grad_find_node_index(plan, node->task_node_depends_on[d]);
            if (pi >= 0 && in_set[pi]) {
                in_set[v] = 1;
                closure[count++] = (int)v; /* size_t→int 收窄显式化（C4267） */
                break;
            }
        }
    }

    AIRY_FREE(in_set);
    *out_closure = closure;
    *out_count = count;
    return 0;
}

airy_err_t airy_grad_verify_scope(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                                  const char *const *scope_nodes, size_t scope_count,
                                  airy_grad_report_t *report)
{
    if (!plan || !report)
        return AIRY_EINVAL;

    __builtin_memset(report, 0, sizeof(airy_grad_report_t));

    if (plan->task_plan_node_count == 0 || !plan->task_plan_nodes)
        return AIRY_SUCCESS;

    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        if (plan->task_plan_nodes[i] && plan->task_plan_nodes[i]->task_node_invariant_guard)
            report->invariant_guards++;
    }

    int *topo = NULL;
    size_t topo_count = 0;
    int kahn_ret =
        grad_kahn_topo(plan, &topo, &topo_count, report->cycle_path, sizeof(report->cycle_path));
    if (kahn_ret == -2)
        return AIRY_ENOMEM;
    if (kahn_ret == -1) {
        report->error = AIRY_GRAD_E02_DEADLOCK;
        return AIRY_SUCCESS;
    }

    int *closure = NULL;
    size_t closure_count = 0;
    if (scope_nodes && scope_count > 0) {
        int cl_ret = grad_compute_closure(plan, scope_nodes, scope_count, &closure, &closure_count);
        if (cl_ret == -2) {
            AIRY_FREE(topo);
            return AIRY_ENOMEM;
        }

        if (closure_count == 0) {
            AIRY_FREE(closure);
            closure = NULL;
        }
    }

    int causal_ret = grad_check_causal(plan, topo, topo_count, closure, closure_count, report);
    AIRY_FREE(topo);
    if (causal_ret == -2) {
        if (closure)
            AIRY_FREE(closure);
        return AIRY_ENOMEM;
    }
    if (causal_ret == -1) {
        if (closure)
            AIRY_FREE(closure);

        return AIRY_SUCCESS;
    }
    if (closure)
        AIRY_FREE(closure);

    int res_ret = grad_check_resource(plan, budget, report);
    if (res_ret == -1) {

        return AIRY_SUCCESS;
    }

    /* E-05 verification-insufficient (false-convergence defense): a
     * non-empty plan where no node was strictly checked (all inputs/outputs/
     * cost metadata absent) must not report OK. Previously such an
     * "empty-shell" LLM plan skipped every E-01/E-03 check, produced
     * checked_nodes=0, and still converged — the gate accepted a plan it
     * never verified. Reject so model A must fill metadata (patch E-05). */
    if (plan->task_plan_node_count > 0 && report->checked_nodes == 0) {
        snprintf(report->affected_node, sizeof(report->affected_node), "all");
        report->error = AIRY_GRAD_E05_VERIFY_INSUFFICIENT;
        return AIRY_SUCCESS;
    }

    report->error = AIRY_GRAD_OK;
    return AIRY_SUCCESS;
}

const char *airy_grad_error_str(airy_grad_error_t err)
{
    switch (err) {
    case AIRY_GRAD_OK:
        return "OK";
    case AIRY_GRAD_E01_CAUSAL_BREAK:
        return "E-01 causal break: input not produced by any predecessor";
    case AIRY_GRAD_E02_DEADLOCK:
        return "E-02 deadlock: dependency cycle detected (not a DAG)";
    case AIRY_GRAD_E03_RESOURCE_OVER:
        return "E-03 resource overrun: aggregate cost exceeds budget";
    case AIRY_GRAD_E05_VERIFY_INSUFFICIENT:
        return "E-05 verify insufficient: no node carries checkable metadata";
    default:
        return "UNKNOWN";
    }
}

airy_err_t airy_grad_report_to_json(const airy_grad_report_t *report, char **out_json)
{
    if (!report || !out_json)
        return AIRY_EINVAL;
    *out_json = NULL;

#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return AIRY_ENOMEM;
    cJSON_AddNumberToObject(root, "error", (double)report->error);
    cJSON_AddStringToObject(root, "error_desc", airy_grad_error_str(report->error));
    cJSON_AddStringToObject(root, "affected_node", report->affected_node);
    cJSON_AddStringToObject(root, "missing_artifact", report->missing_artifact);
    cJSON_AddStringToObject(root, "cycle_path", report->cycle_path);
    cJSON_AddNumberToObject(root, "cost_sum_ms", (double)report->cost_sum_ms);
    cJSON_AddNumberToObject(root, "cost_sum_mb", (double)report->cost_sum_mb);
    cJSON_AddNumberToObject(root, "checked_nodes", (double)report->checked_nodes);
    cJSON_AddNumberToObject(root, "skipped_nodes", (double)report->skipped_nodes);
    cJSON_AddNumberToObject(root, "invariant_guards", (double)report->invariant_guards);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return AIRY_ENOMEM;
    *out_json = json;
    return AIRY_SUCCESS;
#else

    size_t cap = 512;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return AIRY_ENOMEM;
    int n = snprintf(buf, cap,
                     "{\"error\":%d,\"error_desc\":\"%s\",\"affected_node\":\"%s\","
                     "\"cost_sum_ms\":%lld,\"cost_sum_mb\":%lld,\"checked_nodes\":%u,"
                     "\"skipped_nodes\":%u,\"invariant_guards\":%u}",
                     (int)report->error, airy_grad_error_str(report->error), report->affected_node,
                     (long long)report->cost_sum_ms, (long long)report->cost_sum_mb,
                     (unsigned)report->checked_nodes, (unsigned)report->skipped_nodes,
                     (unsigned)report->invariant_guards);
    if (n <= 0 || (size_t)n >= cap) {
        AIRY_FREE(buf);
        return AIRY_EUNKNOWN;
    }
    *out_json = buf;
    return AIRY_SUCCESS;
#endif
}
