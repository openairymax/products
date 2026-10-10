/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file entity_extractor.h
 * @brief Entity extractor interface.
 */

#ifndef AIRY_RT_ENTITY_EXTRACTOR_H
#define AIRY_RT_ENTITY_EXTRACTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Entity type enum.
 */
typedef enum airy_entity_type {
    AIRY_ENTITY_UNKNOWN = 0,
    AIRY_ENTITY_PERSON = 1,
    AIRY_ENTITY_ORGANIZATION = 2,
    AIRY_ENTITY_LOCATION = 3,
    AIRY_ENTITY_TIME = 4,
    AIRY_ENTITY_DATE = 5,
    AIRY_ENTITY_NUMBER = 6,
    AIRY_ENTITY_URL = 7, /* URL */
    AIRY_ENTITY_EMAIL = 8,
    AIRY_ENTITY_FILEPATH = 9,
    AIRY_ENTITY_COMMAND = 10,
    AIRY_ENTITY_PARAMETER = 11,
    AIRY_ENTITY_MAX
} airy_entity_type_t;

/**
 * @brief Entity structure.
 */
typedef struct airy_entity {
    airy_entity_type_t type;
    const char *type_name;
    char *value;
    size_t value_len;
    int start_pos;
    int end_pos;
    float confidence;
} airy_entity_t;

/**
 * @brief Entity-extraction result.
 */
typedef struct airy_extraction_result {
    airy_entity_t *entities;
    size_t entity_count;
    size_t capacity;
} airy_extraction_result_t;

/**
 * @brief Initialize the entity extractor.
 * @return 0 on success, error code on failure
 */
int airy_entity_extractor_init(void);

/**
 * @brief Clean up the entity extractor.
 */
void airy_entity_extractor_cleanup(void);

/**
 * @brief Extract entities from text.
 * @param input Input text
 * @param input_len Input length
 * @param result Output result
 * @return 0 on success, error code on failure
 */
int airy_entity_extract(const char *input, size_t input_len, airy_extraction_result_t *result);

/**
 * @brief Create an empty extraction result.
 * @param initial_capacity Initial capacity
 * @return Result pointer, or NULL on failure
 */
airy_extraction_result_t *airy_extraction_result_create(size_t initial_capacity);

/**
 * @brief Destroy an extraction result.
 * @param result Result pointer
 */
void airy_extraction_result_destroy(airy_extraction_result_t *result);

/**
 * @brief Add an entity to the result.
 * @param result Result pointer
 * @param entity Entity data
 * @return 0 on success, error code on failure
 */
int airy_extraction_result_add(airy_extraction_result_t *result, const airy_entity_t *entity);

/**
 * @brief Get the entity type name.
 * @param type Entity type
 * @return Type-name string
 */
const char *airy_entity_type_name(airy_entity_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_ENTITY_EXTRACTOR_H */
