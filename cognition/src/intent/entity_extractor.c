// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file entity_extractor.c
 * @brief Entity extractor implementation.
 */

#include "entity_extractor.h"
#include "error.h"

#include "atomic_compat.h"
#include "airy_memory.h"
#include "types.h"

#include <ctype.h>
#ifndef _WIN32
#include <regex.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int g_extractor_initialized = 0;

const char *airy_entity_type_name(airy_entity_type_t type)
{
    switch (type) {
    case AIRY_ENTITY_UNKNOWN:
        return "unknown";
    case AIRY_ENTITY_PERSON:
        return "person";
    case AIRY_ENTITY_ORGANIZATION:
        return "organization";
    case AIRY_ENTITY_LOCATION:
        return "location";
    case AIRY_ENTITY_TIME:
        return "time";
    case AIRY_ENTITY_DATE:
        return "date";
    case AIRY_ENTITY_NUMBER:
        return "number";
    case AIRY_ENTITY_URL:
        return "url";
    case AIRY_ENTITY_EMAIL:
        return "email";
    case AIRY_ENTITY_FILEPATH:
        return "filepath";
    case AIRY_ENTITY_COMMAND:
        return "command";
    case AIRY_ENTITY_PARAMETER:
        return "parameter";
    default:
        return "unknown";
    }
}

int airy_entity_extractor_init(void)
{
    int expected = 0;
    atomic_compare_exchange_strong_explicit(&g_extractor_initialized, &expected, 1,
                                            memory_order_seq_cst, memory_order_seq_cst);
    return 0;
}

void airy_entity_extractor_cleanup(void)
{
    atomic_store_explicit(&g_extractor_initialized, 0, memory_order_seq_cst);
}

airy_extraction_result_t *airy_extraction_result_create(size_t initial_capacity)
{
    if (initial_capacity == 0) {
        initial_capacity = 10;
    }

    airy_extraction_result_t *result =
        (airy_extraction_result_t *)AIRY_CALLOC(1, sizeof(airy_extraction_result_t));
    if (!result) {
        AIRY_ERROR_NULL(AIRY_EUNKNOWN, "validation failed");
    }

    result->entities = (airy_entity_t *)AIRY_CALLOC(initial_capacity, sizeof(airy_entity_t));
    if (!result->entities) {
        AIRY_FREE(result);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    result->entity_count = 0;
    result->capacity = initial_capacity;

    return result;
}

void airy_extraction_result_destroy(airy_extraction_result_t *result)
{
    if (!result) {
        return;
    }

    if (result->entities) {
        for (size_t i = 0; i < result->entity_count; i++) {
            if (result->entities[i].value) {
                AIRY_FREE(result->entities[i].value);
            }
        }
        AIRY_FREE(result->entities);
    }

    AIRY_FREE(result);
}

int airy_extraction_result_add(airy_extraction_result_t *result, const airy_entity_t *entity)
{
    if (!result || !entity) {
        AIRY_RET_ERR(AIRY_EINVAL);
    }

    if (result->entity_count >= result->capacity) {
        size_t new_capacity = result->capacity * 2;
        airy_entity_t *new_entities =
            (airy_entity_t *)AIRY_REALLOC(result->entities, new_capacity * sizeof(airy_entity_t));
        if (!new_entities) {
            AIRY_RET_ERR(AIRY_EINVAL);
        }

        __builtin_memset(new_entities + result->capacity, 0,
                         (new_capacity - result->capacity) * sizeof(airy_entity_t));

        result->entities = new_entities;
        result->capacity = new_capacity;
    }

    airy_entity_t *target = &result->entities[result->entity_count];
    target->type = entity->type;
    target->type_name = entity->type_name;
    target->start_pos = entity->start_pos;
    target->end_pos = entity->end_pos;
    target->confidence = entity->confidence;

    if (entity->value && entity->value_len > 0) {
        target->value = (char *)AIRY_MALLOC(entity->value_len + 1);
        if (target->value) {
            __builtin_memcpy(target->value, entity->value, entity->value_len);
            target->value[entity->value_len] = '\0';
            target->value_len = entity->value_len;
        } else {
            target->value = NULL;
            target->value_len = 0;
        }
    } else {
        target->value = NULL;
        target->value_len = 0;
    }

    result->entity_count++;
    return 0;
}

static void extract_numbers(const char *input, size_t input_len, airy_extraction_result_t *result)
{
    const char *p = input;
    size_t pos = 0;

    while (*p && pos < input_len) {
        if (isdigit(*p)) {
            const char *start = p;
            int start_pos = pos;

            while (*p && (isdigit(*p) || *p == '.')) {
                p++;
                pos++;
            }

            size_t len = p - start;
            if (len > 0 && len <= 20) {
                airy_entity_t entity;
                __builtin_memset(&entity, 0, sizeof(entity));
                entity.type = AIRY_ENTITY_NUMBER;
                entity.type_name = airy_entity_type_name(AIRY_ENTITY_NUMBER);
                entity.value = (char *)AIRY_MALLOC(len + 1);
                if (entity.value) {
                    __builtin_memcpy(entity.value, start, len);
                    entity.value[len] = '\0';
                    entity.value_len = len;
                    entity.start_pos = start_pos;
                    entity.end_pos = pos - 1;
                    entity.confidence = 0.95f;

                    airy_extraction_result_add(result, &entity);
                    AIRY_FREE(entity.value);
                }
            }

            continue;
        }

        p++;
        pos++;
    }
}

#ifndef _WIN32
static void extract_urls(const char *input, size_t input_len, airy_extraction_result_t *result)
{
    regex_t regex;
    regmatch_t match;
    const char *pattern = "(https?://[^\\s]+|ftp://[^\\s]+)";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return;
    }

    const char *p = input;
    int offset = 0;

    while (regexec(&regex, p, 1, &match, 0) == 0) {
        size_t len = match.rm_eo - match.rm_so;
        if (len > 3 && len <= 256) {
            airy_entity_t entity;
            __builtin_memset(&entity, 0, sizeof(entity));
            entity.type = AIRY_ENTITY_URL;
            entity.type_name = airy_entity_type_name(AIRY_ENTITY_URL);
            entity.value = (char *)AIRY_MALLOC(len + 1);
            if (entity.value) {
                __builtin_memcpy(entity.value, p + match.rm_so, len);
                entity.value[len] = '\0';
                entity.value_len = len;
                entity.start_pos = offset + match.rm_so;
                entity.end_pos = offset + match.rm_eo - 1;
                entity.confidence = 0.98f;

                airy_extraction_result_add(result, &entity);
                AIRY_FREE(entity.value);
            }
        }

        p += match.rm_eo;
        offset += match.rm_eo;
    }

    regfree(&regex);
}

static void extract_emails(const char *input, size_t input_len, airy_extraction_result_t *result)
{
    regex_t regex;
    regmatch_t match;
    const char *pattern = "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return;
    }

    const char *p = input;
    int offset = 0;

    while (regexec(&regex, p, 1, &match, 0) == 0) {
        size_t len = match.rm_eo - match.rm_so;
        if (len > 5 && len <= 128) {
            airy_entity_t entity;
            __builtin_memset(&entity, 0, sizeof(entity));
            entity.type = AIRY_ENTITY_EMAIL;
            entity.type_name = airy_entity_type_name(AIRY_ENTITY_EMAIL);
            entity.value = (char *)AIRY_MALLOC(len + 1);
            if (entity.value) {
                __builtin_memcpy(entity.value, p + match.rm_so, len);
                entity.value[len] = '\0';
                entity.value_len = len;
                entity.start_pos = offset + match.rm_so;
                entity.end_pos = offset + match.rm_eo - 1;
                entity.confidence = 0.97f;

                airy_extraction_result_add(result, &entity);
                AIRY_FREE(entity.value);
            }
        }

        p += match.rm_eo;
        offset += match.rm_eo;
    }

    regfree(&regex);
}

static void extract_filepaths(const char *input, size_t input_len, airy_extraction_result_t *result)
{
    regex_t regex;
    regmatch_t match;
    const char *pattern = "(/[a-zA-Z0-9_./-]+|[A-Za-z]:\\\\[a-zA-Z0-9_./\\\\-]+)";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return;
    }

    const char *p = input;
    int offset = 0;

    while (regexec(&regex, p, 1, &match, 0) == 0) {
        size_t len = match.rm_eo - match.rm_so;
        if (len > 2 && len <= 512) {
            airy_entity_t entity;
            __builtin_memset(&entity, 0, sizeof(entity));
            entity.type = AIRY_ENTITY_FILEPATH;
            entity.type_name = airy_entity_type_name(AIRY_ENTITY_FILEPATH);
            entity.value = (char *)AIRY_MALLOC(len + 1);
            if (entity.value) {
                __builtin_memcpy(entity.value, p + match.rm_so, len);
                entity.value[len] = '\0';
                entity.value_len = len;
                entity.start_pos = offset + match.rm_so;
                entity.end_pos = offset + match.rm_eo - 1;
                entity.confidence = 0.92f;

                airy_extraction_result_add(result, &entity);
                AIRY_FREE(entity.value);
            }
        }

        p += match.rm_eo;
        offset += match.rm_eo;
    }

    regfree(&regex);
}
#else
static void extract_urls(const char *input, size_t input_len, airy_extraction_result_t *result)
{
    (void)input;
    (void)input_len;
    (void)result;
}
static void extract_emails(const char *input, size_t input_len, airy_extraction_result_t *result)
{
    (void)input;
    (void)input_len;
    (void)result;
}
static void extract_filepaths(const char *input, size_t input_len, airy_extraction_result_t *result)
{
    (void)input;
    (void)input_len;
    (void)result;
}
#endif

int airy_entity_extract(const char *input, size_t input_len, airy_extraction_result_t *result)
{
    if (!input || !result || input_len == 0) {
        AIRY_RET_ERR(AIRY_EINVAL);
    }

    if (!atomic_load_explicit(&g_extractor_initialized, memory_order_acquire)) {
        airy_entity_extractor_init();
    }

    if (!result->entities) {
        airy_extraction_result_t *new_result = airy_extraction_result_create(10);
        if (!new_result) {
            AIRY_RET_ERR(AIRY_EINVAL);
        }
        __builtin_memcpy(result, new_result, sizeof(*result));
        AIRY_FREE(new_result);
    }

    extract_numbers(input, input_len, result);
    extract_urls(input, input_len, result);
    extract_emails(input, input_len, result);
    extract_filepaths(input, input_len, result);

    return 0;
}
