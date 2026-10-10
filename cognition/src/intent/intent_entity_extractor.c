// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file intent_entity_extractor.c
 * @brief Intent parser: keyword-based entity extraction.
 */

#include "intent_parser_internal.h"

#include <ctype.h>

size_t intent_extract_entities(const char *text, extracted_entity_t *entities,
                               size_t max_entries)
{
    if (!text || !entities || max_entries == 0)
        return 0;

    size_t count = 0;
    char *keywords[MAX_KEYWORDS];
    size_t keyword_count = airy_text_keywords(text, keywords, MAX_KEYWORDS);

    for (size_t i = 0; i < keyword_count && count < max_entries; i++) {
        const char *keyword = keywords[i];
        char *type = NULL;
        float confidence = 0.5f;

        int is_number = 1;
        for (size_t j = 0; j < strlen(keyword); j++) {
            if (!isdigit(keyword[j])) {
                is_number = 0;
                break;
            }
        }

        if (is_number) {
            type = "number";
            confidence = 0.9f;
        } else if (airy_text_icontains(keyword, "time") ||
                   airy_text_icontains(keyword, "hour") ||
                   airy_text_icontains(keyword, "minute") ||
                   airy_text_icontains(keyword, "second") ||
                   airy_text_icontains(keyword, "day") ||
                   airy_text_icontains(keyword, "week") ||
                   airy_text_icontains(keyword, "month") ||
                   airy_text_icontains(keyword, "year")) {
            type = "time";
            confidence = 0.8f;
        } else if (airy_text_icontains(keyword, "file") ||
                   airy_text_icontains(keyword, "document") ||
                   airy_text_icontains(keyword, "report") ||
                   airy_text_icontains(keyword, "data")) {
            type = "file";
            confidence = 0.7f;
        }

        if (type) {
            entities[count].type = AIRY_STRDUP(type);
            entities[count].value = AIRY_STRDUP(keyword);
            if (!entities[count].type || !entities[count].value) {
                AIRY_FREE(entities[count].type);
                AIRY_FREE(entities[count].value);
                continue;
            }
            entities[count].value_len = strlen(keyword);
            entities[count].confidence = confidence;

            const char *pos = strstr(text, keyword);
            if (pos) {
                entities[count].start_pos = (size_t)(pos - text);
                entities[count].end_pos = entities[count].start_pos + strlen(keyword);
            } else {
                entities[count].start_pos = 0;
                entities[count].end_pos = 0;
            }
            count++;
        }
    }

    airy_text_kw_free(keywords, keyword_count);
    return count;
}
