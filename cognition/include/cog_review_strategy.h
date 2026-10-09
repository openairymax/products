/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cog_review_strategy.h
 * @brief 认知并行审查（CPR）策略公共契约（products/cognition 策略载荷）。
 *
 * 多子 agent 并行独立审查的公共入口：对用户指令排出多个认知子 agent
 * （认知确认 + 问题/边界/覆盖审查），各自并行独立调用 LLM，再汇总为
 * 认知决策 JSON。机制核 atoms/coreloopthree 仅保留审查数据类型与 ops
 * 注入面（cognitive_review.h），策略载荷由本库承载，由 daemon（think_d）
 * 在启动期经 are_ops_set_cog_review() 注入。
 *
 * LLM 出口经机制核注入的补全闭包（cog_review_complete_fn）解耦：策略
 * 不链接任何机制符号（如 llm_svc_adapter_complete），机制核把适配器/
 * service 句柄放进不透明 ctx，策略仅经函数指针发起真实 LLM 请求。
 */

#ifndef AIRY_RT_COG_REVIEW_STRATEGY_H
#define AIRY_RT_COG_REVIEW_STRATEGY_H

#include "cognitive_review.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 执行一次认知并行审查。
 * @param complete 机制核提供的 LLM 补全闭包（NULL 即无 LLM，走降级）
 * @param complete_ctx 闭包上下文（不透明；由机制核解释）
 * @param input 用户指令
 * @param input_len 指令长度
 * @param max_parallel >0 显式并行度（裁剪到 [1, MAX]）；0/负按硬件探测
 * @param out_result 输出审查结果（调用者 result_free 释放）
 * @return AIRY_SUCCESS（含 LLM 不可用降级）；输入非法 AIRY_EINVAL
 */
airy_err_t cog_review_run(cog_review_complete_fn complete, void *complete_ctx, const char *input,
                          size_t input_len, int max_parallel, cog_review_result_t *out_result);

/**
 * @brief 零值初始化审查结果。
 * @param res 结果对象
 */
void cog_review_result_init(cog_review_result_t *res);

/**
 * @brief 释放审查结果持有的意见/决策内存并复位（NULL 安全）。
 * @param res 结果对象
 */
void cog_review_result_free(cog_review_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_COG_REVIEW_STRATEGY_H */
