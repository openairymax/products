/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file signal_extractor.c
 * @brief 推理语言网关 Phase 1：信号提取（语言检测/任务分类/上下文估算）。
 *
 * 纯本地启发式实现，零 LLM 调用、零外部依赖。策略可经关键词表与阈值
 * 调整，后续升级为 ML 分类器不改变公共 API（airy_lang_detect /
 * airy_lang_classify_task）。
 */

#include "lang_gateway.h"
#include "canonical.h"

#include <ctype.h>
#include <string.h>

/* 文化/政策硬约束关键词（路由决策的不可逾越层） */
static const char *const k_culture_keywords[] = {
    "成语", "典故", "古诗", "诗词", "论语", "道德经", "孙子兵法",
    "四大名著", "红楼梦", "西游记", "三国演义", "水浒传",
};

static const char *const k_policy_keywords[] = {
    "政策", "法规", "法律", "宪法", "民法典", "行政法规", "国务院",
};

/* 任务分类关键词 */
static const char *const k_code_marks[] = {
    "写代码", "实现", "编写", "函数", "class", "import", "def ",
    "function", "代码", "bug", "编译", "算法", "排序", "程序", "脚本",
};

static const char *const k_math_marks[] = {
    "计算", "方程", "导数", "积分", "矩阵", "求和", "证明",
    "solve", "equation", "calculate", "math",
};

static const char *const k_culture_marks[] = {
    "成语", "典故", "古诗", "诗词", "论语", "道德经", "孙子兵法",
    "红楼梦", "西游记", "三国演义", "水浒传", "传统文化",
};

static const char *const k_policy_marks[] = {
    "政策", "法规", "法律", "宪法", "民法典", "行政法规", "国务院", "合规",
};

size_t airy_lang_utf8_sanitize(const char *in, char *out, size_t out_sz)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;

    if (!in || !out || out_sz == 0)
        return 0;

    while (*p && o + 1 < out_sz) {
        unsigned char c = *p;
        size_t need = 0;

        if (c < 0x80) {
            need = 1;
        } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            need = 4;
        }

        if (need == 0 || o + need >= out_sz) {
            /* 非法字节或缓冲区不足：替换为 U+FFFD (EF BF BD)。
             * 八进制转义使字节常量在 signed char 上可表示，规避 MSVC
             * C4310（(char)0xEF 显式截断告警，/W4 /WX 下致编译失败）。 */
            if (o + 3 < out_sz) {
                out[o++] = '\357';
                out[o++] = '\277';
                out[o++] = '\275';
            }
            p++;
            continue;
        }

        for (size_t i = 0; i < need; i++)
            out[o++] = (char)p[i];
        p += need;
    }

    out[o] = '\0';
    return o;
}

uint32_t airy_lang_chinese_count(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    uint32_t n = 0;

    if (!text)
        return 0;

    while (*p) {
        /* U+4E00~U+9FFF 的 UTF-8 编码为 E4 B8 80 ~ E9 BF BF（3 字节） */
        if (*p == 0xE4 || *p == 0xE5 || *p == 0xE6 || *p == 0xE7 ||
            *p == 0xE8 || *p == 0xE9) {
            uint32_t cp = ((uint32_t)(*p & 0x0F) << 12) |
                          ((uint32_t)(p[1] & 0x3F) << 6) |
                          (uint32_t)(p[2] & 0x3F);
            if (cp >= 0x4E00 && cp <= 0x9FFF)
                n++;
            p += 3;
            continue;
        }
        if (*p < 0x80)
            p++;
        else if ((*p & 0xE0) == 0xC0)
            p += 2;
        else if ((*p & 0xF0) == 0xE0)
            p += 3;
        else if ((*p & 0xF8) == 0xF0)
            p += 4;
        else
            p++;
    }
    return n;
}

uint32_t airy_lang_ascii_count(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    uint32_t n = 0;

    if (!text)
        return 0;

    while (*p) {
        if (*p < 0x80)
            n++;
        p++;
    }
    return n;
}

const char *airy_lang_name(airy_lang_t lang)
{
    switch (lang) {
    case AIRY_LANG_ZH:
        return "中文";
    case AIRY_LANG_EN:
        return "英文";
    default:
        return "未知";
    }
}

/* 代码特征：符号密集度超过阈值视为代码/结构化文本 */
static int text_looks_like_code(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t total = 0;
    size_t sym = 0;

    if (!text)
        return 0;

    while (*p) {
        unsigned char c = *p;
        if (c < 0x80) {
            total++;
            if (c == '{' || c == '}' || c == '[' || c == ']' || c == '(' ||
                c == ')' || c == '=' || c == '+' || c == '*' || c == '/' ||
                c == ';' || c == '<' || c == '>')
                sym++;
        }
        p++;
    }
    if (total == 0)
        return 0;
    return (double)sym / (double)total > 0.25;
}

/* 仅含 ASCII 可打印字符（无空格/中文/标点）且含结构标记 → 疑似代码/标识符串 */
static int text_is_code_block(const char *text)
{
    if (!text || !text[0])
        return 0;

    int has_struct = 0;
    int has_space = 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n < 4096;
         p++, n++) {
        if (*p >= 0x80)
            return 0;
        if (*p == ' ' || *p == '\t' || *p == '\n')
            has_space = 1;
        if (strchr("{}[]();=<>,*+/&|!?\"'`", (char)*p))
            has_struct = 1;
    }
    return has_struct && !has_space;
}

airy_err_t airy_lang_detect(const char *text, airy_lang_t *out_lang, double *out_conf)
{
    airy_lang_t lang = AIRY_LANG_UNKNOWN;
    double conf = 0.0;

    if (!text || !text[0])
        return AIRY_EINVAL;

    size_t len = strlen(text);
    if (len < 3) {
        conf = 0.0;
        lang = AIRY_LANG_UNKNOWN;
    } else if (text_looks_like_code(text) || text_is_code_block(text)) {
        /* 代码/结构化文本按英文处理（英文代码 token 开销更小） */
        lang = AIRY_LANG_EN;
        conf = 0.6;
    } else {
        size_t ascii_n = airy_lang_ascii_count(text);
        double ascii_ratio = (double)ascii_n / (double)len;
        if (ascii_ratio > 0.85) {
            lang = AIRY_LANG_EN;
            conf = 0.9;
        } else {
            size_t ch = airy_lang_chinese_count(text);
            double ch_ratio = (double)ch / (double)len;
            if (ch_ratio > 0.30) {
                lang = AIRY_LANG_ZH;
                conf = ch_ratio > 0.5 ? 0.95 : 0.75;
            } else {
                /* 混合语言：中文占比 5%~30%，置信度打折 */
                lang = AIRY_LANG_ZH;
                conf = 0.55;
            }
        }
    }

    if (out_lang)
        *out_lang = lang;
    if (out_conf)
        *out_conf = conf;
    return AIRY_EOK;
}

static int contains_any(const char *text, const char *const *keys, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strstr(text, keys[i]))
            return 1;
    }
    return 0;
}

airy_err_t airy_lang_classify_task(const char *text, airy_lang_task_t *out_task)
{
    airy_lang_task_t task = AIRY_LANG_TASK_QA;

    if (!text || !text[0])
        return AIRY_EINVAL;

    if (contains_any(text, k_culture_marks,
                     sizeof(k_culture_marks) / sizeof(k_culture_marks[0]))) {
        task = AIRY_LANG_TASK_CULTURE;
    } else if (contains_any(text, k_policy_marks,
                            sizeof(k_policy_marks) / sizeof(k_policy_marks[0]))) {
        task = AIRY_LANG_TASK_POLICY;
    } else if (contains_any(text, k_code_marks,
                            sizeof(k_code_marks) / sizeof(k_code_marks[0]))) {
        task = AIRY_LANG_TASK_CODE;
    } else if (contains_any(text, k_math_marks,
                            sizeof(k_math_marks) / sizeof(k_math_marks[0]))) {
        task = AIRY_LANG_TASK_MATH;
    } else {
        task = AIRY_LANG_TASK_GENERAL;
    }

    /* 文化/政策关键词为硬约束，直接命中（即使文本也含代码特征） */
    if (contains_any(text, k_culture_keywords,
                     sizeof(k_culture_keywords) / sizeof(k_culture_keywords[0])))
        task = AIRY_LANG_TASK_CULTURE;
    else if (contains_any(text, k_policy_keywords,
                          sizeof(k_policy_keywords) / sizeof(k_policy_keywords[0])))
        task = AIRY_LANG_TASK_POLICY;

    if (out_task)
        *out_task = task;
    return AIRY_EOK;
}

airy_err_t airy_lang_estimate_tokens(const char *text, uint32_t *out_tokens)
{
    if (!text || !text[0] || !out_tokens)
        return AIRY_EINVAL;

    uint32_t chinese = airy_lang_chinese_count(text);

    /* 英文单词数（简单统计） */
    uint32_t words = 0;
    int in_word = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9')) {
            if (!in_word) {
                in_word = 1;
                words++;
            }
        } else {
            in_word = 0;
        }
    }

    *out_tokens = (uint32_t)(chinese * 1.8 + words * 1.2);
    return AIRY_EOK;
}
