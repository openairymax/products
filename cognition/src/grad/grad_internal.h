// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#ifndef AIRY_PRODUCTS_COGNITION_GRAD_INTERNAL_H
#define AIRY_PRODUCTS_COGNITION_GRAD_INTERNAL_H

/**
 * @file grad_internal.h
 * @brief GRAD 策略内部共享头（M5-4 迁出后组装）。
 *
 * 组装原 atoms/coreloopthree grad 域三份头：
 *   - grad_coordinator.h：批判环配置/回合记录/协调器 API（去 AIRY_API；
 *     stats 类型与 progress 回调上移机制契约头 grad.h）；
 *   - grad_llm_callbacks.h：LLM 回调上下文与回调声明（ctx 闭包化——
 *     llm_adapter/llm_svc/lock 三机制句柄替换为 airy_grad_complete_fn
 *     补全闭包）；
 *   - grad_llm_callbacks_internal.h：跨文件共享声明（ws/call/s2/s1 域）。
 *
 * GRAD 三权分立：
 *   - 模型 A（t2）：生成/修订 DAG 计划（骨架 → 增量扩展）；
 *   - 模型 C（t1-p）：确定性四路验证（E-01 因果 / E-02 死锁 / E-03 资源
 *     / E-04 护栏计数），零生成 token；E-04 语义评估由模型 B 承载；
 *   - 模型 B（t1-f）：结合目标 G、验证报告与 E-04 判定做最终仲裁。
 *
 * 微分熵缩减（V3.0 §2.5）：每轮验证器只处理增量 patch 及其一阶邻域，
 * 未变节点视为已证定理；总验证代价 O(N + M·Δ_max)。
 *
 * @see atoms/coreloopthree/docs/GRAD.md
 */

#include "grad.h"
#include "grad_verifier.h"

#include "logging.h"
#include "airy_memory.h"
#include "string_compat.h"
#include "platform.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AIRY_GRAD_COORDINATOR_VERSION "1.0.0"

#define AIRY_GRAD_MAX_ITERATIONS 3

#define AIRY_GRAD_NEIGHBOR_DEPTH 2

/**
 * @brief Model B (t1-f) arbitration confidence threshold θ_B (GRAD V3.0 §11).
 *
 * When B's final ruling confidence falls below this threshold, B forfeits
 * the arbitration right and defaults to Model C's hard-logic verdict
 * (conservative safety policy: a low-confidence "reject" must not trigger
 * a wasteful fix loop, and a low-confidence "accept" must not be trusted).
 */
#define AIRY_GRAD_ARBITER_MIN_CONF 0.55

/**
 * @brief Plan generation callback (model A, t2).
 *
 * Takes the GCCP goal and an optional rejection patch, and outputs or
 * revises the DAG plan. First round: empty patch, generate a skeleton
 * plan; later rounds: rejection patch JSON (patch), generate only an
 * incremental fix.
 *
 * @param goal [in] GCCP goal (may be NULL, BORROW)
 * @param patch_json [in] Rejection patch JSON (NULL on first round, BORROW)
 * @param out_plan [out] Output plan (OWNER, caller airy_task_plan_free)
 * @param user_data [in] User data
 * @return AIRY_SUCCESS on success
 */
typedef airy_err_t (*grad_s2_plan_fn)(const airy_gccp_goal_t *goal, const char *patch_json,
                                      airy_task_plan_t **out_plan, void *user_data);

/**
 * @brief Contextual arbitration callback (model B, t1-f).
 *
 * Combines the goal G with C's four-way verification result to issue a
 * final approval/rejection. Returning NULL unconditionally accepts C's
 * hard-logic verdict (conservative policy).
 *
 * @param goal [in] GCCP goal (may be NULL, BORROW)
 * @param report [in] Verification report (BORROW)
 * @param plan [in] Plan to arbitrate (BORROW)
 * @param out_verdict [out] Arbitration JSON (NULL means accept C; OWNER, caller AIRY_FREE)
 * @param user_data [in] User data
 * @return AIRY_SUCCESS on success
 */
typedef airy_err_t (*grad_s1_arbiter_fn)(const airy_gccp_goal_t *goal,
                                         const airy_grad_report_t *report,
                                         const airy_task_plan_t *plan, char **out_verdict,
                                         void *user_data);

/**
 * @brief Coordinator configuration.
 */
typedef struct airy_grad_config {
    uint32_t max_iterations;
    uint32_t neighbor_depth;
    airy_grad_budget_t budget;
    grad_s2_plan_fn s2_plan;
    grad_s1_arbiter_fn s1_arbiter;
    void *s2_user_data;
    void *s1_user_data;
    /* 阶段进度回调（可为 NULL）：GRAD 各模型回合对上层可见（决策链） */
    grad_progress_fn progress_cb;
    void *progress_user_data;
} airy_grad_config_t;

#define AIRY_GRAD_CONFIG_DEFAULTS                \
    {.max_iterations = AIRY_GRAD_MAX_ITERATIONS, \
     .neighbor_depth = AIRY_GRAD_NEIGHBOR_DEPTH, \
     .budget = {600000, 1024},                   \
     .s2_plan = NULL,                            \
     .s1_arbiter = NULL,                         \
     .s2_user_data = NULL,                       \
     .s1_user_data = NULL,                       \
     .progress_cb = NULL,                        \
     .progress_user_data = NULL}

/**
 * @brief Single-round verification/fix record (decision-chain log unit).
 */
typedef struct airy_grad_round {
    uint32_t iteration;
    airy_grad_error_t error;
    char error_desc[128];
    char affected_node[128];
    uint32_t verified_scope;
    uint32_t total_nodes;
    char *patch_json;
} airy_grad_round_t;

typedef struct airy_grad_coordinator airy_grad_coordinator_t;

/**
 * @brief Create a GRAD coordinator.
 *
 * @param config [in] Configuration (non-NULL, BORROW)
 * @param out_coord [out] Output coordinator (OWNER, airy_grad_coordinator_destroy)
 * @return AIRY_SUCCESS / AIRY_EINVAL / AIRY_ENOMEM
 *
 * @ownership out_coord: OWNER
 */
airy_err_t airy_grad_coordinator_create(const airy_grad_config_t *config,
                                        airy_grad_coordinator_t **out_coord);

/**
 * @brief Destroy a GRAD coordinator.
 *
 * @param coord [in] Coordinator (may be NULL, TRANSFER)
 *
 * @ownership coord: TRANSFER
 */
void airy_grad_coordinator_destroy(airy_grad_coordinator_t *coord);

/**
 * @brief Run the GRAD plan-level critique loop.
 *
 * Anchored on the GCCP goal, iteratively verifies the initial plan
 * (seed_plan):
 *   round 0: verify seed_plan → converged if passed; rejected enters loop.
 *   later rounds: "generate → verify → arbitrate → fix" until convergence
 *   or max iterations.
 * Outputs the converged final plan (D_final).
 *
 * @param coord [in] Coordinator (non-NULL, BORROW)
 * @param goal [in] GCCP goal (may be NULL, BORROW)
 * @param seed_plan [in] Initial plan (may be NULL = fully generated by
 *        model A; BORROW, coordinator does not free)
 * @param out_plan [out] Output final plan (OWNER, airy_task_plan_free);
 *        NULL when the seed plan passed round 0 unmodified
 * @param out_stats [out] Run statistics (may be NULL, OUT)
 * @return AIRY_SUCCESS converged; AIRY_ETIMEDOUT max rounds reached,
 *         needs manual review; AIRY_EINVAL/other on failure
 *
 * @ownership out_plan: OWNER
 * @threadsafe no
 */
airy_err_t airy_grad_coordinator_execute(airy_grad_coordinator_t *coord,
                                         const airy_gccp_goal_t *goal,
                                         const airy_task_plan_t *seed_plan,
                                         airy_task_plan_t **out_plan,
                                         airy_grad_stats_t *out_stats);

/**
 * @brief Get coordinator statistics.
 *
 * @param coord [in] Coordinator (non-NULL, BORROW)
 * @param out_stats [out] Output statistics (non-NULL, OUT)
 * @return AIRY_SUCCESS / AIRY_EINVAL
 *
 * @ownership out_stats: NONE
 */
airy_err_t airy_grad_coordinator_get_stats(const airy_grad_coordinator_t *coord,
                                           airy_grad_stats_t *out_stats);

/**
 * @brief Serialize a round record to JSON (decision-chain logging).
 *
 * @param round [in] Round record (non-NULL, BORROW)
 * @param out_json [out] JSON string (OWNER, caller AIRY_FREE)
 * @return AIRY_SUCCESS / AIRY_ENOMEM
 *
 * @ownership out_json: OWNER
 */
airy_err_t airy_grad_round_to_json(const airy_grad_round_t *round, char **out_json);

/**
 * @brief Build the E-01~E-04 rejection patch JSON (for model A precise fixing).
 *
 * Generates the GRAD §6.2 standard patch format from the verification report:
 * {"patch_id":"...","source":"C_Model","error_code":"E-xx",
 *  "affected_scope":[...],"missing_artifact":"...","suggestion":"..."}
 *
 * @param report [in] Verification report (non-NULL, BORROW)
 * @param iteration [in] Current round
 * @param out_patch [out] Patch JSON (OWNER, caller AIRY_FREE)
 * @return AIRY_SUCCESS / AIRY_ENOMEM
 *
 * @ownership out_patch: OWNER
 */
airy_err_t airy_grad_build_patch(const airy_grad_report_t *report, uint32_t iteration,
                                 char **out_patch);

/**
 * @brief GRAD LLM 回调上下文（闭包化，M5-4）。
 *
 * LLM 出口经 complete/complete_ctx 补全闭包解耦——策略侧不持有机制核
 * llm_adapter/llm_svc/lock 句柄，由 grad_run() 从 launch 快照装配。
 * 其余指针均 BORROW（调用方管理生命周期）；workspace_root 为决策链
 * 持久化目录根（$AIRY_HOME/workspace）。
 */
typedef struct grad_llm_ctx {

    /* LLM 补全闭包（airy_grad_complete_fn 契约，见 grad.h） */
    airy_grad_complete_fn complete;
    void *complete_ctx;

    /* Three-model slots (borrowed):
     * model A(s2) / model B(verify) / model C(expert) */
    const char *s2_model;
    const char *s1_verify_model;
    const char *s1_expert_model;

    uint32_t max_tokens;

    /* 2.1.1.6 修复：GRAD 各阶段 LLM 调用 token 累计（思考 token 保留）——
     * s2_plan（模型 A）与 s1_arbiter（模型 B）的真实 usage 逐次累加，
     * 供 engine feedback / 计费上报使用。t1-p（模型 C）为确定性验证器，
     * 不调 LLM，不计入。 */
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;

    const airy_gccp_goal_t *goal;

    const char *original_input;
    size_t original_input_len;

    const char *workspace_root;

    const char *plan_id;

    void *chain_log;

    uint32_t round_index;
} grad_llm_ctx_t;

/**
 * @brief Model A plan generation callback (grad_s2_plan_fn 实现)。
 *
 * First round (patch_json=NULL): LLM generates a skeleton plan JSON →
 * parsed into airy_task_plan_t.
 * Later rounds (patch_json non-NULL): LLM generates an incremental fix
 * plan JSON from the rejection patch.
 *
 * Degradation when LLM is unavailable: build a minimal single-node plan
 * from goal keywords (conservative).
 *
 * @ownership out_plan: OWNER
 */
airy_err_t grad_llm_s2_plan(const airy_gccp_goal_t *goal, const char *patch_json,
                            airy_task_plan_t **out_plan, void *user_data);

/**
 * @brief Model B contextual arbitration callback (grad_s1_arbiter_fn 实现)。
 *
 * Combines goal G with C's verification report; LLM issues the final
 * verdict JSON: {"verdict":"accept|reject","confidence":0.0-1.0,"opinion":"..."}
 * Returns NULL verdict when LLM is unavailable (accept C's hard-logic
 * verdict, conservative policy).
 *
 * @ownership out_verdict: OWNER
 */
airy_err_t grad_llm_s1_arbiter(const airy_gccp_goal_t *goal, const airy_grad_report_t *report,
                               const airy_task_plan_t *plan, char **out_verdict, void *user_data);

/**
 * @brief Open/create the decision-chain log (JSONL, append mode).
 *
 * @return AIRY_SUCCESS (or silently skipped when workspace_root is empty)
 * @threadsafe no
 */
airy_err_t grad_llm_trace_open(grad_llm_ctx_t *ctx);

/**
 * @brief Append a decision-chain record (one JSONL line).
 *
 * @param event [in] Event name (e.g. "s2_plan"/"c_verify"/"b_arbiter"/"patch")
 * @threadsafe no
 */
airy_err_t grad_llm_trace_append(grad_llm_ctx_t *ctx, const char *event, const char *json);

/**
 * @brief Close the decision-chain log.
 * @threadsafe no
 */
void grad_llm_trace_close(grad_llm_ctx_t *ctx);

/* grad_llm_ws.c — 工作区/文件域 */
int grad_ws_mkdir(const char *path);
char *grad_ws_dir(const grad_llm_ctx_t *ctx);
int grad_ws_write_file(const char *dir, const char *sub, const char *name, const char *data,
                       size_t data_len);

/* grad_llm_call.c — LLM 调用域（经 complete 闭包） */
airy_err_t grad_llm_call(grad_llm_ctx_t *ctx, const char *system, const char *user,
                         const char *model, char **out_text, size_t *out_len);

/* grad_llm_s2_plan.c — 模型 A 计划生成域 */
char *grad_goal_to_json(const airy_gccp_goal_t *goal, const grad_llm_ctx_t *ctx);
airy_err_t grad_parse_plan_json(const char *json, const char *plan_id,
                                airy_task_plan_t **out_plan);

/* grad_llm_s1_arbiter.c — 模型 B 仲裁域 */
char *grad_collect_guards(const airy_task_plan_t *plan);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_PRODUCTS_COGNITION_GRAD_INTERNAL_H */
