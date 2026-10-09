/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file lang_router.c
 * @brief 推理语言网关 Phase 1：多因子路由决策引擎。
 *
 * 决策链（优先级从高到低，可解释/可观测）：
 *   1. 硬规则层（不可逾越）：文化/政策类任务 → 强制中文推理与输出
 *   2. 模型原生能力层：Qwen/DeepSeek + 代码/数学 → 中文推理更优
 *   3. 输入语言对齐层：输入英文 + 模型英文高效 → 英文；输入中文 + 中文高效 → 中文
 *   4. 性价比阈值层：lang_ratio 超出阈值切换推理语言；中间区间跟随模型原生
 *
 * 切换阈值从模型画像读取（ratio_high/ratio_low，B6-3/V6.3 同源同值）；
 * 画像未携带阈值时回退内置默认。层 3 与层 4 阈值同源：层 3 捕获输入
 * 语言与效率同方向的请求（推理/输出全对齐），层 4 捕获反方向请求
 * （仅切推理语言，输出跟随用户输入）。
 *
 * 输出语言默认跟随用户输入语言（除非硬规则强制），保证"用户怎么问、
 * 怎么答"的体验一致性；推理语言可在不牺牲体验的前提下按 token 效率切换。
 */

#include "lang_gateway.h"
#include "canonical.h"
#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static airy_err_t route_chain_append(char *buf, size_t buf_sz, size_t *off,
                                     const char *step)
{
    if (!buf || !step)
        return AIRY_EINVAL;

    size_t need = strlen(step) + 4; /* 前后引号 + 逗号/换行 + 终止符 */
    if (*off + need >= buf_sz)
        return AIRY_ENOSPC;

    if (*off > 1) {
        buf[*off] = ',';
        (*off)++;
    }
    (*off) += (size_t)snprintf(buf + *off, buf_sz - *off, "\"%s\"", step);
    return AIRY_EOK;
}

static void decision_reset(airy_lang_decision_t *d)
{
    d->reasoning_lang = AIRY_LANG_UNKNOWN;
    d->output_lang = AIRY_LANG_UNKNOWN;
    AIRY_FREE(d->decision_reason);
    d->decision_reason = NULL;
    AIRY_FREE(d->decision_chain);
    d->decision_chain = NULL;
}

airy_err_t airy_lang_route(const airy_lang_signals_t *signals,
                           const airy_model_profile_t *profile,
                           airy_lang_decision_t *out)
{
    if (!signals || !out)
        return AIRY_EINVAL;

    decision_reset(out);
    out->reasoning_lang = AIRY_LANG_ZH;
    out->output_lang = AIRY_LANG_ZH;

    char chain[1024];
    size_t co = 0;
    chain[0] = '[';
    co = 1;
    chain[1] = '\0';

    /* 1. 硬规则层（不可逾越） */
    if (signals->task_type == AIRY_LANG_TASK_CULTURE ||
        signals->task_type == AIRY_LANG_TASK_POLICY) {
        out->reasoning_lang = AIRY_LANG_ZH;
        out->output_lang = AIRY_LANG_ZH;
        out->decision_reason = airy_strdup("hard_rule_culture_policy");
        route_chain_append(chain, sizeof(chain), &co,
                           "hard_rule: culture/policy → force_zh");
        out->decision_chain = airy_strdup(chain);
        return AIRY_EOK;
    }

    /* 模型画像缺失：仅输入对齐 + 默认原生英文兜底（不阻断主流程） */
    if (!profile) {
        out->reasoning_lang =
            (signals->detected_lang == AIRY_LANG_ZH) ? AIRY_LANG_ZH : AIRY_LANG_EN;
        out->output_lang = signals->detected_lang;
        if (out->output_lang == AIRY_LANG_UNKNOWN)
            out->output_lang = AIRY_LANG_ZH;
        out->decision_reason = airy_strdup("no_profile_input_aligned");
        route_chain_append(chain, sizeof(chain), &co,
                           "no_profile: input language aligned");
        out->decision_chain = airy_strdup(chain);
        return AIRY_EOK;
    }

    double ratio = profile->lang_ratio;
    /* 阈值同源（V6.3）：从画像读取；外部注入画像未设置（<=0）时回退默认 */
    double ratio_high =
        profile->ratio_high > 0.0 ? profile->ratio_high : AIRY_LANG_RATIO_HIGH_DEFAULT;
    double ratio_low =
        profile->ratio_low > 0.0 ? profile->ratio_low : AIRY_LANG_RATIO_LOW_DEFAULT;

    /* 2. 模型原生能力层 */
    if ((signals->task_type == AIRY_LANG_TASK_CODE ||
         signals->task_type == AIRY_LANG_TASK_MATH) &&
        (strstr(profile->family, "Qwen") || strstr(profile->family, "DeepSeek") ||
         strstr(profile->provider, "qwen") || strstr(profile->provider, "deepseek"))) {
        out->reasoning_lang = AIRY_LANG_ZH;
        out->output_lang = signals->detected_lang;
        if (out->output_lang == AIRY_LANG_UNKNOWN)
            out->output_lang = AIRY_LANG_ZH;
        out->decision_reason = airy_strdup("native_advantage_code_math");
        route_chain_append(chain, sizeof(chain), &co,
                           "native: Qwen/DeepSeek + code/math → zh");
        out->decision_chain = airy_strdup(chain);
        return AIRY_EOK;
    }

    /* 3. 输入语言对齐层（阈值同源画像） */
    char step[96];
    if (signals->detected_lang == AIRY_LANG_EN && ratio > ratio_high) {
        out->reasoning_lang = AIRY_LANG_EN;
        out->output_lang = AIRY_LANG_EN;
        out->decision_reason = airy_strdup("input_en_efficient");
        snprintf(step, sizeof(step), "input_en + ratio>%.2f → en", ratio_high);
        route_chain_append(chain, sizeof(chain), &co, step);
        out->decision_chain = airy_strdup(chain);
        return AIRY_EOK;
    }
    if (signals->detected_lang == AIRY_LANG_ZH && ratio < ratio_low) {
        out->reasoning_lang = AIRY_LANG_ZH;
        out->output_lang = AIRY_LANG_ZH;
        out->decision_reason = airy_strdup("input_zh_efficient");
        snprintf(step, sizeof(step), "input_zh + ratio<%.2f → zh", ratio_low);
        route_chain_append(chain, sizeof(chain), &co, step);
        out->decision_chain = airy_strdup(chain);
        return AIRY_EOK;
    }

    /* 4. 性价比阈值层（阈值同源画像） */
    char reason[96];
    if (ratio > ratio_high) {
        out->reasoning_lang = AIRY_LANG_EN;
        out->output_lang = signals->detected_lang;
        if (out->output_lang == AIRY_LANG_UNKNOWN)
            out->output_lang = AIRY_LANG_ZH;
        snprintf(reason, sizeof(reason), "lang_ratio_high_%.2f", ratio);
        out->decision_reason = airy_strdup(reason);
        snprintf(reason, sizeof(reason), "ratio %.2f > %.2f → en", ratio, ratio_high);
        route_chain_append(chain, sizeof(chain), &co, reason);
    } else if (ratio < ratio_low) {
        out->reasoning_lang = AIRY_LANG_ZH;
        out->output_lang = AIRY_LANG_ZH;
        snprintf(reason, sizeof(reason), "lang_ratio_low_%.2f", ratio);
        out->decision_reason = airy_strdup(reason);
        snprintf(reason, sizeof(reason), "ratio %.2f < %.2f → zh", ratio, ratio_low);
        route_chain_append(chain, sizeof(chain), &co, reason);
    } else {
        /* 中庸区间：跟随模型原生语言 */
        out->reasoning_lang = profile->native_lang;
        out->output_lang = signals->detected_lang;
        if (out->output_lang == AIRY_LANG_UNKNOWN)
            out->output_lang = AIRY_LANG_ZH;
        snprintf(reason, sizeof(reason), "native_fallback_%s",
                 profile->native_lang == AIRY_LANG_ZH ? "zh" : "en");
        out->decision_reason = airy_strdup(reason);
        snprintf(reason, sizeof(reason), "ratio %.2f in [%.2f,%.2f] → native(%s)",
                 ratio, ratio_low, ratio_high,
                 profile->native_lang == AIRY_LANG_ZH ? "zh" : "en");
        route_chain_append(chain, sizeof(chain), &co, reason);
    }

    chain[co] = ']';
    chain[co + 1] = '\0';
    out->decision_chain = airy_strdup(chain);
    return AIRY_EOK;
}
