// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file metacognition_internal.h
 * @brief Metacognition module internal shared declarations (internal contract
 *        after domain split; not a public API).
 *
 * metacognition.c is split into multiple translation units by
 * responsibility, keeping the public API (airy_mc_* in metacognition.h)
 * and ABI completely unchanged:
 *   - metacognition.c  core domain: create/destroy/chain attach + audit records (stats/history/reset)
 *   - mc_evaluate.c    evaluation domain: five-dimension scoring + evaluate_step/evaluate_quick
 *   - mc_correct.c     correction-execution domain: apply_correction + should_self_correct
 *   - mc_calibrate.c   confidence-calibration domain: calibrate_confidence + feedback
 *   - mc_learn.c       persistent-learning domain: pattern detection/strategy learning/pre-correction/threshold adaptation
 *
 * Cross-domain internal helpers (mc_time_now/clampf) are declared here
 * (defined once in their own .c files) and not promoted to the public API.
 * This header is for internal use by the airy_cognition library only and
 * is not installed with the public include set.
 */

#ifndef AIRY_RT_METACOGNITION_INTERNAL_H
#define AIRY_RT_METACOGNITION_INTERNAL_H

#include "metacognition.h"
#include "logging_compat.h"

#include "airy_rt.h"
#include "airy_memory.h"
#include "platform.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Core domain (metacognition.c) */
uint64_t mc_time_now(void);
float clampf(float v, float lo, float hi);

/* Learning domain (mc_learn.c)，供评估域与校准域复用同一模式键与回填路径 */

/* 按「步型_最弱维_分值」构造失败模式键（detect 与 evaluate 共用唯一形）。 */
void mc_pattern_key(tc_step_type_t type, const mc_evaluation_result_t *result, char *out,
                    size_t cap);

/* 查找模式键，返回下标；未找到返回 MC_MAX_PATTERNS。 */
size_t mc_pattern_lookup(const airy_metacognition_t *mc, const char *key);

/* 用最近一条评估记录回填学习（模式计数与策略成功率），仅受 enable_learning 控制。 */
airy_err_t mc_learn_from_feedback(airy_metacognition_t *mc, int was_correct);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_METACOGNITION_INTERNAL_H */
