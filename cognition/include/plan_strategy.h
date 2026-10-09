/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file plan_strategy.h
 * @brief 规划策略产品库公共接口（products/cognition，M5-4 迁出，台账 §262）。
 *
 * 机制/策略切分：本库为认知规划的策略载荷——分层（hierarchical）与
 * 机器学习（ml）两类规划策略实现，均实现机制核公共契约
 * airy_plan_strategy_t（atoms/coreloopthree/include/cognition.h）。机制核
 * 不链接本库、不携带默认策略；两工厂在迁出前扇入为 0（A 通道死件候选，
 * G28），迁出后由生态层按需装配。
 *
 * 归位说明：原地为 atoms/coreloopthree/src/cognition/think/planner/，按
 * "机制留核、策略迁生态层"归一至 products 装配仓。计划节点回收机制件
 * （plan_node_free / plan_nodes_reclaim）随迁提升为机制公共契约头
 * airy_plan_nodes.h（static inline，机制核内 reactive 与机制外策略共享）。
 */

#ifndef AIRY_PRODUCTS_COGNITION_PLAN_STRATEGY_H
#define AIRY_PRODUCTS_COGNITION_PLAN_STRATEGY_H

#include "cognition.h"

#include <stddef.h>

/* LLM 服务句柄：与机制核 llm_client.h 同一不透明类型（struct airy_llm_service）。
 * 本库不引机制核内部头，故在此前向声明对齐，避免引入机制核私有依赖。 */
typedef struct airy_llm_service airy_llm_service_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a hierarchical planning strategy.
 * @param llm LLM service client handle (reserved; 当前实现未消费)
 * @param max_depth Maximum decomposition depth
 * @return Strategy object, or NULL on failure
 */
airy_plan_strategy_t *airy_plan_hierarchical_create(airy_llm_service_t *llm, int max_depth);

/**
 * @brief Create an ML-based planning strategy.
 * @param model_path Model file path
 * @param llm LLM service client (optional, for fallback)
 * @return Strategy object, or NULL on failure
 */
airy_plan_strategy_t *airy_plan_ml_create(const char *model_path, airy_llm_service_t *llm);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_PRODUCTS_COGNITION_PLAN_STRATEGY_H */
