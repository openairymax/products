/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file output_post_processor.c
 * @brief 推理语言网关 Phase 3：输出后处理（语言漂移检测 + 术语一致性 + 润色）。
 *
 * 1. <thinking>/<answer> 双标签解析：answer 为最终输出，thinking 记入遥测
 *    （推理链条原始证据，随 canonical.telemetry_json 保留）；
 * 2. 语言漂移检测：期望输出语言字符占比低于阈值（60%）判定漂移；
 * 3. 术语一致性：中英术语表替换（词边界匹配，避免破坏代码内标识符）；
 * 4. 意译润色：去除直译腔（多余空格/换行归一）。
 */

#include "lang_gateway.h"
#include "canonical.h"
#include "airy_memory.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* 通用术语表（英文 → 中文），词边界匹配，仅作用于英文单词。
 * 替换在恰好定长缓冲上原位进行，表项必须满足 sizeof(zh) <= sizeof(en)
 *（含终止符，即替换后不变长）——由下方 _Static_assert 编译期强制，
 * 违例条目（曾致静默死配置）无法通过编译。 */
#define AIRY_LANG_GLOSSARY(X)          \
    X("gradient descent", "梯度下降")   \
    X("backpropagation", "反向传播")    \
    X("attention mechanism", "注意力机制") \
    X("overfitting", "过拟合")          \
    X("underfitting", "欠拟合")         \
    X("loss function", "损失函数")      \
    X("activation function", "激活函数") \
    X("regularization", "正则化")       \
    X("embedding", "嵌入")              \
    X("semantic", "语义")               \
    X("learning rate", "学习率")        \
    X("batch size", "批大小")

#define GL_ROW(en, zh) {en, zh},
static const struct {
    const char *en;
    const char *zh;
} k_glossary[] = {AIRY_LANG_GLOSSARY(GL_ROW)};
#undef GL_ROW

#define GL_SHRINK(en, zh) \
    _Static_assert(sizeof(en) >= sizeof(zh), "glossary entry must not grow output");
AIRY_LANG_GLOSSARY(GL_SHRINK)
#undef GL_SHRINK
#undef AIRY_LANG_GLOSSARY

#define AIRY_LANG_DRIFT_THRESHOLD 0.60

/** 解析 <thinking>/<answer> 标签；无标签时全文为 answer。 */
static void parse_tags(const char *raw, char **out_thinking, char **out_answer)
{
    const char *t_start = strstr(raw, "<thinking");
    const char *t_end = t_start ? strstr(t_start, "</thinking>") : NULL;
    const char *a_start = strstr(raw, "<answer");
    const char *a_end = a_start ? strstr(a_start, "</answer>") : NULL;

    *out_thinking = NULL;
    *out_answer = NULL;

    if (t_start && t_end) {
        const char *body = t_start + strlen("<thinking>");
        size_t len = (size_t)(t_end - body);
        if (len > 0) {
            *out_thinking = AIRY_MALLOC(len + 1);
            if (*out_thinking) {
                AIRY_MEMCPY(*out_thinking, body, len);
                (*out_thinking)[len] = '\0';
            }
        }
    }

    if (a_start && a_end) {
        const char *body = a_start + strlen("<answer>");
        size_t len = (size_t)(a_end - body);
        *out_answer = AIRY_MALLOC(len + 1);
        if (*out_answer) {
            AIRY_MEMCPY(*out_answer, body, len);
            (*out_answer)[len] = '\0';
        }
    } else if (t_start && t_end) {
        /* 有 thinking 无 answer：剥离 thinking 后取剩余 */
        const char *rest = t_end + strlen("</thinking>");
        *out_answer = AIRY_STRDUP(rest);
    } else {
        *out_answer = AIRY_STRDUP(raw);
    }
}

/** 不区分大小写的子串查找（strcasestr 非 C 标准，Windows 无此函数）。 */
static const char *lang_strcasestr(const char *haystack, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0)
        return haystack;

    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return p;
    }
    return NULL;
}

/** 词边界匹配替换（仅处理 ASCII 边界）。表项编译期保证替换不变长
 *（见 k_glossary 处 _Static_assert），原位收缩移动安全。 */
static void apply_glossary(char *text)
{
    for (size_t i = 0; i < sizeof(k_glossary) / sizeof(k_glossary[0]); i++) {
        size_t en_len = strlen(k_glossary[i].en);
        size_t zh_len = strlen(k_glossary[i].zh);
        char *p = text;
        while ((p = (char *)lang_strcasestr(p, k_glossary[i].en)) != NULL) {
            int left_ok = (p == text) || !isalnum((unsigned char)p[-1]) ||
                          (p[-1] == '_');
            int right_ok = !isalnum((unsigned char)p[en_len]) &&
                           (p[en_len] != '_');
            if (left_ok && right_ok) {
                AIRY_MEMCPY(p, k_glossary[i].zh, zh_len);
                if (en_len > zh_len)
                    AIRY_MEMMOVE(p + zh_len, p + en_len, strlen(p + en_len) + 1);
                p += zh_len;
            } else {
                p += en_len;
            }
        }
    }
}

/** 空白/换行归一（去除直译腔）。 */
static void normalize_space(char *text)
{
    char *dst = text;
    int prev_space = 0;

    for (const char *src = text; *src; src++) {
        if (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') {
            if (!prev_space) {
                *dst++ = ' ';
                prev_space = 1;
            }
        } else {
            *dst++ = *src;
            prev_space = 0;
        }
    }
    while (dst > text && dst[-1] == ' ')
        dst--;
    *dst = '\0';
}

/* Phase 3 独立入口：漂移检测（供网关/上层使用） */
int airy_lang_detect_drift(const char *text, airy_lang_t expected, double *out_ratio)
{
    if (!text || !text[0])
        return 1;

    size_t len = strlen(text);
    if (expected == AIRY_LANG_ZH) {
        double ratio = (double)airy_lang_chinese_count(text) / (double)len;
        if (out_ratio)
            *out_ratio = ratio;
        return ratio < AIRY_LANG_DRIFT_THRESHOLD;
    }
    double ratio = (double)airy_lang_ascii_count(text) / (double)len;
    if (out_ratio)
        *out_ratio = ratio;
    return ratio < AIRY_LANG_DRIFT_THRESHOLD;
}

airy_err_t airy_lang_gateway_post_process(airy_lang_gateway_t *gw, const char *text,
                                          airy_lang_t expected_lang, char **out)
{
    if (!gw || !text || !out)
        return AIRY_EINVAL;

    char *thinking = NULL;
    char *answer = NULL;
    parse_tags(text, &thinking, &answer);

    /* answer 为空（仅思考内容）时降级为原始文本，避免空输出 */
    const char *body = (answer && answer[0]) ? answer : text;
    char *buf = AIRY_STRDUP(body);
    AIRY_FREE(answer);
    AIRY_FREE(thinking);
    if (!buf)
        return AIRY_ENOMEM;

    /* 语言漂移检测：漂移不阻断，进入术语/润色归一（计数供可观测性） */
    if (airy_lang_detect_drift(buf, expected_lang, NULL)) {
        airy_mtx_lock(&gw->prof_lock);
        gw->drift_detected++;
        airy_mtx_unlock(&gw->prof_lock);
    }

    /* 术语一致性（期望中文输出时应用术语表） */
    if (expected_lang == AIRY_LANG_ZH)
        apply_glossary(buf);

    /* 意译润色：去除直译腔（多余空白归一） */
    normalize_space(buf);

    *out = buf;
    return AIRY_EOK;
}
