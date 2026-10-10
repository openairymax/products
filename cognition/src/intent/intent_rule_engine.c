// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file intent_rule_engine.c
 * @brief Intent parser: rule management and rule-based intent matching.
 */

#include "intent_parser_internal.h"

intent_rule_t *intent_create_rule(const char *pattern, const char *intent_name,
                                  float confidence, uint32_t flags)
{
    if (!pattern || !intent_name)
        return NULL;

    intent_rule_t *rule = (intent_rule_t *)AIRY_CALLOC(1, sizeof(intent_rule_t));
    if (!rule) {
        AIRY_LOG_ERROR("Failed to allocate intent rule");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    rule->pattern = AIRY_STRDUP(pattern);
    rule->pattern_len = strlen(pattern);
    rule->intent_name = AIRY_STRDUP(intent_name);
    rule->confidence = confidence;
    rule->flags = flags;
    rule->next = NULL;

    if (!rule->pattern || !rule->intent_name) {
        if (rule->pattern)
            AIRY_FREE(rule->pattern);
        if (rule->intent_name)
            AIRY_FREE(rule->intent_name);
        AIRY_FREE(rule);
        AIRY_LOG_ERROR("Failed to duplicate strings for intent rule");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_text_lower(rule->pattern);

    return rule;
}

void intent_free_rule(intent_rule_t *rule)
{
    if (!rule)
        return;
    if (rule->pattern)
        AIRY_FREE(rule->pattern);
    if (rule->intent_name)
        AIRY_FREE(rule->intent_name);
    AIRY_FREE(rule);
}

void intent_free_rule_list(intent_rule_t *head)
{
    while (head) {
        intent_rule_t *next = head->next;
        intent_free_rule(head);
        head = next;
    }
}

airy_err_t intent_add_rule_to_parser(airy_intent_parser_t *parser,
                                     intent_rule_t *rule)
{
    if (!parser || !rule)
        AIRY_RET_ERR(AIRY_EINVAL);

    airy_mtx_lock(parser->lock);

    rule->next = parser->rule_list;
    parser->rule_list = rule;

    airy_mtx_unlock(parser->lock);

    AIRY_LOG_DEBUG("Added intent rule: %s -> %s", rule->pattern, rule->intent_name);
    return AIRY_SUCCESS;
}

airy_err_t intent_match_by_rules(airy_intent_parser_t *parser, const char *text,
                                 char **out_intent_name, float *out_confidence)
{
    if (!parser || !text || !out_intent_name || !out_confidence) {
        AIRY_RET_ERR(AIRY_EINVAL);
    }

    char *lower_text = AIRY_STRDUP(text);
    if (!lower_text)
        AIRY_RET_ERR(AIRY_ENOMEM);
    airy_text_lower(lower_text);

    airy_mtx_lock(parser->lock);
    intent_rule_t *rule = parser->rule_list;

    float best_confidence = 0.0f;
    char *best_intent = NULL;

    while (rule) {
        if (airy_text_icontains(lower_text, rule->pattern)) {
            if (rule->confidence > best_confidence) {
                best_confidence = rule->confidence;
                if (best_intent)
                    AIRY_FREE(best_intent);
                best_intent = AIRY_STRDUP(rule->intent_name);
                if (!best_intent) {
                    AIRY_FREE(lower_text);
                    airy_mtx_unlock(parser->lock);
                    AIRY_RET_ERR(AIRY_ENOMEM);
                }
            }
        } else {
            float similarity = airy_text_similar(lower_text, rule->pattern);
            float adjusted_confidence = similarity * rule->confidence;
            if (adjusted_confidence > best_confidence &&
                adjusted_confidence > MIN_CONFIDENCE_THRESHOLD) {
                best_confidence = adjusted_confidence;
                if (best_intent)
                    AIRY_FREE(best_intent);
                best_intent = AIRY_STRDUP(rule->intent_name);
                if (!best_intent) {
                    AIRY_FREE(lower_text);
                    airy_mtx_unlock(parser->lock);
                    AIRY_RET_ERR(AIRY_ENOMEM);
                }
            }
        }

        rule = rule->next;
    }

    airy_mtx_unlock(parser->lock);
    AIRY_FREE(lower_text);

    if (best_intent && best_confidence > MIN_CONFIDENCE_THRESHOLD) {
        *out_intent_name = best_intent;
        *out_confidence = best_confidence;
        return AIRY_SUCCESS;
    }

    if (best_intent)
        AIRY_FREE(best_intent);
    AIRY_RET_ERR(AIRY_ENOENT);
}
