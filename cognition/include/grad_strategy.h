// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_strategy.h
 * @brief GRAD 策略载荷：计划级批判环策略入口（M5-4 迁出）。
 *
 * 模型 A（t2）生成/修订 DAG 计划 → 模型 C（t1-p）确定性四路验证 →
 * 模型 B（t1-f）上下文仲裁的修复环，保证任何进入执行态的计划都经过
 * 同一逻辑守门。机制核（atoms/coreloopthree）仅保留数据类型、决策链
 * 通道与 ops 注入面；本模块承载完整批判环策略与 fallback 降级检测。
 * LLM 出口经 airy_grad_complete_fn 补全闭包解耦——策略侧不与机制核
 * LLM 句柄链接耦合，由 daemon（think_d）在启动期经 are_ops_set_grad()
 * 注入。
 *
 * 结果契约（airy_grad_outcome_t，见 grad.h）：
 *   - 返回 AIRY_SUCCESS 且 final_plan 非 NULL：收敛并产出精化计划；
 *   - 返回 AIRY_SUCCESS 且 final_plan 为 NULL：seed 计划首轮即通过批判门；
 *   - 返回其他错误码：未收敛/降级（degraded=1 表示收敛于 fallback 空壳），
 *     final_plan 必为 NULL（fail-closed，调用方须转人工评审）。
 *
 * @see atoms/coreloopthree/docs/GRAD.md
 */

#ifndef AIRY_PRODUCTS_COGNITION_GRAD_STRATEGY_H
#define AIRY_PRODUCTS_COGNITION_GRAD_STRATEGY_H

#include "grad.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 运行 GRAD 计划级批判环（airy_grad_ops_t.run 实现）。
 *
 * @param launch [in] 运行快照（全 BORROW，调用方保证作用域）
 * @param seed_plan [in] 初始计划（可 NULL = 由模型 A 全新生成；BORROW）
 * @param out_outcome [out] 运行结果（非 NULL，OUT）
 * @return AIRY_SUCCESS 收敛；其他错误码未收敛/降级（fail-closed）
 *
 * @ownership out_outcome->final_plan: OWNER（调用方 airy_task_plan_free）
 */
airy_err_t grad_run(const airy_grad_launch_t *launch, const airy_task_plan_t *seed_plan,
                    airy_grad_outcome_t *out_outcome);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_PRODUCTS_COGNITION_GRAD_STRATEGY_H */
