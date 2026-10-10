/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file dispatch_strategy.h
 * @brief 分发策略产品库公共接口（products/cognition，M5-4 迁出，台账 §261）。
 *
 * 机制/策略切分：本库为认知分发的策略载荷——加权 / 轮询 / 优先级 / ML
 * 四类策略实现，均实现机制核公共契约 airy_dispatching_strategy_t
 * （atoms/coreloopthree/include/cognition.h）。机制核不链接本库、不携带
 * 默认策略：消费者以本库工厂创建策略实例后，经
 * airy_cognition_set_dispatching_strategy() 或
 * airy_cognition_create*_take() 的 disp_strategy 形参注入；机制核在该
 * 形参为 NULL 时 fail-fast（BAN-257/DT-01）。
 *
 * 归位说明：原地为 atoms/coreloopthree/src/cognition/think/dispatcher/，
 * 按"机制留核、策略迁生态层"归一至 products 装配仓；符号族随迁统一为
 * airy_dispatching_*，修复原头声明（airy_dispatching_*）与实现
 * （airy_disp_*）命名不一致的根因缺陷；原 dispatch_strategy.h 中无人
 * 消费的 airy_dispatcher_base_t 前向声明一并清除。
 */

#ifndef AIRY_PRODUCTS_COGNITION_DISPATCH_STRATEGY_H
#define AIRY_PRODUCTS_COGNITION_DISPATCH_STRATEGY_H

#include "agent_registry.h"
#include "cognition.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 加权分发策略配置（三因子权重）。
 *
 * 以 airy_dispatching_ 前缀维持独立命名空间，避免与消费者侧
 * 头文件中的同名类型冲突。
 */
typedef struct airy_dispatching_weighted_config {
    float cost_weight;
    float perf_weight;
    float trust_weight;
} airy_dispatching_weighted_config_t;

/**
 * @brief Create a weighted dispatch strategy.
 * @param config [in] Weight config (defaults used when NULL)
 * @param registry_ctx [in] Registry context
 * @param get_agents_func [in] Function to fetch the candidate agent list
 * @return Strategy object, or NULL on failure
 */
airy_dispatching_strategy_t *airy_dispatching_weighted_create(
    const airy_dispatching_weighted_config_t *config, void *registry_ctx,
    agent_registry_get_agents_func get_agents_func);

/**
 * @brief Create a round-robin dispatch strategy.
 * @param registry_ctx [in] Registry context
 * @param get_agents_func [in] Function to fetch the candidate agent list
 * @return Strategy object, or NULL on failure
 */
airy_dispatching_strategy_t *airy_dispatching_round_robin_create(
    void *registry_ctx, agent_registry_get_agents_func get_agents_func);

/**
 * @brief Create a priority dispatch strategy.
 * @param registry_ctx [in] Registry context
 * @param get_agents_func [in] Function to fetch the candidate agent list
 * @param out_strategy [out] Output strategy instance
 * @return AIRY_SUCCESS on success, AIRY_EINVAL/AIRY_ENOMEM otherwise
 */
airy_err_t airy_dispatching_priority_create(void *registry_ctx,
                                            agent_registry_get_agents_func get_agents_func,
                                            airy_dispatching_strategy_t **out_strategy);

/**
 * @brief Create an ML-based dispatch strategy.
 * @param model_path [in] Model file path (NULL/empty uses defaults)
 * @param registry_ctx [in] Registry context
 * @param get_agents_func [in] Function to fetch the candidate agent list
 * @return Strategy object, or NULL on failure
 */
airy_dispatching_strategy_t *airy_dispatching_ml_create(
    const char *model_path, void *registry_ctx, agent_registry_get_agents_func get_agents_func);

/**
 * @brief Report the outcome of an ML dispatch decision (online adaptation).
 * @param strategy [in] Strategy instance created by airy_dispatching_ml_create()
 * @param reward [in] Reward in [0,1] (clamped)
 * @return AIRY_SUCCESS on success, AIRY_EINVAL on invalid arguments
 */
airy_err_t airy_dispatching_ml_report_outcome(airy_dispatching_strategy_t *strategy, float reward);

/**
 * @brief Get the moving-average reward of an ML dispatch strategy.
 * @param strategy [in] Strategy instance (NULL yields 0.0f)
 * @return Average reward, or 0.0f when unavailable
 */
float airy_dispatching_ml_avg_reward(const airy_dispatching_strategy_t *strategy);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_PRODUCTS_COGNITION_DISPATCH_STRATEGY_H */
