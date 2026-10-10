// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file intent_classifier.h
 * @brief Intent classifier payload interface.
 *
 * 值类型契约收口于 intent.h（机制核契约头，0.1.19 M5-4 §269）；本头
 * 仅声明策略载荷的分类入口，供 ops 表装配（payload_registry）与
 * 直链消费者使用。关键词库、评分策略、类型名表属载荷内部实现。
 */

#ifndef AIRY_RT_INTENT_CLASSIFIER_H
#define AIRY_RT_INTENT_CLASSIFIER_H

#include "intent.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Classify the user intent.
 * @param input User input text
 * @param input_len Input length
 * @param result Output classification result
 * @return 0 on success, error code on failure
 */
int airy_intent_classify(const char *input, size_t input_len, airy_intent_classification_t *result);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_INTENT_CLASSIFIER_H */
