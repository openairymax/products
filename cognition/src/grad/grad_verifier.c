// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_verifier.c
 * @brief GRAD deterministic verifier implementation (model C hard-logic
 *        verification, zero generative tokens).
 *
 * Implements the five-way deterministic check (0.1.20 C5):
 *   - E-01 causal sufficiency: Inputs(v_i) ⊆ ∪ Outputs(predecessors),
 *     three input forms — literal ("lit:" prefix), composite
 *     "{producer-id}.{artifact}" (producer must be in the transitive
 *     depends closure and export the artifact), bare signature
 *     (exact match against the produced set)
 *   - E-02 deadlock: Kahn topological sort (order reused by E-01)
 *   - E-03 resource conservation: Σ cost ≤ budget
 *   - E-04 purpose-drift guard (deterministic, C5): with a goal, every
 *     invariant_guard node's outputs must overlap the goal keywords
 *     (bidirectional substring match); goal NULL degrades to counting
 *   - E-05 metadata coverage (any-semantics): participating nodes with
 *     incomplete metadata (outputs/cost/input-completeness) fail the
 *     plan — empty-shell plans can no longer report OK
 *
 * Every check sets its bit in report->errors; report->error holds the
 * single highest-priority verdict (E-02 > E-05 > E-01 > E-03 > E-04).
 * Causal walk checks each node's inputs before merging its own outputs
 * into the produced set (self-reference cannot pass), and out-of-closure
 * nodes still donate their outputs (supplier semantics).
 */

#include "grad_verifier.h"
#include "airy_memory.h"
#include "string_compat.h"
#include "text_utils.h"

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
 * @brief Merge a node's outputs into the produced signature set (dedup).
 * @return 0 success; -2 ENOMEM (set left intact for cleanup)
 */
static int grad_merge_outputs(const airy_task_node_t *node, char ***produced, size_t *count,
                              size_t *cap)
{
    for (size_t o = 0; o < node->task_node_outputs_count; o++) {
        const char *sig = node->task_node_outputs[o];
        if (!sig)
            continue;
        if (grad_strarr_contains((const char **)*produced, *count, sig))
            continue;
        if (*count >= *cap) {
            size_t new_cap = *cap * 2;
            char **new_arr = (char **)AIRY_REALLOC(*produced, new_cap * sizeof(char *));
            if (!new_arr)
                return -2;
            *produced = new_arr;
            *cap = new_cap;
        }
        (*produced)[*count] = AIRY_STRDUP(sig);
        if (!(*produced)[*count])
            return -2;
        (*count)++;
    }
    return 0;
}

/**
 * @brief Transitive depends-closure reachability (BFS, excludes self).
 * @return 1 reachable; 0 not; -2 ENOMEM
 */
static int grad_dep_reachable(const airy_task_plan_t *plan, size_t from_idx, int target_idx)
{
    size_t n = plan->task_plan_node_count;
    int *visited = (int *)AIRY_CALLOC(n, sizeof(int));
    size_t *queue = (size_t *)AIRY_MALLOC(n * sizeof(size_t));
    if (!visited || !queue) {
        if (visited)
            AIRY_FREE(visited);
        if (queue)
            AIRY_FREE(queue);
        return -2;
    }
    size_t head = 0, tail = 0;
    visited[from_idx] = 1;
    queue[tail++] = from_idx;
    int found = 0;
    while (head < tail && !found) {
        airy_task_node_t *node = plan->task_plan_nodes[queue[head++]];
        if (!node)
            continue;
        for (size_t d = 0; d < node->task_node_depends_count; d++) {
            int pi = grad_find_node_index(plan, node->task_node_depends_on[d]);
            if (pi < 0 || visited[pi])
                continue;
            if (pi == target_idx) {
                found = 1;
                break;
            }
            visited[pi] = 1;
            queue[tail++] = (size_t)pi;
        }
    }
    AIRY_FREE(visited);
    AIRY_FREE(queue);
    return found;
}

/**
 * @brief Find the node whose id equals input[0..pid_len) (producer prefix).
 * @return node index; -1 not found
 */
static int grad_find_producer_index(const airy_task_plan_t *plan, const char *input,
                                    size_t pid_len)
{
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        airy_task_node_t *node = plan->task_plan_nodes[i];
        if (!node || !node->task_node_id)
            continue;
        if (strncmp(node->task_node_id, input, pid_len) == 0 &&
            node->task_node_id[pid_len] == '\0')
            return (int)i;
    }
    return -1;
}

/**
 * @brief E-01 single-input judgment (three forms).
 *
 *   - "lit:" prefix: literal value, exempt (always satisfied)
 *   - "{pid}.{name}": producer must be in the current node's transitive
 *     depends closure (a node cannot consume its own artifact — the
 *     declared-dependency graph and the dataflow graph must agree) and
 *     its outputs must contain the exact artifact name
 *   - bare signature: exact match in the produced set
 *
 * @return 1 satisfied; 0 causal break; -2 ENOMEM
 */
static int grad_input_satisfied(const airy_task_plan_t *plan, size_t node_idx, const char *input,
                                const char **produced, size_t produced_count)
{
    if (strncmp(input, "lit:", 4) == 0)
        return 1;
    const char *dot = strchr(input, '.');
    if (dot && dot != input) {
        size_t pid_len = (size_t)(dot - input);
        int prod_idx = grad_find_producer_index(plan, input, pid_len);
        if (prod_idx < 0)
            return 0;
        int reach = grad_dep_reachable(plan, node_idx, prod_idx);
        if (reach <= 0)
            return (reach == 0) ? 0 : -2;
        airy_task_node_t *producer = plan->task_plan_nodes[prod_idx];
        const char *name = dot + 1;
        for (size_t o = 0; o < producer->task_node_outputs_count; o++) {
            if (producer->task_node_outputs[o] &&
                strcmp(producer->task_node_outputs[o], name) == 0)
                return 1;
        }
        return 0;
    }
    return grad_strarr_contains(produced, produced_count, input);
}

/**
 * @brief E-01 causal sufficiency check (0.1.20 C5 rewrite).
 *
 * Walks the topological order. Each participating node is checked
 * BEFORE its own outputs join the produced set — a self-referential
 * plan can no longer satisfy its inputs from itself. Out-of-closure
 * nodes are not checked (differential entropy reduction) but still
 * donate their outputs (supplier semantics). Empty-input nodes count
 * as trivially checked (metadata completeness is E-05's domain).
 * All participating nodes are visited even after a break (the first
 * break fills the report detail); causal_checked therefore equals the
 * participating-set size and E-05 is driven solely by metadata_ok.
 *
 * @param plan [in] Task plan
 * @param topo [in] Topological-order node index array
 * @param topo_count [in] Topological-order length
 * @param closure [in] Differential closure (NULL = full verification)
 * @param closure_count [in] Closure size
 * @param report [out] Report (fills errors bit, causal_checked, metadata_ok)
 * @return 0 pass; -1 causal break (report filled); -2 ENOMEM
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

    for (size_t t = 0; t < topo_count; t++) {
        airy_task_node_t *node = plan->task_plan_nodes[topo[t]];
        if (!node)
            continue;

        if (closure && closure_count > 0 && !grad_in_closure(closure, closure_count, topo[t])) {
            /* 闭包外供体：不检查，但 outputs 并入 produced（供体语义） */
            if (grad_merge_outputs(node, &produced, &produced_count, &prod_cap) == -2) {
                result = -2;
                break;
            }
            report->skipped_nodes++;
            continue;
        }

        report->causal_checked++;
        for (size_t i = 0; i < node->task_node_inputs_count; i++) {
            const char *input_sig = node->task_node_inputs[i];
            if (!input_sig)
                continue;
            int sat = grad_input_satisfied(plan, topo[t], input_sig,
                                           (const char **)produced, produced_count);
            if (sat == -2) {
                result = -2;
                break;
            }
            if (sat == 0) {
                if (!(report->errors & AIRY_GRAD_ERR_BIT(AIRY_GRAD_E01_CAUSAL_BREAK))) {
                    snprintf(report->affected_node, sizeof(report->affected_node), "%s",
                             node->task_node_id ? node->task_node_id : "?");
                    snprintf(report->missing_artifact, sizeof(report->missing_artifact), "%s",
                             input_sig);
                    report->errors |= AIRY_GRAD_ERR_BIT(AIRY_GRAD_E01_CAUSAL_BREAK);
                }
                result = -1;
                break;
            }
        }
        if (result == -2)
            break;

        /* metadata_ok（E-05 覆盖统计，参与集内）：产出非空 ∧ 有成本信息
         * ∧ 输入完备（无依赖免输入；有依赖必须声明 inputs，堵
         * "依赖声明/数据流两张皮"）。 */
        if (node->task_node_outputs_count > 0 &&
            (node->task_node_cost_time_ms > 0 || node->task_node_cost_mem_mb > 0) &&
            (node->task_node_depends_count == 0 || node->task_node_inputs_count > 0))
            report->metadata_ok++;

        if (grad_merge_outputs(node, &produced, &produced_count, &prod_cap) == -2) {
            result = -2;
            break;
        }
    }

    for (size_t k = 0; k < produced_count; k++)
        AIRY_FREE(produced[k]);
    AIRY_FREE(produced);
    report->checked_nodes = report->causal_checked; /* 兼容别名 */
    return result;
}

/**
 * @brief E-03 resource conservation check: Σ cost(v_i) ≤ R_total.
 *
 * Sums only nodes with cost > 0 (cost=0 is treated as unknown; excluded
 * from the sum, counted separately as cost_checked/unknown). The E-01
 * counter checked_nodes is no longer polluted (0.1.20 C5).
 *
 * @param plan [in] Task plan
 * @param budget [in] Resource budget
 * @param report [out] Report (fills errors bit, cost_checked, cost sums)
 * @return 0 pass/skip; -1 over budget (report filled)
 */
static int grad_check_resource(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                               const int *closure, size_t closure_count, airy_grad_report_t *report)
{
    if (!budget || (budget->time_ms <= 0 && budget->mem_mb <= 0))
        return 0;

    int64_t sum_ms = 0, sum_mb = 0;
    uint32_t costed_nodes = 0, unknown_nodes = 0;

    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        /* F9（0.1.20 C6）：差分模式下 E-03 只累计闭包内节点的资源——
         * 闭包外节点视为已证定理（§2.5 微分熵减），其成本已在上轮
         * 预算判定中结清，重复累计会令小修补被全图旧账误判超限。 */
        if (closure && !grad_in_closure(closure, closure_count, (int)i))
            continue;
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
    report->cost_checked = costed_nodes;
    report->skipped_nodes += unknown_nodes;

    if ((budget->time_ms > 0 && sum_ms > budget->time_ms) ||
        (budget->mem_mb > 0 && sum_mb > budget->mem_mb)) {
        snprintf(report->affected_node, sizeof(report->affected_node), "aggregate");
        report->errors |= AIRY_GRAD_ERR_BIT(AIRY_GRAD_E03_RESOURCE_OVER);
        return -1;
    }
    return 0;
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
 * @return 0 success; -1 invalid arguments; -2 ENOMEM
 */
static int grad_closure_impl(const airy_task_plan_t *plan, const char *const *scope_nodes,
                             size_t scope_count, int **out_closure, size_t *out_count);

airy_err_t grad_compute_closure(const airy_task_plan_t *plan, const char *const *scope_nodes,
                                size_t scope_count, int **out_closure, size_t *out_count)
{
    if (!plan || !out_closure || !out_count)
        return AIRY_EINVAL;
    *out_closure = NULL;
    *out_count = 0;
    return grad_closure_impl(plan, scope_nodes, scope_count, out_closure, out_count) == 0
               ? AIRY_SUCCESS
               : AIRY_ENOMEM;
}

static int grad_closure_impl(const airy_task_plan_t *plan, const char *const *scope_nodes,
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

    /* 一阶闭包：scope + 传递前驱 + scope 的直接后继。后继扩展必须以
     * scope 原集合为准（先快照），否则前驱的兄弟节点与同趟级联都会被
     * 吸进来，修补域被无谓放大。 */
    int *snap = (int *)AIRY_CALLOC(n, sizeof(int));
    if (!snap) {
        AIRY_FREE(closure);
        AIRY_FREE(in_set);
        return -2;
    }
    AIRY_MEMCPY(snap, in_set, n * sizeof(int));

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
            if (pi >= 0 && snap[pi]) {
                in_set[v] = 1;
                closure[count++] = (int)v; /* size_t→int 收窄显式化（C4267） */
                break;
            }
        }
    }

    AIRY_FREE(snap);
    AIRY_FREE(in_set);
    *out_closure = closure;
    *out_count = count;
    return 0;
}

/**
 * @brief E-04 purpose-drift guard check (deterministic, 0.1.20 C5).
 *
 * Extracts lowercased keywords from the goal endpoint text and requires
 * every invariant_guard node's outputs to share entity overlap with at
 * least one keyword (bidirectional substring match). A guard node whose
 * outputs mention nothing the goal mentions protects entities the goal
 * never asked for — purpose drift. Without a goal (or without keywords
 * or guard nodes) the check is not applicable: guard_state = NONE, and
 * only invariant_guards counting remains (semantic layer B still owns
 * the fine-grained e04=pass|fail verdict).
 *
 * Whole-graph domain (E-03 alike): drift is a plan-level property, so
 * scope mode still inspects every guard node (fail-closed).
 */
#define GUARD_KW_MAX 24

static void grad_check_guard(const airy_task_plan_t *plan, const airy_gccp_goal_t *goal,
                             airy_grad_report_t *report)
{
    report->guard_state = AIRY_GRAD_GUARD_NONE;
    if (!goal || !goal->goal_endpoint || report->invariant_guards == 0)
        return;
    char *keywords[GUARD_KW_MAX];
    size_t kw_count = airy_text_keywords(goal->goal_endpoint, keywords, GUARD_KW_MAX);
    if (kw_count == 0)
        return;
    int fail = 0;
    for (size_t i = 0; i < plan->task_plan_node_count && !fail; i++) {
        airy_task_node_t *node = plan->task_plan_nodes[i];
        if (!node || !node->task_node_invariant_guard)
            continue;
        int overlap = 0;
        for (size_t o = 0; o < node->task_node_outputs_count && !overlap; o++) {
            const char *sig = node->task_node_outputs[o];
            if (!sig || !sig[0])
                continue;
            for (size_t k = 0; k < kw_count; k++) {
                if (strstr(sig, keywords[k]) || strstr(keywords[k], sig)) {
                    overlap = 1;
                    break;
                }
            }
        }
        if (!overlap) {
            snprintf(report->affected_node, sizeof(report->affected_node), "%s",
                     node->task_node_id ? node->task_node_id : "?");
            report->errors |= AIRY_GRAD_ERR_BIT(AIRY_GRAD_E04_GUARD_DRIFT);
            fail = 1;
        }
    }
    airy_text_kw_free(keywords, kw_count);
    report->guard_state = fail ? AIRY_GRAD_GUARD_FAIL : AIRY_GRAD_GUARD_PASS;
}

/**
 * @brief Shared verification pipeline (full or scope, with optional goal).
 *
 * Execution order E-02 → E-01 → E-03 → E-04 → E-05 fills the errors
 * bitmap and the report detail of the first hit; the single-value
 * verdict is then chosen by priority E-02 > E-05 > E-01 > E-03 > E-04
 * (metadata completeness gates causal judgment — complete the metadata
 * first, then judge causality).
 */
static airy_err_t grad_verify_impl(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                                   const airy_gccp_goal_t *goal, const char *const *scope_nodes,
                                   size_t scope_count, airy_grad_report_t *report)
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
        report->errors |= AIRY_GRAD_ERR_BIT(AIRY_GRAD_E02_DEADLOCK);
        report->error = AIRY_GRAD_E02_DEADLOCK;
        return AIRY_SUCCESS;
    }

    int *closure = NULL;
    size_t closure_count = 0;
    if (scope_nodes && scope_count > 0) {
        airy_err_t cl_ret =
            grad_compute_closure(plan, scope_nodes, scope_count, &closure, &closure_count);
        if (cl_ret != AIRY_SUCCESS) {
            AIRY_FREE(topo);
            return cl_ret;
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

    grad_check_resource(plan, budget, closure, closure_count, report);
    grad_check_guard(plan, goal, report);

    /* E-05 元数据覆盖（any 语义，J-05 等式）：参与集内任一节点元数据
     * 不全即拒。因果遍历恒全覆盖参与集，causal 侧条件防御保留。 */
    size_t participating = 0;
    if (closure) {
        participating = closure_count;
    } else {
        for (size_t i = 0; i < plan->task_plan_node_count; i++)
            if (plan->task_plan_nodes[i])
                participating++;
    }
    if (report->causal_checked < participating || report->metadata_ok < participating) {
        report->errors |= AIRY_GRAD_ERR_BIT(AIRY_GRAD_E05_VERIFY_INSUFFICIENT);
        if (report->affected_node[0] == '\0')
            snprintf(report->affected_node, sizeof(report->affected_node), "all");
    }
    if (closure)
        AIRY_FREE(closure);

    if (report->errors & AIRY_GRAD_ERR_BIT(AIRY_GRAD_E02_DEADLOCK))
        report->error = AIRY_GRAD_E02_DEADLOCK;
    else if (report->errors & AIRY_GRAD_ERR_BIT(AIRY_GRAD_E05_VERIFY_INSUFFICIENT))
        report->error = AIRY_GRAD_E05_VERIFY_INSUFFICIENT;
    else if (report->errors & AIRY_GRAD_ERR_BIT(AIRY_GRAD_E01_CAUSAL_BREAK))
        report->error = AIRY_GRAD_E01_CAUSAL_BREAK;
    else if (report->errors & AIRY_GRAD_ERR_BIT(AIRY_GRAD_E03_RESOURCE_OVER))
        report->error = AIRY_GRAD_E03_RESOURCE_OVER;
    else if (report->errors & AIRY_GRAD_ERR_BIT(AIRY_GRAD_E04_GUARD_DRIFT))
        report->error = AIRY_GRAD_E04_GUARD_DRIFT;
    else
        report->error = AIRY_GRAD_OK;

    return AIRY_SUCCESS;
}

airy_err_t airy_grad_verify_plan_g(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                                   const airy_gccp_goal_t *goal, airy_grad_report_t *report)
{
    return grad_verify_impl(plan, budget, goal, NULL, 0, report);
}

airy_err_t airy_grad_verify_plan(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                                 airy_grad_report_t *report)
{
    return airy_grad_verify_plan_g(plan, budget, NULL, report);
}

airy_err_t airy_grad_verify_scope_g(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                                    const airy_gccp_goal_t *goal, const char *const *scope_nodes,
                                    size_t scope_count, airy_grad_report_t *report)
{
    return grad_verify_impl(plan, budget, goal, scope_nodes, scope_count, report);
}

airy_err_t airy_grad_verify_scope(const airy_task_plan_t *plan, const airy_grad_budget_t *budget,
                                  const char *const *scope_nodes, size_t scope_count,
                                  airy_grad_report_t *report)
{
    return airy_grad_verify_scope_g(plan, budget, NULL, scope_nodes, scope_count, report);
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
        return "E-05 verify insufficient: node metadata incomplete in participating set";
    case AIRY_GRAD_E04_GUARD_DRIFT:
        return "E-04 purpose drift: guard node outputs share no overlap with goal";
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
    cJSON_AddNumberToObject(root, "errors", (double)report->errors);
    cJSON_AddNumberToObject(root, "causal_checked", (double)report->causal_checked);
    cJSON_AddNumberToObject(root, "cost_checked", (double)report->cost_checked);
    cJSON_AddNumberToObject(root, "metadata_ok", (double)report->metadata_ok);
    cJSON_AddNumberToObject(root, "guard_state", (double)report->guard_state);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return AIRY_ENOMEM;
    *out_json = json;
    return AIRY_SUCCESS;
#else

    size_t cap = 768;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return AIRY_ENOMEM;
    int n = snprintf(buf, cap,
                     "{\"error\":%d,\"error_desc\":\"%s\",\"affected_node\":\"%s\","
                     "\"cost_sum_ms\":%lld,\"cost_sum_mb\":%lld,\"checked_nodes\":%u,"
                     "\"skipped_nodes\":%u,\"invariant_guards\":%u,\"errors\":%u,"
                     "\"causal_checked\":%u,\"cost_checked\":%u,\"metadata_ok\":%u,"
                     "\"guard_state\":%u}",
                     (int)report->error, airy_grad_error_str(report->error), report->affected_node,
                     (long long)report->cost_sum_ms, (long long)report->cost_sum_mb,
                     (unsigned)report->checked_nodes, (unsigned)report->skipped_nodes,
                     (unsigned)report->invariant_guards, (unsigned)report->errors,
                     (unsigned)report->causal_checked, (unsigned)report->cost_checked,
                     (unsigned)report->metadata_ok, (unsigned)report->guard_state);
    if (n <= 0 || (size_t)n >= cap) {
        AIRY_FREE(buf);
        return AIRY_EUNKNOWN;
    }
    *out_json = buf;
    return AIRY_SUCCESS;
#endif
}
