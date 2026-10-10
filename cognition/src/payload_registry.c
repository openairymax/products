/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file payload_registry.c
 * @brief TC/MC/Intent/Plan ops 静态装配——机制核契约面到载荷实现符号的唯一映射点。
 *
 * 装配约定（0.1.19 M5-4）：30 项 ops 中 23 项直引载荷库 AIRY_API 符号
 * （零包装）；5 项经 static 适配器——ctx_append/ctx_recent/wm_store/
 * wm_retrieve 因契约头 tc.h 将 ctx/wm 子件寻址统一为 chain 首参（机制侧
 * 只持 chain 句柄，子件指针是载荷内部知识），适配器在此解析 chain->
 * ctx_window/working_mem 后转发；set_thresh 因载荷类型对机制侧 opaque，
 * critique 阈值配置经此通道下发。字段为空即装配缺陷，编译期以指定初始
 * 化器全字段显式列举强制暴露。
 */

#include "payload_registry.h"

#include "foundation/metacognition.h"
#include "foundation/thinking_chain.h"
#include "intent_classifier.h"
#include "plan_strategy.h"

/* ---- chain 首参适配器：解析内嵌子件后转发实现 ---- */

static ssize_t tc_ctx_append(airy_thinking_chain_t *chain, const char *data, size_t len)
{
    return airy_tc_context_window_append(chain->ctx_window, data, len);
}

static airy_err_t tc_ctx_recent(airy_thinking_chain_t *chain, size_t token_count, char **out_data,
                                size_t *out_len)
{
    return airy_tc_context_window_get_recent(chain->ctx_window, token_count, out_data, out_len);
}

static airy_err_t tc_wm_store(airy_thinking_chain_t *chain, const char *key, const void *value,
                              size_t value_size, const char *type, int pin)
{
    return airy_tc_working_memory_store(chain->working_mem, key, value, value_size, type, pin);
}

static airy_err_t tc_wm_retrieve(airy_thinking_chain_t *chain, const char *key, void **out_value,
                                 size_t *out_size)
{
    return airy_tc_working_memory_retrieve(chain->working_mem, key, out_value, out_size);
}

static void mc_set_thresh(airy_metacognition_t *mc, float acceptance, float auto_correct)
{
    mc->acceptance_threshold = acceptance;
    mc->auto_correct_threshold = auto_correct;
}

/* ---- ops 静态装配表 ---- */

static const airy_tc_ops_t g_tc_payload_ops = {
    .chain_create = airy_tc_chain_create,
    .chain_destroy = airy_tc_chain_destroy,
    .chain_start = airy_tc_chain_start,
    .chain_stop = airy_tc_chain_stop,
    .chain_set_mem = airy_tc_chain_set_memory,
    .chain_health = airy_tc_chain_health_check,
    .step_create = airy_tc_step_create,
    .step_complete = airy_tc_step_complete,
    .step_monitor = airy_tc_step_monitor,
    .step_recover = airy_tc_step_recover,
    .step_to_mem = airy_tc_step_write_to_memory,
    .meta_to_mem = airy_tc_meta_inform_mem,
    .ctx_append = tc_ctx_append,
    .ctx_recent = tc_ctx_recent,
    .ctx_prepop = airy_tc_context_window_prepop,
    .wm_store = tc_wm_store,
    .wm_retrieve = tc_wm_retrieve,
};

static const airy_mc_ops_t g_mc_payload_ops = {
    .create = airy_mc_create,
    .destroy = airy_mc_destroy,
    .set_chain = airy_mc_set_chain,
    .eval_step = airy_mc_evaluate_step,
    .correct = airy_mc_correct,
    .preempt = airy_mc_preemptive_check,
    .feedback = airy_mc_feedback,
    .detect = airy_mc_detect_patterns,
    .adapt = airy_mc_adapt_threshold,
    .set_thresh = mc_set_thresh,
};

static const airy_intent_ops_t g_intent_payload_ops = {
    .classify = airy_intent_classify,
};

static const airy_plan_ops_t g_plan_payload_ops = {
    .create_reactive = airy_plan_reactive_create,
    .create_reflective = airy_plan_reflective_create,
};

const airy_tc_ops_t *cog_payload_tc(void)
{
    return &g_tc_payload_ops;
}

const airy_mc_ops_t *cog_payload_mc(void)
{
    return &g_mc_payload_ops;
}

const airy_intent_ops_t *cog_payload_intent(void)
{
    return &g_intent_payload_ops;
}

const airy_plan_ops_t *cog_payload_plan(void)
{
    return &g_plan_payload_ops;
}
