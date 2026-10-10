// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file intent_parser.c
 * @brief Intent-understanding engine core domain.
 *
 * Parses natural-language user input to identify the real intent and
 * goal. Uses a hybrid of rule-based and machine-learning methods,
 * supporting multi-domain intent recognition with production-grade
 * reliability targeting 99.999% availability.
 *
 * Core domain (this file): lifecycle create/destroy + parse + free +
 * add-rule orchestration.  Other domains are split into:
 *   - intent_rule_engine.c      rule management + matching
 *   - intent_entity_extractor.c entity extraction
 *   - intent_parser_ops.c       stats / reset / health-check
 */

#include "airy_rt.h"
#include "cognition.h"
#include "error_utils.h"
#include "id_utils.h"
#include "logging.h"

#include "entity_extractor.h"

/* Unified base library compatibility layer */
#include "airy_memory.h"
#include "string_compat.h"

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

#include "intent_parser_internal.h"

/**
 * @brief Create an intent parser.
 * @param out_parser Output parser handle
 * @return airy_err_t
 */
airy_err_t airy_intent_parser_create(airy_intent_parser_t **out_parser)
{
    if (!out_parser)
        AIRY_RET_ERR(AIRY_EINVAL);

    airy_intent_parser_t *parser =
        (airy_intent_parser_t *)AIRY_CALLOC(1, sizeof(airy_intent_parser_t));
    if (!parser) {
        AIRY_LOG_ERROR("Failed to allocate intent parser");
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    parser->lock = airy_mtx_create();
    if (!parser->lock) {
        AIRY_LOG_ERROR("Failed to create mutex for intent parser");
        AIRY_FREE(parser);
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    char uuid_buf[64];
    if (airy_generate_uuid(uuid_buf) == AIRY_SUCCESS) {
        parser->parser_id = AIRY_STRDUP(uuid_buf);
    } else {
        parser->parser_id = NULL;
    }
    if (!parser->parser_id) {
        AIRY_LOG_WARN("Failed to generate UUID for intent parser, using default");
        parser->parser_id = AIRY_STRDUP("intent_parser_default");
    }

    intent_rule_t *rule;

    rule = intent_create_rule("read file", "file_read", 0.9f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("write file", "file_write", 0.9f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("create file", "file_create", 0.9f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("delete file", "file_delete", 0.9f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("analyze data", "data_analyze", 0.8f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("process data", "data_process", 0.8f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("summarize report", "report_summarize", 0.85f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("download", "network_download", 0.9f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("upload", "network_upload", 0.9f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("send email", "email_send", 0.95f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("help", "general_help", 0.95f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    rule = intent_create_rule("what can you do", "general_capabilities", 0.9f, 0);
    if (rule)
        intent_add_rule_to_parser(parser, rule);

    *out_parser = parser;

    AIRY_LOG_INFO("Intent parser created: %s", parser->parser_id);
    return AIRY_SUCCESS;
}

/**
 * @brief Destroy an intent parser.
 * @param parser Parser handle
 */
void airy_intent_parser_destroy(airy_intent_parser_t *parser)
{
    if (!parser)
        return;

    AIRY_LOG_DEBUG("Destroying intent parser: %s", parser->parser_id);

    intent_free_rule_list(parser->rule_list);

    if (parser->lock) {
        airy_mtx_free(parser->lock);
    }

    if (parser->parser_id) {
        AIRY_FREE(parser->parser_id);
    }

    AIRY_FREE(parser);
}

/**
 * @brief Parse user input and extract the intent.
 * @param parser Parser
 * @param input User input text
 * @param input_len Input length
 * @param out_intent Output intent structure
 * @return airy_err_t
 */
airy_err_t airy_intent_parser_parse(airy_intent_parser_t *parser, const char *input,
                                    size_t input_len, airy_intent_t **out_intent)
{
    if (!parser || !input || !out_intent) {
        AIRY_RET_ERR(AIRY_EINVAL);
    }

    uint64_t start_time_ns = (uint64_t)airy_time_monotonic_ms() * 1000000ULL;

    parser->total_parsed++;

    airy_intent_t *intent = (airy_intent_t *)AIRY_CALLOC(1, sizeof(airy_intent_t));
    if (!intent) {
        AIRY_LOG_ERROR("Failed to allocate intent structure");
        parser->failure_count++;
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    intent->intent_raw_text = (char *)AIRY_MALLOC(input_len + 1);
    if (!intent->intent_raw_text) {
        AIRY_LOG_ERROR("Failed to allocate raw text buffer");
        AIRY_FREE(intent);
        parser->failure_count++;
        AIRY_RET_ERR(AIRY_ENOMEM);
    }
    __builtin_memcpy(intent->intent_raw_text, input, input_len);
    intent->intent_raw_text[input_len] = '\0';
    intent->intent_raw_len = input_len;

    char *intent_name = NULL;
    float confidence = 0.0f;
    airy_err_t match_result =
        intent_match_by_rules(parser, input, &intent_name, &confidence);

    if (match_result == AIRY_SUCCESS && intent_name) {
        intent->intent_goal = intent_name;
        intent->intent_goal_len = strlen(intent_name);
        intent->intent_flags = 0;

        if (confidence > HIGH_CONFIDENCE_THRESHOLD) {
            intent->intent_flags |= 0x01;
        }

        extracted_entity_t entities[MAX_ENTITIES];
        size_t entity_count =
            intent_extract_entities(input, entities, MAX_ENTITIES);

        if (entity_count > 0) {
            cJSON *context_json = cJSON_CreateObject();
            if (context_json) {
                cJSON *entities_array = cJSON_CreateArray();
                for (size_t i = 0; i < entity_count; i++) {
                    cJSON *entity_obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(entity_obj, "type", entities[i].type);
                    cJSON_AddStringToObject(entity_obj, "value", entities[i].value);
                    cJSON_AddNumberToObject(entity_obj, "confidence",
                                            entities[i].confidence);
                    cJSON_AddItemToArray(entities_array, entity_obj);

                    AIRY_FREE(entities[i].type);
                    AIRY_FREE(entities[i].value);
                }
                cJSON_AddItemToObject(context_json, "entities", entities_array);

                char *context_str = cJSON_PrintUnformatted(context_json);
                if (context_str) {
                    intent->intent_context = context_str;
                }
                cJSON_Delete(context_json);
            }
        }

        AIRY_LOG_DEBUG("Intent parsed successfully: %s (confidence: %.2f)",
                       intent_name, confidence);

        parser->success_count++;
    } else {
        intent->intent_goal = AIRY_STRDUP("unknown");
        if (!intent->intent_goal) {
            AIRY_FREE(intent);
            parser->failure_count++;
            AIRY_RET_ERR(AIRY_ENOMEM);
        }
        intent->intent_goal_len = 7;
        intent->intent_flags = 0x02;

        AIRY_LOG_WARN("No intent matched for input: %.*s", (int)input_len, input);

        parser->failure_count++;
    }

    uint64_t end_time_ns = (uint64_t)airy_time_monotonic_ms() * 1000000ULL;
    uint64_t duration_ns = end_time_ns - start_time_ns;
    parser->total_time_ns += duration_ns;

    *out_intent = intent;

    if (match_result == AIRY_SUCCESS) {
        char feedback_json[256];
        snprintf(feedback_json, sizeof(feedback_json),
                 "{\"intent\":\"%s\",\"confidence\":%.2f,\"duration_ns\":%llu}",
                 intent_name, confidence,
                 (unsigned long long)duration_ns);
    }

    return AIRY_SUCCESS;
}

/**
 * @brief Free an intent structure.
 * @param intent Intent structure
 */
void airy_intent_free(airy_intent_t *intent)
{
    if (!intent)
        return;

    if (intent->intent_raw_text)
        AIRY_FREE(intent->intent_raw_text);
    if (intent->intent_goal)
        AIRY_FREE(intent->intent_goal);
    if (intent->intent_context)
        AIRY_FREE(intent->intent_context);
    if (intent->intent_gccp_goal)
        airy_gccp_goal_free(intent->intent_gccp_goal);

    AIRY_FREE(intent);
}

/**
 * @brief Add a custom intent rule.
 * @param parser Parser
 * @param pattern Pattern string
 * @param intent_name Intent name
 * @param confidence Confidence
 * @param flags Flags
 * @return airy_err_t
 */
airy_err_t airy_intent_parser_add_rule(airy_intent_parser_t *parser, const char *pattern,
                                       const char *intent_name, float confidence,
                                       uint32_t flags)
{
    if (!parser || !pattern || !intent_name)
        AIRY_RET_ERR(AIRY_EINVAL);

    if (confidence < 0.0f || confidence > 1.0f) {
        AIRY_LOG_ERROR("Confidence must be between 0.0 and 1.0");
        AIRY_RET_ERR(AIRY_EINVAL);
    }

    intent_rule_t *rule =
        intent_create_rule(pattern, intent_name, confidence, flags);
    if (!rule) {
        AIRY_LOG_ERROR("Failed to create intent rule");
        AIRY_RET_ERR(AIRY_ENOMEM);
    }

    airy_err_t result = intent_add_rule_to_parser(parser, rule);
    if (result != AIRY_SUCCESS) {
        intent_free_rule(rule);
    }

    return result;
}
