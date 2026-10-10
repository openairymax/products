// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file mc_calibrate.c
 * @brief Metacognition confidence-calibration domain: calibration and feedback learning.
 */

#include "metacognition_internal.h"

/* ============================================================================
 * Confidence calibration
 * ============================================================================ */

float airy_mc_calibrate_confidence(airy_metacognition_t *mc, float raw_confidence)
{
    if (!mc) {
        AIRY_LOG_WARN("airy_mc_calibrate_confidence: NULL mc parameter, returning raw value");
        return raw_confidence;
    }
    if (!mc->enable_confidence_calibration)
        return raw_confidence;

    raw_confidence = clampf(raw_confidence, 0.0f, 1.0f);

    if (mc->calibrator.calibration_count < 5)
        return raw_confidence;

    float bias = (mc->calibrator.calibration_count > 0) ?
                     mc->calibrator.calibration_sum / (float)mc->calibrator.calibration_count :
                     0.0f;

    float calibrated = raw_confidence - bias * 0.5f;
    calibrated = clampf(calibrated, 0.05f, 0.99f);

    if (calibrated < 0.3f && raw_confidence > 0.7f) {
        mc->calibrator.overconfidence_rate +=
            (1.0f / (float)(mc->calibrator.calibration_count + 1));
    } else if (calibrated > 0.7f && raw_confidence < 0.3f) {
        mc->calibrator.underconfidence_rate +=
            (1.0f / (float)(mc->calibrator.calibration_count + 1));
    }

    return calibrated;
}

airy_err_t airy_mc_feedback(airy_metacognition_t *mc, float predicted_confidence, int was_correct)
{

    if (!mc) {
        AIRY_LOG_ERROR("airy_mc_feedback: NULL mc parameter");
        return AIRY_EINVAL;
    }
    if (!mc->enable_confidence_calibration)
        return AIRY_SUCCESS;

    float actual = was_correct ? 1.0f : 0.0f;
    float error = predicted_confidence - actual;

    mc->calibrator.calibration_sum += error;
    mc->calibrator.calibration_count++;
    mc->calibrator.last_calibration_error = error;

    size_t idx = mc->calibrator.history_index % MC_CALIBRATION_WINDOW;
    mc->calibrator.history[idx].predicted = predicted_confidence;
    mc->calibrator.history[idx].actual = actual;
    mc->calibrator.history_index++;

    return AIRY_SUCCESS;
}
