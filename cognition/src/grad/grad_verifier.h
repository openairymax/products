/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file grad_verifier.h
 * @brief GRAD deterministic verifier — goal-based relative logic accuracy
 *        judgment (model C hard-logic verification).
 *
 * Implements GRAD protocol V3.0 chapter 5 "process logic four-way check".
 * All checks rely only on structured metadata
 * (inputs/outputs/cost/invariant_guard), not natural-language text; the
 * deadlock check uses the deterministic Kahn topological sort algorithm,
 * consuming zero generative tokens.
 *
 * Four-way check list:
 *   - E-01 causal sufficiency (logic break): Inputs(v_i) ⊆ ∪ Outputs(predecessors)
 *   - E-02 temporal partial order (deadlock): acyclicity of the subgraph
 *     (Kahn topological sort)
 *   - E-03 resource conservation (resource collapse): Σ cost(v_i) ≤ R_total
 *     (per component)
 *   - E-04 invariant preservation (purpose drift): invariant_guard nodes
 *     must not break goal G
 *
 * Conservative policy: when metadata is unknown (inputs/outputs NULL,
 * cost 0), the corresponding check is skipped (returns OK); strict
 * judgment is applied only when metadata is sufficient.
 *
 * @see atoms/coreloopthree/docs/GRAD.md
 */

#ifndef AIRY_RT_GRAD_VERIFIER_H
#define AIRY_RT_GRAD_VERIFIER_H

#include "airy_rt.h"
#include "cognition.h"
#include "gccp.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define AIRY_GRAD_VERIFIER_VERSION "1.0.0"


typedef enum {
    AIRY_GRAD_OK = 0,
    AIRY_GRAD_E01_CAUSAL_BREAK,
    AIRY_GRAD_E02_DEADLOCK,
    AIRY_GRAD_E03_RESOURCE_OVER,
    /* E-05 verification-insufficient: the plan is non-empty but no node
     * carries checkable metadata (inputs/outputs/cost all absent), so no
     * single node was strictly verified. Rejecting closes the false
     * convergence hole where an "empty-shell" LLM plan skipped every check
     * and still reported OK. */
    AIRY_GRAD_E05_VERIFY_INSUFFICIENT
    /* E-04 purpose drift has no deterministic error code: the deterministic
     * layer counts invariant_guard flags (report->invariant_guards) and the
     * semantic layer (model B arbiter, grad_llm_s1_arbiter) evaluates guard
     * nodes against goal G, returning e04=pass|fail in the verdict. */
} airy_grad_error_t;


typedef struct airy_grad_budget {
    int64_t time_ms;
    int64_t mem_mb;
} airy_grad_budget_t;


typedef struct airy_grad_report {
    airy_grad_error_t error;
    char affected_node[128];
    char missing_artifact[128];
    char cycle_path[512];
    int64_t cost_sum_ms;
    int64_t cost_sum_mb;
    uint32_t checked_nodes;
    uint32_t skipped_nodes;
    uint32_t invariant_guards;
} airy_grad_report_t;

/**
 * @brief Run the GRAD four-way verification (E-01 causality / E-02 deadlock / E-03 resources).
 *
 * Performs deterministic hard-logic verification of the task plan.
 * E-04 purpose drift needs goal-semantic judgment; this function only
 * counts invariant_guard flags (the semantic layer, model B arbiter,
 * evaluates guard nodes against goal G via e04=pass|fail).
 *
 * @param plan [in] Task plan (non-NULL, BORROW)
 * @param budget [in] Resource budget (NULL means no E-03 constraint)
 * @param report [out] Verification report (non-NULL, OUT)
 * @return AIRY_SUCCESS verification executed (report->error holds the
 *         verdict); AIRY_EINVAL invalid arguments
 *
 * @ownership report: NONE (caller fills/reads)
 * @threadsafe yes (pure function, no shared state)
 */
airy_err_t airy_grad_verify_plan(const airy_task_plan_t *plan,
                                 const airy_grad_budget_t *budget,
                                 airy_grad_report_t *report);

/**
 * @brief Differential verification scope (GRAD differential entropy reduction:
 *        verify only Δ_k and its first-order closure).
 *
 * When scope_nodes is NULL or count==0, degrades to full verification
 * (equivalent to airy_grad_verify_plan). The caller passes this round's
 * changed node ID array; the verifier strictly checks only the changed
 * nodes and their dependency neighbors.
 *
 * @param plan [in] Task plan (non-NULL, BORROW)
 * @param budget [in] Resource budget (NULL means no E-03 constraint)
 * @param scope_nodes [in] Changed node ID array (NULL = full verification)
 * @param scope_count [in] Number of changed nodes
 * @param report [out] Verification report (non-NULL, OUT)
 * @return AIRY_SUCCESS / AIRY_EINVAL / AIRY_ENOMEM
 *
 * @ownership report: NONE
 * @threadsafe yes
 */
airy_err_t airy_grad_verify_scope(const airy_task_plan_t *plan,
                                  const airy_grad_budget_t *budget,
                                  const char *const *scope_nodes, size_t scope_count,
                                  airy_grad_report_t *report);

/**
 * @brief Human-readable error-code description.
 *
 * @param err [in] Error code
 * @return Description string (static storage, BORROW)
 *
 * @ownership return: NONE
 */
const char *airy_grad_error_str(airy_grad_error_t err);

/**
 * @brief Serialize the verification report to JSON (for decision-chain logging).
 *
 * @param report [in] Verification report (non-NULL, BORROW)
 * @param out_json [out] JSON string (OWNER, caller AIRY_FREE)
 * @return AIRY_SUCCESS / AIRY_ENOMEM
 *
 * @ownership out_json: OWNER
 */
airy_err_t airy_grad_report_to_json(const airy_grad_report_t *report, char **out_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GRAD_VERIFIER_H */
