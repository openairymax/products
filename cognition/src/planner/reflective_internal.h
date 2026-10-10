// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file reflective_internal.h
 * @brief Reflective planning strategy — internal cross-TU contract.
 *
 * 共享 reflective 域内部类型与跨编译单元（reflective.c / reflective_llm_plan.c）
 * 函数声明。保持单一定义原则（reflective_context_t 生命周期与外部可见 API 在
 * reflective.c；LLM 动态计划构建域在 reflective_llm_plan.c）。
 *
 * 0.1.19 M5-4 §271 自 atoms/coreloopthree src/cognition/think/planner/
 * 迁入 products/cognition（族内归格 src/planner/）。
 */

#ifndef AIRY_REFLECTIVE_INTERNAL_H
#define AIRY_REFLECTIVE_INTERNAL_H

#include "airy_rt.h"
#include "cognition.h"
#include "mc.h"
#include "tc.h"
#include "llm_client.h"
#include "plan_strategy.h"

#include <stddef.h>
#include <stdint.h>

/* reflective.c / reflective_llm_plan.c 共享上下文 */
typedef struct {
    airy_thinking_chain_t *chain;
    airy_metacognition_t *meta;
    airy_llm_service_t *llm;
    airy_memory_engine_t *memory_engine;
    int initialized;
    uint64_t session_count;
    char *last_goal;
    int max_verify_rounds;
    float acceptance_threshold;
} reflective_context_t;

/* LLM 动态计划构建域（reflective_llm_plan.c）——被 reflective_plan 管线调用 */
airy_err_t llm_build_dynamic_plan(reflective_context_t *, const airy_intent_t *,
                                  airy_task_plan_t **, int, int);
airy_task_plan_t *build_fallback_plan(const airy_intent_t *, reflective_context_t *, int, int,
                                      uint64_t);

#endif /* AIRY_REFLECTIVE_INTERNAL_H */
