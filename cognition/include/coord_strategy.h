/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file coord_strategy.h
 * @brief 协调策略公共工厂契约（products/cognition 协调策略载荷）。
 *
 * 多模型输出一致性协调策略的公共创建接口：双模型（dual / 三模型降级为
 * 加权）、多数投票（majority）、加权融合（weighted）、外部仲裁
 * （arbiter model / human）。机制核 atoms/coreloopthree 仅提供
 * airy_coordinator_strategy_t 对象契约（cognition.h），策略实现由本库承载。
 *
 * 消费者创建后经 airy_coordinator_strategy_t 的 coordinate 调用，destroy
 * 接管所有权并释放。LLM 句柄以 commons llm_service_types.h 的
 * llm_service_t（不透明）传递，仲裁策略经注入的 LLM ops 表调用。
 */

#ifndef AIRY_RT_COORD_STRATEGY_H
#define AIRY_RT_COORD_STRATEGY_H

#include "cognition.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a dual-model coordination strategy (1 primary + 2 secondary).
 * @param primary_model Primary model name
 * @param secondary1 Secondary model 1 name
 * @param secondary2 Secondary model 2 name (NULL/empty selects dual mode)
 * @param llm LLM service client handle (borrowed)
 * @return Strategy object, or NULL on failure
 */
airy_coordinator_strategy_t *airy_dmc_create(const char *primary_model, const char *secondary1,
                                             const char *secondary2, llm_service_t *llm);

/**
 * @brief Create a majority-vote coordination strategy.
 * @param model_names Model name array
 * @param model_count Number of models
 * @param llm LLM service client handle (borrowed)
 * @return Strategy object, or NULL on failure
 */
airy_coordinator_strategy_t *airy_mcoord_create(const char **model_names, size_t model_count,
                                                llm_service_t *llm);

/**
 * @brief Create a weighted fusion strategy.
 * @param model_names Model name array
 * @param weights Weight array (sum need not be 1)
 * @param model_count Number of models
 * @param llm LLM service client handle (borrowed)
 * @return Strategy object, or NULL on failure
 */
airy_coordinator_strategy_t *airy_wcoord_create(const char **model_names, const float *weights,
                                                size_t model_count, llm_service_t *llm);

/**
 * @brief Create an external arbitration strategy (model arbitration).
 * @param arbiter_model Arbiter model name
 * @param llm LLM service client handle (borrowed)
 * @return Strategy object, or NULL on failure
 */
airy_coordinator_strategy_t *airy_arbiter_model_create(const char *arbiter_model,
                                                       llm_service_t *llm);

/**
 * @brief Create an external arbitration strategy (human arbitration).
 * @param callback Human callback: receives a question and fills in the answer
 * @return Strategy object, or NULL on failure
 */
airy_coordinator_strategy_t *airy_arbiter_human_create(void (*callback)(const char *question,
                                                                        char *answer,
                                                                        size_t max_len));

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_COORD_STRATEGY_H */
