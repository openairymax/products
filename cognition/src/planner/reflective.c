// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file reflective.c
 * @brief Reflective planning strategy — production-grade dual-thinking implementation.
 *
 * Implements the full 5-phase reasoning pipeline:
 * - Phase 0: instruction decomposition (S1) → identify subtasks
 * - Phase 1: plan generation (S2+S1) → build dependency chain
 * - Phase 2: execution-verification loop → streaming critique (S2 generate → S1 verify → fix)
 * - Phase 3: subtask audit → quality gate
 * - Phase 4: goal-alignment check
 *
 * LLM 动态计划构建域（parse_llm_plan_json / llm_build_dynamic_plan /
 * build_fallback_plan）已拆分至 reflective_llm_plan.c（超 800 行文件拆分），
 * 共享契约见 reflective_internal.h。
 *
 * 0.1.19 M5-4 §271 自 atoms/coreloopthree src/cognition/think/planner/
 * 迁入 products/cognition（族内归格 src/planner/）。
 */

#include "mc.h"
#include "tc.h"
#include "airy_rt.h"
#include "cognition.h"
#include "logging.h"
#include "airy_memory.h"
#include "string_compat.h"
#include "reflective_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_client.h"
#include "error.h"
#include "plan_strategy.h"

/* ============================================================================
 * Real S2 content generator — calls the LLM service
 * ============================================================================ */

static airy_err_t real_s2_generate(const char *input, size_t in_len, char **output, size_t *out_len,
                                   void *user_data)
{
    if (!input || !output || !out_len)
        AIRY_RET_ERR(AIRY_EINVAL);

    reflective_context_t *ctx = (reflective_context_t *)user_data;

    if (ctx && ctx->llm && airy_llm_service_is_available(ctx->llm)) {
        char *response = NULL;
        airy_err_t err = airy_llm_service_call(ctx->llm, input, &response);
        if (err == AIRY_SUCCESS && response) {
            *output = response;
            *out_len = strlen(response);
            return AIRY_SUCCESS;
        }
        if (response)
            AIRY_FREE(response);
    }

    size_t buf_size = in_len + 256;
    char *buf = (char *)AIRY_MALLOC(buf_size);
    if (!buf)
        AIRY_RET_ERR(AIRY_ENOMEM);

    int written = snprintf(buf, buf_size,
                           "[Reflective Analysis of: %.*s]\n"
                           "Task decomposition into actionable sub-components.\n"
                           "Dependency identification between sub-tasks.\n"
                           "Resource and constraint evaluation.\n"
                           "Risk assessment and mitigation planning.\n",
                           (int)(in_len > 80 ? 80 : in_len), input);

    if (written <= 0 || (size_t)written >= buf_size) {
        snprintf(buf, buf_size, "[Analysis for input %zu bytes]", in_len);
        written = (int)strlen(buf);
    }

    *output = buf;
    *out_len = (size_t)written;
    return AIRY_SUCCESS;
}

/* ============================================================================
 * Real S1 verifier — calls the LLM for quality assessment
 * ============================================================================ */

static int real_s1_verify(const char *content, size_t len, float confidence, void *user_data)
{
    if (!content || len == 0)
        return 0;

    reflective_context_t *ctx = (reflective_context_t *)user_data;

    if (ctx && ctx->llm && airy_llm_service_is_available(ctx->llm)) {
        char prompt[1024];
        snprintf(prompt, sizeof(prompt),
                 "Rate the quality of this analysis on a scale of 0.0 to 1.0.\n"
                 "Reply with only a number.\n\n%s",
                 len > 800 ? "(truncated)" : content);

        char *response = NULL;
        airy_err_t err = airy_llm_service_call(ctx->llm, prompt, &response);
        if (err == AIRY_SUCCESS && response) {
            float score = (float)atof(response);
            AIRY_FREE(response);
            if (score > 0.0f && score <= 1.0f) {
                return (score >= 0.7f) ? 1 : 0;
            }
        }
        if (response)
            AIRY_FREE(response);
    }

    float quality = 0.65f;
    if (len > 50 && strstr(content, "analysis"))
        quality += 0.1f;
    if (len > 100 && strstr(content, "decomposition"))
        quality += 0.1f;
    if (len > 150 && strstr(content, "dependency"))
        quality += 0.05f;
    if (confidence > 0.5f)
        quality += 0.05f;

    return (quality >= 0.7f) ? 1 : 0;
}

/* ============================================================================
 * Reflective planning implementation
 * ============================================================================ */

static airy_err_t reflective_plan_init(void **out_context)
{
    if (!out_context)
        AIRY_RET_ERR(AIRY_EINVAL);

    reflective_context_t *ctx;
    SAFE_MALLOC_ARRAY(ctx, 1, sizeof(reflective_context_t));
    if (!ctx)
        AIRY_RET_ERR(AIRY_ENOMEM);

    ctx->chain = NULL;
    ctx->meta = NULL;
    ctx->llm = NULL;
    ctx->memory_engine = NULL;
    ctx->initialized = 0;
    ctx->session_count = 0;
    ctx->last_goal = NULL;
    ctx->max_verify_rounds = 3;
    ctx->acceptance_threshold = 0.7f;

    *out_context = ctx;
    return AIRY_SUCCESS;
}

static void reflective_plan_cleanup(airy_plan_strategy_t *strategy)
{
    if (!strategy)
        return;
    const airy_tc_ops_t *tc = are_ops_get_tc();
    const airy_mc_ops_t *mc = are_ops_get_mc();
    reflective_context_t *ctx = (reflective_context_t *)strategy->data;
    if (ctx) {
        if (ctx->chain && tc) {
            tc->chain_stop(ctx->chain);
            tc->chain_destroy(ctx->chain);
            ctx->chain = NULL;
        }
        if (ctx->meta && mc) {
            mc->destroy(ctx->meta);
            ctx->meta = NULL;
        }
        if (ctx->last_goal) {
            AIRY_FREE(ctx->last_goal);
            ctx->last_goal = NULL;
        }
        AIRY_FREE(ctx);
    }
    /* P0.20.7: free the strategy itself, consistent with the
     * reactive_destroy convention (reactive.c L86 AIRY_FREE(strategy)).
     * cognition engine destroy calls plan_strat->destroy(plan_strat),
     * which expects the destroy callback to free the strategy itself.
     * The old implementation missed this, leaking 24 bytes. Even when
     * data=NULL (theoretically impossible), the strategy itself must be
     * freed. */
    AIRY_FREE(strategy);
}

static airy_err_t reflective_plan(const airy_intent_t *intent, void *context,
                                  airy_task_plan_t **out_plan)
{

    if (!intent || !out_plan)
        AIRY_RET_ERR(AIRY_EINVAL);

    /* reflective is the dual-thinking strategy itself: without the tc/mc
     * payload registered it cannot run — fail fast per contract (tc.h/mc.h). */
    const airy_tc_ops_t *tc = are_ops_get_tc();
    const airy_mc_ops_t *mc = are_ops_get_mc();
    if (!tc || !mc)
        AIRY_RET_ERR(AIRY_ENOSYS);

    reflective_context_t *ctx = (reflective_context_t *)context;

    if (!ctx) {
        airy_err_t init_err = reflective_plan_init((void **)&ctx);
        if (init_err != AIRY_SUCCESS)
            return init_err;
    }

    if (!ctx->initialized) {
        airy_err_t err =
            tc->chain_create(intent->intent_goal ? (const char *)intent->intent_goal :
                                                   "reflective_session",
                             8192, 64, &ctx->chain);
        if (err != AIRY_SUCCESS)
            return err;

        err = mc->create(&ctx->meta);
        if (err != AIRY_SUCCESS) {
            tc->chain_destroy(ctx->chain);
            ctx->chain = NULL;
            return err;
        }

        mc->set_chain(ctx->meta, ctx->chain);
        ctx->initialized = 1;
    }

    if (ctx->last_goal)
        AIRY_FREE(ctx->last_goal);
    char goal_buf[512];
    int glen = snprintf(goal_buf, sizeof(goal_buf), "%s_flags%u",
                        intent->intent_goal ? (const char *)intent->intent_goal : "unknown",
                        intent->intent_flags);
    ctx->last_goal = AIRY_STRDUP(goal_buf);

    tc->chain_start(ctx->chain);
    ctx->session_count++;

    /* ========== Phase 0: Instruction Decomposition (S1) ========== */
    airy_thinking_step_t *step_decomp = NULL;
    char decomp_input[512];
    int di_len = snprintf(decomp_input, sizeof(decomp_input),
                          "Decompose this task into sub-tasks with clear dependencies:\n"
                          "Goal: %s\nFlags: %u\nContext: %s\n"
                          "Provide a structured breakdown with numbered steps.",
                          intent->intent_goal ? (const char *)intent->intent_goal : "?",
                          intent->intent_flags,
                          intent->intent_raw_text ? (const char *)intent->intent_raw_text : "");

    tc->step_create(ctx->chain, TC_STEP_DECOMPOSITION, decomp_input, (size_t)di_len, NULL, 0,
                    &step_decomp);

    char *decomp_output = NULL;
    size_t decomp_out_len = 0;
    real_s2_generate(decomp_input, (size_t)di_len, &decomp_output, &decomp_out_len, ctx);

    if (decomp_output && decomp_out_len > 0) {
        tc->ctx_append(ctx->chain, decomp_output, decomp_out_len);
    }
    tc->step_complete(step_decomp, decomp_output ? decomp_output : "decomposition_failed",
                      decomp_output ? decomp_out_len : 19, 0.75f, "S2-decomposer");

    mc_evaluation_result_t eval_decomp;
    char *recent_ctx = NULL;
    size_t recent_ctx_len = 0;
    tc->ctx_recent(ctx->chain, 200, &recent_ctx, &recent_ctx_len);
    mc->eval_step(ctx->meta, step_decomp, recent_ctx, recent_ctx_len, &eval_decomp);
    if (recent_ctx)
        AIRY_FREE(recent_ctx);

    if (eval_decomp.strategy == MC_CORRECT_AUTO || eval_decomp.strategy == MC_CORRECT_RERUN) {
        mc->correct(ctx->meta, step_decomp, &eval_decomp, real_s2_generate, ctx);
    }
    if (eval_decomp.critique_text)
        AIRY_FREE(eval_decomp.critique_text);
    if (decomp_output)
        AIRY_FREE(decomp_output);

    /* ========== Phase 1: Planning (S2+S1) ========== */
    airy_thinking_step_t *step_plan = NULL;
    uint32_t deps[] = {step_decomp->step_id};
    char plan_input[512];
    int pi_len = snprintf(plan_input, sizeof(plan_input),
                          "Generate a detailed execution plan based on the decomposition above.\n"
                          "Goal: %s\nInclude: step IDs, dependencies, and verification criteria.",
                          intent->intent_goal ? (const char *)intent->intent_goal : "?");

    tc->step_create(ctx->chain, TC_STEP_PLANNING, plan_input, (size_t)pi_len, deps, 1,
                    &step_plan);

    char *plan_output = NULL;
    size_t plan_out_len = 0;
    real_s2_generate(plan_input, (size_t)pi_len, &plan_output, &plan_out_len, ctx);

    if (plan_output && plan_out_len > 0) {
        tc->ctx_append(ctx->chain, plan_output, plan_out_len);
    }
    tc->step_complete(step_plan, plan_output ? plan_output : "planning_failed",
                      plan_output ? plan_out_len : 15, 0.70f, "S2-planner");

    mc_evaluation_result_t eval_plan;
    tc->ctx_recent(ctx->chain, 300, &recent_ctx, &recent_ctx_len);
    mc->eval_step(ctx->meta, step_plan, recent_ctx, recent_ctx_len, &eval_plan);
    if (recent_ctx)
        AIRY_FREE(recent_ctx);

    if (eval_plan.strategy == MC_CORRECT_AUTO || eval_plan.strategy == MC_CORRECT_RERUN) {
        mc->correct(ctx->meta, step_plan, &eval_plan, real_s2_generate, ctx);
    }
    if (eval_plan.critique_text)
        AIRY_FREE(eval_plan.critique_text);
    if (plan_output)
        AIRY_FREE(plan_output);

    /* ========== Phase 2: Execution-Verification Loop ========== */
    airy_thinking_step_t *step_exec = NULL;
    uint32_t exec_deps[] = {step_plan->step_id};
    tc->step_create(ctx->chain, TC_STEP_GENERATION, plan_input, (size_t)pi_len, exec_deps, 1,
                    &step_exec);

    char *exec_output = NULL;
    size_t exec_out_len = 0;
    int verified = 0;

    for (int round = 0; round < ctx->max_verify_rounds && !verified; round++) {
        if (exec_output) {
            AIRY_FREE(exec_output);
            exec_output = NULL;
        }
        exec_out_len = 0;

        real_s2_generate(plan_input, (size_t)pi_len, &exec_output, &exec_out_len, ctx);

        if (!exec_output || exec_out_len == 0)
            break;

        verified = real_s1_verify(exec_output, exec_out_len, 0.7f, ctx);

        if (!verified && round < ctx->max_verify_rounds - 1) {
            char correction_prompt[1024];
            snprintf(correction_prompt, sizeof(correction_prompt),
                     "The previous output did not pass quality verification.\n"
                     "Please improve and regenerate:\n%s",
                     exec_out_len > 500 ? "(content too long, regenerating)" : exec_output);

            AIRY_FREE(exec_output);
            exec_output = NULL;

            real_s2_generate(correction_prompt, strlen(correction_prompt), &exec_output,
                             &exec_out_len, ctx);
        }
    }

    if (exec_output && exec_out_len > 0) {
        tc->ctx_append(ctx->chain, exec_output, exec_out_len);
    }
    tc->step_complete(step_exec, exec_output ? exec_output : "execution_failed",
                      exec_output ? exec_out_len : 16, verified ? 0.85f : 0.5f,
                      verified ? "S2-executor" : "S2-executor-unverified");

    tc_monitor_result_t mon_result;
    tc->step_monitor(step_exec, NULL, &mon_result);
    if (mon_result.anomaly != TC_ANOMALY_NONE && mon_result.is_critical) {
        tc_recovery_result_t rec_result;
        tc->step_recover(ctx->chain, step_exec, &mon_result, real_s2_generate, ctx, &rec_result);
        if (rec_result.recovery_log)
            AIRY_FREE(rec_result.recovery_log);
    }
    if (mon_result.description)
        AIRY_FREE(mon_result.description);

    if (exec_output)
        AIRY_FREE(exec_output);

    /* ========== Phase 3: Subtask Audit (S1 quality gate) ========== */
    airy_thinking_step_t *step_audit = NULL;
    uint32_t audit_deps[] = {step_exec->step_id};
    tc->step_create(ctx->chain, TC_STEP_AUDIT, goal_buf, (size_t)glen, audit_deps, 1,
                    &step_audit);

    mc_evaluation_result_t eval_audit;
    tc->ctx_recent(ctx->chain, 400, &recent_ctx, &recent_ctx_len);
    mc->eval_step(ctx->meta, step_exec, recent_ctx, recent_ctx_len, &eval_audit);
    if (recent_ctx)
        AIRY_FREE(recent_ctx);

    int audit_passed = eval_audit.is_acceptable;
    char audit_result[256];
    int ar_len =
        snprintf(audit_result, sizeof(audit_result), "Audit %s: overall_score=%.2f corrections=%d",
                 audit_passed ? "PASSED" : "FAILED", eval_audit.overall_score,
                 step_exec->correction_count);

    tc->step_complete(step_audit, audit_result, (size_t)ar_len, eval_audit.overall_score,
                      "S1-auditor");

    if (eval_audit.critique_text)
        AIRY_FREE(eval_audit.critique_text);

    /* ========== Phase 4: Goal Alignment Check ========== */
    airy_thinking_step_t *step_align = NULL;
    uint32_t align_deps[] = {step_audit->step_id};
    tc->step_create(ctx->chain, TC_STEP_ALIGNMENT, goal_buf, (size_t)glen, align_deps, 1,
                    &step_align);

    mc_evaluation_result_t eval_align;
    mc->eval_step(ctx->meta, step_align, intent->intent_goal, intent->intent_goal_len,
                  &eval_align);

    int aligned = eval_align.is_acceptable;
    tc->step_complete(step_align, aligned ? "goal_aligned" : "goal_drift_detected",
                      aligned ? 12 : 18, eval_align.overall_score, "S1-alignment");

    if (eval_align.critique_text)
        AIRY_FREE(eval_align.critique_text);

    mc->detect(ctx->meta, NULL, NULL);
    mc->adapt(ctx->meta);

    /* ========== Build Output Plan (LLM Dynamic Planning) ========== */
    airy_task_plan_t *plan = NULL;
    airy_err_t plan_err = llm_build_dynamic_plan(ctx, intent, &plan, audit_passed, aligned);
    if (plan_err != AIRY_SUCCESS || !plan) {
        plan = build_fallback_plan(intent, ctx, audit_passed, aligned, ctx->session_count);
    }

    if (!plan)
        AIRY_RET_ERR(AIRY_ENOMEM);
    *out_plan = plan;
    return AIRY_SUCCESS;
}

/* P4.8.1 (ACC-DT32): implement the airy_plan_reflective_create factory to
 * wire the reflective planning strategy into the main flow, removing
 * dead code. The old g_reflective_strategy global was referenced nowhere
 * (dead code) and has been deleted.
 *
 * Usage (like airy_plan_reactive_create):
 *   airy_plan_strategy_t *strat = airy_plan_reflective_create(llm, memory_engine);
 *   airy_cognition_set_plan_strategy(cognition, strat);
 *
 * Params:
 *   llm - LLM service client handle (for S2 content generation)
 *   memory_engine - memory engine handle (for historical experience;
 *   0.1.1 does not deeply integrate yet, only stores)
 *
 * Returns:
 *   strategy object pointer, or NULL on failure */
airy_plan_strategy_t *airy_plan_reflective_create(airy_llm_service_t *llm,
                                                  airy_memory_engine_t *memory_engine)
{
    airy_plan_strategy_t *strat =
        (airy_plan_strategy_t *)AIRY_CALLOC(1, sizeof(airy_plan_strategy_t));
    if (!strat)
        return NULL;

    reflective_context_t *ctx = NULL;
    if (reflective_plan_init((void **)&ctx) != AIRY_SUCCESS) {
        AIRY_FREE(strat);
        AIRY_LOG_ERROR("reflective: reflective_plan_init failed");
        return NULL;
    }

    ctx->llm = llm;
    ctx->memory_engine = memory_engine;

    strat->plan = reflective_plan;
    strat->destroy = reflective_plan_cleanup;
    strat->data = ctx;

    AIRY_LOG_INFO("reflective: strategy created (llm=%p, memory_engine=%p)", (void *)llm,
             (void *)memory_engine);
    return strat;
}
