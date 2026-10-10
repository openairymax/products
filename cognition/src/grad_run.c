// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_run.c
 * @brief GRAD 策略入口：装配批判环并执行（airy_grad_ops_t.run 实现）。
 *
 * 从 launch 快照（全 BORROW）装配 LLM 回调上下文与协调器配置，运行
 * "模型 A 生成 → 模型 C 确定性四验 → 模型 B 仲裁"批判环（0.1.19 M5-4
 * §267 自 atoms/coreloopthree 迁出）。降级检测（fallback_ 前缀空壳计划）
 * 上收策略侧——机制核不再感知策略字符串约定。
 *
 * 结果契约（grad.h 三分支）：SUCCESS + final_plan → 采纳；SUCCESS +
 * final_plan=NULL → seed 首轮过门；其他错误码（含降级 AIRY_EFAIL）→
 * fail-closed，final_plan 必 NULL，调用方转人工评审。
 */

#include "grad_internal.h"

#include "logging.h"
#include "airy_memory.h"

#include <string.h>

airy_err_t grad_run(const airy_grad_launch_t *launch, const airy_task_plan_t *seed_plan,
                    airy_grad_outcome_t *out_outcome)
{
    if (!launch || !out_outcome)
        return AIRY_EINVAL;
    __builtin_memset(out_outcome, 0, sizeof(*out_outcome));
    if (!launch->complete)
        return AIRY_ESERVICE;

    grad_llm_ctx_t grad_ctx;
    __builtin_memset(&grad_ctx, 0, sizeof(grad_ctx));
    grad_ctx.complete = launch->complete;
    grad_ctx.complete_ctx = launch->complete_ctx;
    grad_ctx.max_tokens = launch->max_tokens;
    grad_ctx.s2_model = launch->s2_model;
    grad_ctx.s1_verify_model = launch->s1_verify_model;
    /* t1-p（PROF）在 GRAD 中的角色是模型 C 的确定性四验（零生成 token，
     * 见 coreloopthree/README 模型 C 槽说明），无需 LLM 模型名；
     * s1_expert_model 为设计保留位（追踪用），t1-p 的真实 LLM 消费点
     * 在 GCCP 意图确认，不在 GRAD。 */
    grad_ctx.s1_expert_model = launch->s1_expert_model;
    grad_ctx.goal = launch->goal;
    grad_ctx.original_input = launch->input;
    grad_ctx.original_input_len = launch->input_len;
    grad_ctx.workspace_root = launch->workspace_root;
    grad_ctx.plan_id = launch->plan_id;

    airy_err_t trace_err = grad_llm_trace_open(&grad_ctx);
    if (trace_err != AIRY_SUCCESS)
        return trace_err;

    airy_grad_config_t grad_cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    grad_cfg.s2_plan = grad_llm_s2_plan;
    grad_cfg.s1_arbiter = grad_llm_s1_arbiter;
    grad_cfg.s2_user_data = &grad_ctx;
    grad_cfg.s1_user_data = &grad_ctx;
    /* 2.3.14 GRAD 决策链可见性：阶段进度回调经 launch 透传。 */
    grad_cfg.progress_cb = launch->progress_cb;
    grad_cfg.progress_user_data = launch->progress_user_data;

    airy_grad_coordinator_t *grad_coord = NULL;
    airy_err_t grad_err = airy_grad_coordinator_create(&grad_cfg, &grad_coord);
    if (grad_err != AIRY_SUCCESS || !grad_coord) {
        grad_llm_trace_close(&grad_ctx);
        return grad_err != AIRY_SUCCESS ? grad_err : AIRY_EFAIL;
    }

    airy_task_plan_t *grad_final = NULL;
    airy_grad_stats_t grad_stats;
    __builtin_memset(&grad_stats, 0, sizeof(grad_stats));
    grad_err = airy_grad_coordinator_execute(grad_coord, grad_ctx.goal, seed_plan, &grad_final,
                                             &grad_stats);
    airy_grad_coordinator_destroy(grad_coord);

    out_outcome->converged = grad_stats.converged;
    out_outcome->stats = grad_stats;
    out_outcome->prompt_tokens = grad_ctx.prompt_tokens;
    out_outcome->completion_tokens = grad_ctx.completion_tokens;
    out_outcome->total_tokens = grad_ctx.total_tokens;

    /* 2.1.1.6 修复：思考 token 保留——GRAD 各阶段 LLM 调用（模型 A 计划
     * 生成 / 模型 B 仲裁）的真实 token 消耗写入决策链 trace，与
     * rounds/rejections 等元数据同链持久化，可回溯审计。 */
    {
        char tok_trace[256];
        snprintf(tok_trace, sizeof(tok_trace),
                 "{\"prompt_tokens\":%u,\"completion_tokens\":%u,\"total_tokens\":%u}",
                 grad_ctx.prompt_tokens, grad_ctx.completion_tokens, grad_ctx.total_tokens);
        grad_llm_trace_append(&grad_ctx, "token_usage", tok_trace);
    }
    grad_llm_trace_close(&grad_ctx);

    if (grad_err == AIRY_SUCCESS && grad_final) {
        /* P1：GRAD 降级产物（LLM 故障时模型 A 返回 "fallback_" 前缀空壳
         * 单节点计划，C/B 判定"收敛"）不得作为精化结果流出批判门——
         * 降级收敛意味着 seed 计划从未通过 C/B 验证（C 验证的是空壳、
         * B 仲裁也故障 defer），绝不能被当作已验证放行——置 degraded 走
         * fail-closed 人工评审。 */
        if (grad_final->task_plan_id &&
            strncmp(grad_final->task_plan_id, "fallback_", 9) == 0) {
            AIRY_LOG_WARN("GRAD: converged on degraded fallback plan, seed NOT verified "
                          "(plan_id=%s) — manual review required",
                          grad_final->task_plan_id);
            airy_task_plan_free(grad_final);
            out_outcome->degraded = 1;
            return AIRY_EFAIL;
        }
        out_outcome->final_plan = grad_final;
        return AIRY_SUCCESS;
    }
    if (grad_final) {
        /* Fail-closed：协调器未收敛时已丢弃 owned 计划（返回 NULL），
         * 此处防御性释放任何残留产物——未验证计划绝不允许流出批判门。 */
        AIRY_LOG_WARN("GRAD: non-converged result discarded (err=%d)", (int)grad_err);
        airy_task_plan_free(grad_final);
    }
    return grad_err;
}
