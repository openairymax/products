// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file intent_classifier.c
 * @brief Intent classifier implementation.
 */

#include "intent_classifier.h"

#include "text_utils.h"
#include "airy_memory.h"
#include "error.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *g_query_keywords[] = {"what", "where", "when", "why",    "how",
                                         "who",  "which", "查询", "搜索",   "查找",
                                         "什么", "哪里",  "何时", "为什么", NULL};

static const char *g_command_keywords[] = {"create", "delete", "update", "modify", "set",
                                           "get",    "创建",   "删除",   "更新",   "修改",
                                           "设置",   "获取",   "执行",   "运行",   "启动",
                                           "停止",   "暂停",   "继续",   NULL};

static const char *g_confirm_keywords[] = {"yes", "ok",   "sure", "confirm", "agree", "accept",
                                           "是",  "好的", "确定", "同意",    "接受",  "确认",
                                           "对",  "正确", "没错", NULL};

static const char *g_negate_keywords[] = {"no",      "not",  "cancel", "deny", "reject",
                                          "decline", "不",   "取消",   "否认", "拒绝",
                                          "否定",    "错误", "不对",   "不行", NULL};

static const char *g_greet_keywords[] = {"hello",          "hi",     "hey",  "good morning",
                                         "good afternoon", "你好",   "您好", "早上好",
                                         "下午好",         "晚上好", NULL};

static const char *g_farewell_keywords[] = {"bye",  "goodbye", "see you", "farewell",
                                            "再见", "拜拜",    "回头见",  NULL};

static const char *intent_type_name(airy_intent_type_t type)
{
    switch (type) {
    case AIRY_INTENT_UNKNOWN:
        return "unknown";
    case AIRY_INTENT_QUERY:
        return "query";
    case AIRY_INTENT_COMMAND:
        return "command";
    case AIRY_INTENT_EXPLANATION:
        return "explanation";
    case AIRY_INTENT_CREATION:
        return "creation";
    case AIRY_INTENT_MODIFICATION:
        return "modification";
    case AIRY_INTENT_DELETION:
        return "deletion";
    case AIRY_INTENT_CONFIRMATION:
        return "confirmation";
    case AIRY_INTENT_NEGATION:
        return "negation";
    case AIRY_INTENT_GREETING:
        return "greeting";
    case AIRY_INTENT_FAREWELL:
        return "farewell";
    default:
        return "unknown";
    }
}

static airy_intent_type_t check_greeting(const char *lower_input, float *score)
{
    for (int i = 0; g_greet_keywords[i]; i++) {
        if (airy_text_icontains(lower_input, g_greet_keywords[i])) {
            *score = 0.95f;
            return AIRY_INTENT_GREETING;
        }
    }
    return AIRY_INTENT_UNKNOWN;
}

static airy_intent_type_t check_farewell(const char *lower_input, float *score)
{
    for (int i = 0; g_farewell_keywords[i]; i++) {
        if (airy_text_icontains(lower_input, g_farewell_keywords[i])) {
            *score = 0.95f;
            return AIRY_INTENT_FAREWELL;
        }
    }
    return AIRY_INTENT_UNKNOWN;
}

static airy_intent_type_t check_query(const char *lower_input, float *score)
{
    int query_count = 0;
    for (int i = 0; g_query_keywords[i]; i++) {
        if (airy_text_icontains(lower_input, g_query_keywords[i])) {
            query_count++;
        }
    }
    if (query_count > 0) {
        *score = 0.8f + (query_count > 2 ? 0.15f : 0.05f);
        return AIRY_INTENT_QUERY;
    }
    return AIRY_INTENT_UNKNOWN;
}

static airy_intent_type_t check_command(const char *lower_input, float *score)
{
    int cmd_count = 0;
    for (int i = 0; g_command_keywords[i]; i++) {
        if (airy_text_icontains(lower_input, g_command_keywords[i])) {
            cmd_count++;
        }
    }
    if (cmd_count > 0) {
        *score = 0.85f + (cmd_count > 2 ? 0.10f : 0.05f);
        airy_intent_type_t base_type = AIRY_INTENT_COMMAND;

        if (airy_text_icontains(lower_input, "create") ||
            airy_text_icontains(lower_input, "创建")) {
            base_type = AIRY_INTENT_CREATION;
        } else if (airy_text_icontains(lower_input, "delete") ||
                   airy_text_icontains(lower_input, "删除")) {
            base_type = AIRY_INTENT_DELETION;
        } else if (airy_text_icontains(lower_input, "modify") ||
                   airy_text_icontains(lower_input, "修改")) {
            base_type = AIRY_INTENT_MODIFICATION;
        }
        return base_type;
    }
    return AIRY_INTENT_UNKNOWN;
}

static airy_intent_type_t check_confirmation_negation(const char *lower_input, float *score)
{
    for (int i = 0; g_confirm_keywords[i]; i++) {
        if (airy_text_icontains(lower_input, g_confirm_keywords[i])) {
            *score = 0.90f;
            return AIRY_INTENT_CONFIRMATION;
        }
    }
    for (int i = 0; g_negate_keywords[i]; i++) {
        if (airy_text_icontains(lower_input, g_negate_keywords[i])) {
            *score = 0.90f;
            return AIRY_INTENT_NEGATION;
        }
    }
    return AIRY_INTENT_UNKNOWN;
}

int airy_intent_classify(const char *input, size_t input_len, airy_intent_classification_t *result)
{
    if (!input || !result || input_len == 0) {
        return AIRY_EINVAL;
    }

    char *lower_input = (char *)AIRY_MALLOC(input_len + 1);
    if (!lower_input) {
        return AIRY_EINVAL;
    }

    __builtin_memcpy(lower_input, input, input_len);
    lower_input[input_len] = '\0';
    airy_text_lower(lower_input);

    __builtin_memset(result, 0, sizeof(*result));
    result->type = AIRY_INTENT_UNKNOWN;
    result->confidence = 0.0f;

    float max_score = 0.0f;
    airy_intent_type_t detected_type = AIRY_INTENT_UNKNOWN;

    typedef airy_intent_type_t (*intent_checker_t)(const char *, float *);
    static const intent_checker_t checkers[] = {check_greeting,
                                                check_farewell,
                                                check_query,
                                                check_command,
                                                check_confirmation_negation,
                                                NULL};

    for (int i = 0; checkers[i]; i++) {
        float current_score = 0.0f;
        airy_intent_type_t current_type = checkers[i](lower_input, &current_score);

        if (current_type != AIRY_INTENT_UNKNOWN) {
            detected_type = current_type;
            max_score = current_score;
            break;
        }
    }

    result->type = detected_type;
    result->confidence = max_score > 0 ? max_score : 0.3f;
    result->type_name = intent_type_name(detected_type);

    AIRY_FREE(lower_input);
    return 0;
}
