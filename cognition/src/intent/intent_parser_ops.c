// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file intent_parser_ops.c
 * @brief Intent parser: observability operations (statistics, reset,
 *        health check).
 */

#include "intent_parser_internal.h"

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

airy_err_t airy_intent_parser_stats(airy_intent_parser_t *parser, char **out_stats)
{
    if (!parser || !out_stats)
        AIRY_RET_ERR(AIRY_EINVAL);

    cJSON *stats_json = cJSON_CreateObject();
    if (!stats_json)
        AIRY_RET_ERR(AIRY_ENOMEM);

    airy_mtx_lock(parser->lock);

    cJSON_AddStringToObject(stats_json, "parser_id", parser->parser_id);
    cJSON_AddNumberToObject(stats_json, "total_parsed", parser->total_parsed);
    cJSON_AddNumberToObject(stats_json, "success_count", parser->success_count);
    cJSON_AddNumberToObject(stats_json, "failure_count", parser->failure_count);
    cJSON_AddNumberToObject(stats_json, "total_time_ns", parser->total_time_ns);

    double avg_time_ns =
        parser->total_parsed > 0 ? (double)parser->total_time_ns / parser->total_parsed : 0.0;
    cJSON_AddNumberToObject(stats_json, "avg_time_ns", avg_time_ns);

    double success_rate = parser->total_parsed > 0 ?
                              (double)parser->success_count / parser->total_parsed * 100.0 :
                              0.0;
    cJSON_AddNumberToObject(stats_json, "success_rate_percent", success_rate);

    int rule_count = 0;
    intent_rule_t *rule = parser->rule_list;
    while (rule) {
        rule_count++;
        rule = rule->next;
    }
    cJSON_AddNumberToObject(stats_json, "rule_count", rule_count);

    airy_mtx_unlock(parser->lock);

    char *stats_str = cJSON_PrintUnformatted(stats_json);
    cJSON_Delete(stats_json);

    if (!stats_str)
        AIRY_RET_ERR(AIRY_ENOMEM);

    *out_stats = stats_str;
    return AIRY_SUCCESS;
}

void airy_intent_parser_reset_stats(airy_intent_parser_t *parser)
{
    if (!parser)
        return;

    airy_mtx_lock(parser->lock);

    parser->total_parsed = 0;
    parser->success_count = 0;
    parser->failure_count = 0;
    parser->total_time_ns = 0;

    airy_mtx_unlock(parser->lock);

    AIRY_LOG_INFO("Intent parser stats reset: %s", parser->parser_id);
}

airy_err_t airy_intent_parser_health_check(airy_intent_parser_t *parser, char **out_json)
{
    if (!parser || !out_json)
        AIRY_RET_ERR(AIRY_EINVAL);

    cJSON *health_json = cJSON_CreateObject();
    if (!health_json)
        AIRY_RET_ERR(AIRY_ENOMEM);

    airy_mtx_lock(parser->lock);

    cJSON_AddStringToObject(health_json, "component", "intent_parser");
    cJSON_AddStringToObject(health_json, "parser_id", parser->parser_id);
    cJSON_AddStringToObject(health_json, "status", "healthy");
    size_t rule_count = 0;
    intent_rule_t *r = parser->rule_list;
    while (r) {
        rule_count++;
        r = r->next;
    }
    cJSON_AddNumberToObject(health_json, "rule_count", (double)rule_count);

    int resources_ok = 1;
    if (!parser->lock)
        resources_ok = 0;

    cJSON_AddBoolToObject(health_json, "resources_ok", resources_ok);
    cJSON_AddNumberToObject(health_json, "timestamp_ns", airy_time_monotonic_ns());

    airy_mtx_unlock(parser->lock);

    char *health_str = cJSON_PrintUnformatted(health_json);
    cJSON_Delete(health_json);

    if (!health_str)
        AIRY_RET_ERR(AIRY_ENOMEM);

    *out_json = health_str;
    return AIRY_SUCCESS;
}
