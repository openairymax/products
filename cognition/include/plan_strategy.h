/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file plan_strategy.h
 * @brief 规划策略产品库公共接口（products/cognition，M5-4 迁出，台账 §262/§271）。
 *
 * 机制/策略切分：本库为认知规划的策略载荷——分层（hierarchical）、
 * 机器学习（ml）、反应式（reactive）与反思式（reflective）四类规划
 * 策略实现，均实现机制核公共契约 airy_plan_strategy_t
 * （atoms/coreloopthree/include/cognition.h）。机制核不链接本库、
 * 不携带默认策略；策略工厂经 airy_plan_ops.h 契约由 daemon（think_d）
 * 启动期经 are_ops_set_plan() 注入（payload_registry 装配），ops 缺席
 * 时引擎裸启动（BAN-257，process 期 fail fast）。
 *
 * 归位说明：reactive/reflective 原地为 atoms/coreloopthree/src/
 * cognition/think/planner/（§271 迁入），hierarchical/ml 为 §262 首批
 * 迁入；族内七件归格 src/planner/。计划节点回收机制件（plan_node_free /
 * plan_nodes_reclaim）已提升为机制公共契约头 airy_plan_nodes.h
 * （static inline，机制核与机制外策略共享）。
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

/**
 * @brief Create a reactive planning strategy (keyword rules + LLM assist).
 * @param llm LLM service handle (NULL selects the keyword rule path)
 * @return Strategy object, or NULL on failure
 */
airy_plan_strategy_t *airy_plan_reactive_create(airy_llm_service_t *llm);

/**
 * @brief Create a reflective replanning strategy (5-stage pipeline).
 * @param llm LLM service handle (NULL builds keyword fallback plans)
 * @param memory_engine Memory engine for historical-experience lookup
 * @return Strategy object, or NULL on failure
 */
airy_plan_strategy_t *airy_plan_reflective_create(airy_llm_service_t *llm,
                                                  airy_memory_engine_t *memory_engine);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_PRODUCTS_COGNITION_PLAN_STRATEGY_H */
