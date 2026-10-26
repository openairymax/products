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
#include "plan_schema.h"

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

/* C11：GRAD 全链确定性采样——温度 0 + 固定种子，验证/仲裁结论可复现。
 * 种子非零是硬约束（零值=不发送约定下等价缺省）。 */
#define GRAD_SEED 20260101u

/**
 * @brief Model B (t1-f) arbitration confidence threshold θ_B (GRAD V3.0 §11).
 *
 * When B's final ruling confidence falls below this threshold, B forfeits
 * the arbitration right and defaults to Model C's hard-logic verdict
 * (conservative safety policy: a low-confidence "reject" must not trigger
 * a wasteful fix loop, and a low-confidence "accept" must not be trusted).
 */
#define AIRY_GRAD_ARBITER_MIN_CONF 0.55

/* 模型 B 裁决契约界（C2 §5.2b.4）；置于 ctx 之前，round_hist 定长依赖 */
#define GRAD_DEFECT_CODE_MAX 8              /**< "E-01".."E-05" / "SEM" */
#define GRAD_ARBITER_PROMPT_MAX_CHARS 12000 /**< B 的 prompt 字符预算 */
#define GRAD_ARBITER_GOAL_MAX_CHARS 4000    /**< goal 片段截断上界 */
#define GRAD_ARBITER_MAX_DEFECTS 32         /**< defects[] 解析上界（超出截断） */
#define GRAD_ARBITER_MAX_HISTORY 3          /**< round_history[] 保留轮数 */
#define GRAD_ARBITER_HIST_LINE 128          /**< 单条历史行长 */

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

    /* 0.1.20 B1-e：GRAD 分角色 token 计量（思考 token 保留）——
     * s2_plan（模型 A 槽）与 s1_arbiter（模型 B 槽）的真实 usage 按
     * 角色逐次累加，供 engine feedback / 成本复算使用。t1-p（模型 C）
     * 为确定性验证器，不调 LLM，不设槽。 */
    airy_grad_usage_t usage[AIRY_GRAD_ROLE_COUNT];

    const airy_gccp_goal_t *goal;

    const char *original_input;
    size_t original_input_len;

    const char *workspace_root;

    const char *plan_id;

    void *chain_log;

    uint32_t round_index;

    /* C2 轮次历史（§5.2b.4 第 5 优先级片段）：由 B 每次裁决后自行
     * 追加"round=N verdict=... defects=E-01@id"，环形保留最近
     * GRAD_ARBITER_MAX_HISTORY 条，避免 B 在后续轮次重复同一驳回。 */
    char round_hist[GRAD_ARBITER_MAX_HISTORY][GRAD_ARBITER_HIST_LINE];
    uint32_t round_hist_count;
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
 * verdict JSON (C2 契约, §5.2b.4):
 *   {"verdict":"accept|reject","confidence":0..1,"e04":"pass|fail|unknown",
 *    "defects":[{node_id,code,evidence,suggestion}],"affected_scope":[...],
 *    "opinion":"..."}
 * prompt 按六级优先级装配（goal → c_report → 审查节点全文 → 其余节点
 * 摘要 → 轮次历史 → guard 列表），受 GRAD_ARBITER_PROMPT_MAX_CHARS 约束。
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

/* grad_llm_call.c — LLM 调用域（经 complete 闭包）；role 决定 usage 计量槽 */
airy_err_t grad_llm_call(grad_llm_ctx_t *ctx, const char *system, const char *user,
                         const char *model, airy_grad_role_e role, char **out_text,
                         size_t *out_len);

/* grad_llm_s2_plan.c — 模型 A 计划生成域 */
char *grad_goal_to_json(const airy_gccp_goal_t *goal, const grad_llm_ctx_t *ctx);
airy_err_t grad_parse_plan_json(const char *json, const char *plan_id,
                                airy_task_plan_t **out_plan);

/* plan_metadata.c — 缺省元数据推导（0.1.20 C4）：只补缺省不覆盖显式值 */
size_t grad_plan_metadata_fill(airy_task_plan_t *plan);

/* grad_patch.c — C6 增量修补域（0.1.20 §5.2b.3）：
 * B 驳回缺陷 → Δk 修补 scope → 计划合并 → 合并产物序列化复验 */

/* 模型 B 裁决契约界（C2 §5.2b.4）——定义见文件前部 */

/** @brief 单条缺陷（B_Model verdict defects[] 元素）。 */
typedef struct grad_defect {
    char node_id[GRAD_NODE_ID_MAX + 1];
    char code[GRAD_DEFECT_CODE_MAX]; /**< "E-01".."E-05" | "SEM"（C2 契约枚举） */
    char evidence[256];
    char suggestion[256];
} grad_defect_t;

/** @brief Δk 修补闭包上界：min(本值, ceil(N/2))，超界整份重绘。 */
#define GRAD_PATCH_MAX_NODES 8

/**
 * @brief 计算 Δk 修补 scope：unique(defects.node_id) 经一阶闭包上界裁定。
 *
 * scope 空 / defect 节点不在计划内 → AIRY_EINVAL（G55：模板 patch 不重绘）；
 * 闭包超界 → AIRY_E2BIG（调用方整份重绘 repair_mode=full）。
 *
 * @param out_closure_size [out] 一阶闭包实际大小（可为 NULL）
 * @ownership out_scope: OWNER（调用方 AIRY_FREE 顶层与元素）
 */
airy_err_t grad_patch_scope(const airy_task_plan_t *plan, const grad_defect_t *defects,
                            size_t defect_count, char ***out_scope, size_t *out_count,
                            size_t *out_closure_size);

/**
 * @brief 构建 B_Model 增量修补 patch JSON（repair_mode=incremental）。
 *
 * @ownership out_json: OWNER（调用方 AIRY_FREE）
 */
airy_err_t grad_patch_build(uint32_t iteration, const grad_defect_t *defects,
                            size_t defect_count, const char *const *scope,
                            size_t scope_count, char **out_json);

/**
 * @brief 合并 sub（模型 A 修补产物）到 plan：scope 内整节点替换，
 *        frozen 逐字段精确相等，sub 新节点仅限 scope（E-01 插入生产者）。
 *
 * frozen 篡改 → AIRY_ERR_STATE_ERROR（patch_inconsistent，本轮作废）；
 * 越权新节点 → AIRY_EINVAL；entry points 合并后重算（无依赖节点）。
 *
 * @ownership out_merged: OWNER（调用方 airy_task_plan_free）
 */
airy_err_t grad_plan_merge(const airy_task_plan_t *plan, const airy_task_plan_t *sub,
                           const char *const *scope, size_t scope_count,
                           airy_task_plan_t **out_merged);

/**
 * @brief 序列化计划为 schema v1 视图 JSON（合并产物复验与决策链日志）。
 *
 * @ownership out_json: OWNER（调用方 AIRY_FREE）
 */
airy_err_t grad_plan_to_json(const airy_task_plan_t *plan, char **out_json);

/* grad_verdict.c — C2 裁决契约解析（§5.2b.4 输出强校验） */

/** @brief 终裁取向（契约仅两值；低置信度由协调器按 θ_B 另行折让）。 */
typedef enum grad_vd_kind {
    GRAD_VD_ACCEPT = 0,
    GRAD_VD_REJECT
} grad_vd_kind_t;

/** @brief E-04 目的漂移三态；契约缺省归 UNKNOWN（不臆测）。 */
typedef enum grad_e04 {
    GRAD_E04_UNKNOWN = 0,
    GRAD_E04_PASS,
    GRAD_E04_FAIL
} grad_e04_t;

/**
 * @brief B 裁决解析结果（OWNER 语义：defects/scope 由 grad_verdict_free 回收）。
 *
 * 与原始 JSON 解耦：协调器只消费本结构，不再做松散字段读取。
 */
typedef struct grad_verdict {
    grad_vd_kind_t kind;
    double confidence;
    grad_e04_t e04;
    char opinion[256];
    grad_defect_t *defects; /**< OWNER；defect_count == 0 时为 NULL */
    size_t defect_count;
    char **scope; /**< affected_scope，去重保序；OWNER（顶层与元素） */
    size_t scope_count;
} grad_verdict_t;

/**
 * @brief 严格解析 B 裁决 JSON（契约校验，任一违例整份拒绝）。
 *
 * 校验项（§5.2b.4）：
 *   - verdict ∈ {accept,reject}（缺失/其它值 → 失败）；
 *   - confidence 为数字或数字串且 ∈ [0,1]（类型非法/越界 → 失败）；
 *   - e04 缺省归 UNKNOWN，出现则必须是 pass|fail|unknown；
 *   - defects[] 每项 node_id 必须存在于 plan，code ∈ {E-01..E-05,SEM}；
 *     条数超过 GRAD_ARBITER_MAX_DEFECTS 截断并 WARN（保裁决本体）；
 *   - affected_scope[] 元素必须存在于 plan，去重保序，超计划节点数截断。
 *
 * 契约违例由调用方记 arbiter_parse_error 并降级"采纳 C 判定"，
 * **不得**把非法 JSON 当作 accept。
 *
 * @param json [in] LLM 输出（non-NULL, BORROW）
 * @param plan [in] 被裁决计划（用于 node_id 存在性校验；BORROW）
 * @param out [out] 解析结果（non-NULL, OUT；失败时保持全零）
 * @return AIRY_SUCCESS 合规；AIRY_EINVAL 契约违例；AIRY_ENOMEM
 *
 * @ownership out->defects/out->scope: OWNER（grad_verdict_free）
 * @threadsafe yes（无共享状态）
 */
airy_err_t grad_verdict_parse(const char *json, const airy_task_plan_t *plan,
                              grad_verdict_t *out);

/**
 * @brief 回收裁决解析结果（可重复调用；入参可为 NULL）。
 *
 * @ownership v: NONE（仅释放其内部 OWNER 成员）
 */
void grad_verdict_free(grad_verdict_t *v);

/* grad_llm_s1_arbiter.c — 模型 B 仲裁域（prompt 六级装配，见 .c 头注释） */

#ifdef __cplusplus
}
#endif

#endif /* AIRY_PRODUCTS_COGNITION_GRAD_INTERNAL_H */
