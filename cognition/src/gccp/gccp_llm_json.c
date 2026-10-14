// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gccp_llm_json.c
 * @brief GCCP 策略 LLM 调用 + LLM 输出 JSON 解析域（M5-4 迁出）。
 *
 * gccp_llm_call()（经机制核注入的补全闭包发起 LLM 请求）与
 * gccp_json_field()/gccp_apply_json()/gccp_find_json()（LLM 目标 JSON
 * 提取/应用，仅 AIRY_HAS_CJSON 下可用）。共享声明见 gccp_internal.h。
 *
 * 闭包契约：补全请求的构造（adapter 优先、service 次之）是机制核的
 * 职责（引擎侧 trampoline 封装）；策略侧只看到 airy_gccp_complete_fn。
 * 响应文本复制后经 ops 注入表（are_ops_get_llm）释放原响应，策略库
 * 不直连机制核释放符号。
 */

#include "gccp.h"
#include "gccp_internal.h"
#include "airy_llm_ops.h"
#include "logging.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

/**
 * @brief Call the LLM via the injected completion closure and return the
 *        response text (OWNER, caller AIRY_FREE).
 *
 * Returns NULL when the closure is absent or the completion fails.
 */
char *gccp_llm_call(airy_gccp_complete_fn complete, void *complete_ctx, const char *model,
                    const char *system_prompt, const char *user_input)
{
    if (!complete || !system_prompt || !user_input)
        return NULL;

    llm_message_t msgs[2];
    AIRY_MEMSET(&msgs, 0, sizeof(msgs));
    msgs[0].role = "system";
    msgs[0].content = system_prompt;
    msgs[1].role = "user";
    msgs[1].content = user_input;

    llm_request_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    /* GCCP 意图确认使用 t1-p（PROF）模型槽——NULL 走 provider 默认模型。 */
    cfg.model = model;
    cfg.messages = msgs;
    cfg.message_count = 2;
    cfg.temperature = 0.2f;
    cfg.top_p = 1.0f;
    cfg.max_tokens = 1024;
    cfg.stream = 0;

    llm_response_t *resp = NULL;
    int ret = complete(complete_ctx, &cfg, &resp);
    const airy_llm_ops_t *llm_ops = are_ops_get_llm();

    if (ret != 0 || !resp || !resp->choices || resp->choice_count < 1 ||
        !resp->choices[0].content) {
        if (resp && llm_ops && llm_ops->response_free)
            llm_ops->response_free(resp);
        AIRY_LOG_WARN("GCCP: LLM call failed (ret=%d)", ret);
        return NULL;
    }
    char *text = AIRY_STRDUP(resp->choices[0].content);
    if (llm_ops && llm_ops->response_free)
        llm_ops->response_free(resp);
    return text;
}

#ifdef AIRY_HAS_CJSON

/**
 * @brief Extract a string field from a cJSON object (copy, BORROW→OWNER).
 */
char *gccp_json_field(cJSON *obj, const char *key)
{
    if (!obj || !key)
        return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring && item->valuestring[0])
        return AIRY_STRDUP(item->valuestring);
    return NULL;
}

/* 若 JSON 具该字符串字段，释放旧值并接管新值；缺失/非字符串保持原值。 */
static void set_str_field(cJSON *obj, const char *key, char **dst)
{
    char *v = gccp_json_field(obj, key);
    if (v) {
        AIRY_FREE(*dst);
        *dst = v;
    }
}

/**
 * @brief Parse the LLM's goal JSON into the goal structure.
 */
void gccp_apply_json(airy_gccp_goal_t *goal, cJSON *obj)
{
    set_str_field(obj, "endpoint", &goal->goal_endpoint);
    set_str_field(obj, "start", &goal->goal_start);
    set_str_field(obj, "bottleneck", &goal->goal_bottleneck);
    set_str_field(obj, "audience", &goal->goal_audience);
    set_str_field(obj, "verify", &goal->goal_verify);
    cJSON *conf = cJSON_GetObjectItemCaseSensitive(obj, "confidence");
    if (conf && cJSON_IsNumber(conf)) {
        goal->confidence = (float)conf->valuedouble;
    } else if (conf && cJSON_IsString(conf) && conf->valuestring) {
        goal->confidence = (float)atof(conf->valuestring);
    } else if (conf) {
        /* 非法类型（对象/数组/布尔）→ 不信任，置 0 */
        goal->confidence = 0.0f;
    } else {
        /* q8a：LLM 未返回 confidence 字段（常见于简略输出）不得让目标
         * 落入 AMBIGUOUS——用户已完成 4+1 逐问澄清，缺字段只是 LLM 省略
         * 自我评估。按确认阈值处理（视为可确认）并记录 WARN 可观测。
         * 此前缺省 0.0 → AMBIGUOUS → intent_flags|=0x10 → planner 把已
         * 收敛任务收缩为 2 节点（确认形同虚设）。 */
        goal->confidence = AIRY_GCCP_CONFIDENCE_THRESHOLD;
        AIRY_LOG_WARN("GCCP: LLM goal JSON omitted confidence, defaulting to threshold "
                      "(%.2f) to keep CONFIRMED",
                      (double)AIRY_GCCP_CONFIDENCE_THRESHOLD);
    }
}

/**
 * @brief Locate the JSON substring in LLM output (strip ```json fences).
 */
char *gccp_find_json(const char *text)
{
    if (!text)
        return NULL;
    const char *start = text;
    const char *open = strstr(text, "{");
    if (open)
        start = open;
    const char *end = NULL;

    size_t len = strlen(start);
    for (size_t i = len; i > 0; i--) {
        if (start[i - 1] == '}') {
            end = &start[i - 1];
            break;
        }
    }
    if (!end)
        return NULL;
    size_t json_len = (size_t)(end - start + 1);
    char *json = (char *)AIRY_MALLOC(json_len + 1);
    if (!json)
        return NULL;
    AIRY_MEMCPY(json, start, json_len);
    json[json_len] = '\0';
    return json;
}

#endif /* AIRY_HAS_CJSON */
