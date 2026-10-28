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
 *   - E-04 purpose drift guard (0.1.20 C5): with a goal supplied, every
 *     invariant_guard node's outputs must share entity overlap with the
 *     goal keywords (bidirectional substring match)
 *   - E-05 verification insufficiency: metadata coverage — a plan is
 *     rejected when any participating node lacks checkable metadata
 *
 * Conservative policy: checks rely only on structured metadata, never
 * on natural-language text; a node lacking metadata is not strictly
 * judged for causality/resources but still counted in the metadata_ok
 * coverage, and a plan where no node carries checkable metadata is
 * rejected as E-05 (empty-shell plans can no longer report OK).
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
    AIRY_GRAD_E05_VERIFY_INSUFFICIENT,
    /* E-04 purpose-drift guard (0.1.20 C5, deterministic): with a goal
     * supplied, some invariant_guard node's outputs share zero entity
     * overlap with the goal keywords — the guard protects entities the
     * goal never mentions, i.e. purpose drift. Without a goal the check
     * is not applicable and only invariant_guards is counted; the
     * semantic layer (model B arbiter, grad_llm_s1_arbiter) still owns
     * the finer-grained e04=pass|fail verdict. */
    AIRY_GRAD_E04_GUARD_DRIFT
} airy_grad_error_t;

/** @brief errors 位图位提取：bit(e) = 1 << airy_grad_error_t 枚举值。 */
#define AIRY_GRAD_ERR_BIT(e) (1u << (int)(e))

/** @brief E-04 guard 判定三态（report->guard_state）。 */
typedef enum {
    AIRY_GRAD_GUARD_NONE = 0, /**< goal 缺席，不判定（交语义层 B 仲裁） */
    AIRY_GRAD_GUARD_PASS = 1, /**< 全部 guard 节点 outputs 与 goal 实体有交集 */
    AIRY_GRAD_GUARD_FAIL = 2  /**< 存在 guard 节点 outputs 与 goal 零交集 */
} airy_grad_guard_state_t;


typedef struct airy_grad_budget {
    int64_t time_ms;
    int64_t mem_mb;
} airy_grad_budget_t;

/**
 * @brief 差分验证闭包：scope 节点 + 全部依赖前驱 + 直接后继
 *        （GRAD V3.0 §6.4 一阶闭包，0.1.20 C6 修补 scope 复用）。
 *
 * @param plan [in] 任务计划（non-NULL, BORROW）
 * @param scope_nodes [in] 变更节点 ID 数组（可为 NULL）
 * @param scope_count [in] 变更节点数
 * @param out_closure [out] 闭包节点下标数组（OWNER，调用方 AIRY_FREE）
 * @param out_count [out] 闭包大小
 * @return AIRY_SUCCESS / AIRY_ENOMEM
 *
 * @ownership out_closure: OWNER
 */
airy_err_t grad_compute_closure(const airy_task_plan_t *plan, const char *const *scope_nodes,
                                size_t scope_count, int **out_closure, size_t *out_count);


typedef struct airy_grad_report {
    airy_grad_error_t error;
    char affected_node[128];
    char missing_artifact[128];
    char cycle_path[512];
    int64_t cost_sum_ms;
    int64_t cost_sum_mb;
    uint32_t checked_nodes;     /**< E-01 因果受检数（= causal_checked 兼容别名） */
    uint32_t skipped_nodes;
    uint32_t invariant_guards;
    /* C5（0.1.20）判定细化字段（尾部追加，ABI 兼容）：失败不再只报
     * 单值首错，errors 位图保留全部命中判据，供调度层聚合与回归。 */
    uint32_t errors;         /**< 判定位图，AIRY_GRAD_ERR_BIT(e) 按位或 */
    uint32_t causal_checked; /**< E-01 受检节点数（空 inputs 平凡通过亦计） */
    uint32_t cost_checked;   /**< E-03 参与守恒累计节点数（不再污染 checked_nodes） */
    uint32_t metadata_ok;    /**< 元数据完备节点数（见 airy_grad_verify_plan_g） */
    uint8_t guard_state;     /**< airy_grad_guard_state_t（goal 缺席 = NONE） */
} airy_grad_report_t;

/**
 * @brief 位图 → 单值裁决的唯一映射（Gate 32）。
 *
 * 优先级 E-02 > E-05 > E-01 > E-03 > E-04：死锁使因果遍历无意义，
 * 元数据不全使因果判定不可信，故二者先于 E-01；E-03/E-04 是局部
 * 约束，排在因果之后。无任何位时返回 AIRY_GRAD_OK。
 *
 * @param errors [in] report->errors 位图（AIRY_GRAD_ERR_BIT 按位或）
 * @return 最高优先级错误码；无错误为 AIRY_GRAD_OK
 * @threadsafe yes（纯函数）
 */
airy_grad_error_t grad_err_rank(uint32_t errors);

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
 * @brief Full-plan verification with an optional goal (0.1.20 C5).
 *
 * Equivalent to airy_grad_verify_plan plus two goal-driven refinements:
 * E-04 purpose-drift guard (guard nodes' outputs must overlap the goal
 * keywords) and the any-semantics E-05 metadata-coverage gate. When
 * goal is NULL, E-04 degrades to guard counting (guard_state = NONE)
 * and E-05 still applies — metadata completeness is goal-independent.
 *
 * @param plan [in] Task plan (non-NULL, BORROW)
 * @param budget [in] Resource budget (NULL means no E-03 constraint)
 * @param goal [in] GCCP goal (NULL = E-04 not applicable)
 * @param report [out] Verification report (non-NULL, OUT)
 * @return AIRY_SUCCESS / AIRY_EINVAL
 *
 * @ownership report: NONE
 * @threadsafe yes
 */
airy_err_t airy_grad_verify_plan_g(const airy_task_plan_t *plan,
                                   const airy_grad_budget_t *budget,
                                   const airy_gccp_goal_t *goal,
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
 * @brief Scope verification with an optional goal (0.1.20 C5).
 *
 * airy_grad_verify_scope plus the same goal-driven refinements as
 * airy_grad_verify_plan_g: E-04 guard check against goal keywords and
 * any-semantics E-05 over the participating set (delta closure in
 * scope mode, all nodes when degrading to full verification).
 *
 * @param plan [in] Task plan (non-NULL, BORROW)
 * @param budget [in] Resource budget (NULL means no E-03 constraint)
 * @param goal [in] GCCP goal (NULL = E-04 not applicable)
 * @param scope_nodes [in] Changed node ID array (NULL = full verification)
 * @param scope_count [in] Number of changed nodes
 * @param report [out] Verification report (non-NULL, OUT)
 * @return AIRY_SUCCESS / AIRY_EINVAL / AIRY_ENOMEM
 *
 * @ownership report: NONE
 * @threadsafe yes
 */
airy_err_t airy_grad_verify_scope_g(const airy_task_plan_t *plan,
                                    const airy_grad_budget_t *budget,
                                    const airy_gccp_goal_t *goal,
                                    const char *const *scope_nodes,
                                    size_t scope_count,
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
