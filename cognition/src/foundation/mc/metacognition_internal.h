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

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_METACOGNITION_INTERNAL_H */
