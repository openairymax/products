/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gccp_strategy.h
 * @brief GCCP 策略载荷公共契约（products/cognition 策略载荷）。
 *
 * Goal Complete Confirmation Protocol（GCCP）的两阶段交互目标澄清：
 * probe 产出初始目标与问题集，step 逐问推进，confirm 合并答案为完整
 * 目标模型。机制核 atoms/coreloopthree 仅保留数据类型、两段式异步
 * 通道与 ops 注入面（gccp.h），策略载荷由本库承载，由 daemon
 * （think_d）在启动期经 are_ops_set_gccp() 注入。
 *
 * LLM 出口经机制核注入的补全闭包（airy_gccp_complete_fn）解耦：策略
 * 不链接任何机制核符号，机制核把适配器/service 句柄放进不透明 ctx，
 * 策略仅经函数指针发起真实 LLM 请求；响应释放经 ops 注入表完成。
 */

#ifndef AIRY_RT_GCCP_STRATEGY_H
#define AIRY_RT_GCCP_STRATEGY_H

#include "gccp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 目标澄清探测：推理输入是否需要澄清，产出初始目标 + 问题集。
 * @param complete 机制核提供的 LLM 补全闭包（NULL 即无 LLM，走降级）
 * @param complete_ctx 闭包上下文（不透明；由机制核解释）
 * @param model LLM 模型标识（NULL 走 provider 默认）
 * @param input 用户指令
 * @param input_len 指令长度（非 0）
 * @param out_probe 输出探测结果（调用者 airy_gccp_probe_free 释放）
 * @return AIRY_EOK（含 LLM 不可用降级）；输入非法 AIRY_EINVAL
 */
airy_err_t gccp_probe(airy_gccp_complete_fn complete, void *complete_ctx, const char *model,
                      const char *input, size_t input_len, airy_gccp_probe_t **out_probe);

/**
 * @brief 目标确认：合并用户答案，产出完整目标模型。
 * @param complete LLM 补全闭包（NULL 走降级）
 * @param complete_ctx 闭包上下文
 * @param model LLM 模型标识
 * @param input 用户指令
 * @param input_len 指令长度（非 0）
 * @param answers_json 已收集答案（JSON，可为 NULL）
 * @param out_goal 输出目标（调用者 airy_gccp_goal_free 释放）
 * @return AIRY_EOK（含降级）；输入非法 AIRY_EINVAL
 */
airy_err_t gccp_confirm(airy_gccp_complete_fn complete, void *complete_ctx, const char *model,
                        const char *input, size_t input_len, const char *answers_json,
                        airy_gccp_goal_t **out_goal);

/**
 * @brief 逐问推进：对已答内容决策收敛或给出下一问。
 * @param complete LLM 补全闭包（NULL 降级为无追问）
 * @param complete_ctx 闭包上下文
 * @param model LLM 模型标识
 * @param input 用户指令
 * @param input_len 指令长度
 * @param answers_json 已收集答案（JSON，可为 NULL）
 * @param answered 用户是否回答了上一问（0=跳过 → done=1 收敛）
 * @param next_q 刚问过的问题（仅用于组装上下文，绝不回写——防同题死循环）
 * @param out_step 输出推进结果
 * @return AIRY_EOK；输入非法 AIRY_EINVAL
 */
airy_err_t gccp_step(airy_gccp_complete_fn complete, void *complete_ctx, const char *model,
                     const char *input, size_t input_len, const char *answers_json, int answered,
                     const airy_gccp_question_t *next_q, airy_gccp_step_t *out_step);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GCCP_STRATEGY_H */
