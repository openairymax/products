// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file intent_parser_internal.h
 * @brief Intent-parser internal shared declarations (internal contract
 *        after domain split; not a public API).
 *
 * intent_parser.c is split into multiple translation units by responsibility,
 * keeping the public API (airy_intent_parser_* / airy_intent_* in
 * include/cognition.h) and ABI completely unchanged:
 *   - intent_parser.c            core domain: lifecycle create/destroy +
 *                                parse + free + add-rule orchestration
 *   - intent_rule_engine.c      rule-engine domain: rule CRUD + rule-based
 *                                intent matching
 *   - intent_entity_extractor.c entity-extraction domain: keyword-based
 *                                entity recognition
 *   - intent_parser_ops.c       observability domain: stats / reset /
 *                                health-check
 *
 * The shared intent-parser struct (struct airy_intent_parser, authoritative
 * definition below) and cross-domain internal helpers are declared here.
 * This header is for internal use by the airy_cognition library only and
 * is not installed with the public include set.
 */

#ifndef AIRY_RT_INTENT_PARSER_INTERNAL_H
#define AIRY_RT_INTENT_PARSER_INTERNAL_H

#include "airy_rt.h"
#include "cognition.h"
#include "error_utils.h"
#include "logging.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INTENT_CLASSES 64
#define MAX_ENTITIES 32
#define MAX_KEYWORDS 128
#define DEFAULT_INTENT_TIMEOUT_MS 500
#define MIN_CONFIDENCE_THRESHOLD 0.3
#define HIGH_CONFIDENCE_THRESHOLD 0.8

/**
 * @brief Intent classification rule structure.
 */
typedef struct intent_rule {
    char *pattern;
    size_t pattern_len;
    char *intent_name;
    float confidence;
    uint32_t flags;
    struct intent_rule *next;
} intent_rule_t;

/**
 * @brief Entity extraction result.
 */
typedef struct extracted_entity {
    char *type;
    char *value;
    size_t value_len;
    int start_pos;
    int end_pos;
    float confidence;
} extracted_entity_t;

/**
 * @brief Intent parser internal state.
 */
struct airy_intent_parser {
    intent_rule_t *rule_list;
    airy_mtx_t *lock;
    uint64_t total_parsed;
    uint64_t success_count;
    uint64_t failure_count;
    uint64_t total_time_ns;
    void *obs;
    char *parser_id;
    uint8_t *keyword_trie;
    size_t keyword_count;
};

/* 文本工具函数（权威声明：commons/utils/string/text_utils.h；原 intent_utils.c
 * 于 0.1.19 §263 上移至字符串 SSoT，此处转发给域内消费者） */
#include "text_utils.h"

/* intent_rule_engine.c */
intent_rule_t *intent_create_rule(const char *pattern, const char *intent_name,
                                  float confidence, uint32_t flags);
void intent_free_rule(intent_rule_t *rule);
void intent_free_rule_list(intent_rule_t *head);
airy_err_t intent_add_rule_to_parser(airy_intent_parser_t *parser,
                                     intent_rule_t *rule);
airy_err_t intent_match_by_rules(airy_intent_parser_t *parser, const char *text,
                                 char **out_intent_name, float *out_confidence);

/* intent_entity_extractor.c */
size_t intent_extract_entities(const char *text, extracted_entity_t *entities,
                               size_t max_entries);

#endif /* AIRY_RT_INTENT_PARSER_INTERNAL_H */
