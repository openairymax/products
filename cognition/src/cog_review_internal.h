/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cog_review_internal.h
 * @brief 认知并行审查策略内部共享契约（策略载荷私有）。
 *
 * 仅在 products/cognition/src/cog_review.c 与单元测试之间共享：把纯函数
 * 汇总入口 cog_review_aggregate() 暴露给直测，免去测试宏与 -Wmissing-prototypes
 * 冲突。公共策略契约见 include/cog_review_strategy.h；机制核
 * atoms/coreloopthree/include/cognitive_review.h 仅提供数据类型与 ops 注入面。
 */

#ifndef AIRY_RT_COG_REVIEW_INTERNAL_H
#define AIRY_RT_COG_REVIEW_INTERNAL_H

#include "cognitive_review.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 汇总各子 agent 意见为认知决策 JSON（纯函数，可单测）。
 * @param res 含 opinions/opinion_count 的结果对象（原地写入 decision_json）
 * @return AIRY_SUCCESS，或 AIRY_ENOMEM
 */
airy_err_t cog_review_aggregate(cog_review_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_COG_REVIEW_INTERNAL_H */
