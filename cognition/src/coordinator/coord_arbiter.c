// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file coord_arbiter.c
 * @brief External arbitration strategy (LLM arbiter model or human callback).
 *
 * P2.7: when arbiter_model is configured and base->llm is available,
 * build a prompt with all candidates and let the LLM pick the best one
 * instead of returning inputs[0]. Falls back to inputs[0] with a WARN
 * when the LLM is unavailable.
 *
 * The LLM call goes through the injected airy_llm_ops table so this policy
 * payload never links daemon symbols directly; when no implementation has
 * been injected it degrades to the deterministic inputs[0] fallback.
 */

#include "airy_rt.h"
#include "logging_compat.h"
#include "coord_internal.h"
#include "airy_llm_ops.h"

#include <stdlib.h>

/* Unified base library compatibility layer */
#include "airy_memory.h"
#include "string_compat.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Private data for external arbitration.
 */
typedef struct arbiter_data {
    char *arbiter_model;
    airy_mtx_t *lock;
    void (*human_callback)(const char *question, char *answer, size_t max_len);
} arbiter_data_t;

static void arbiter_destroy(airy_coordinator_base_t *base)
{
    if (!base)
        return;
    arbiter_data_t *data = (arbiter_data_t *)base->data;
    if (data) {
        if (data->arbiter_model)
            AIRY_FREE(data->arbiter_model);
        if (data->lock)
            airy_mtx_free(data->lock);
        AIRY_FREE(data);
    }
    AIRY_FREE(base);
}

/**
 * @brief P2.7: Extract the chosen index (1-based) from the LLM arbitration response.
 *
 * The LLM may reply with a bare number, "Choice: N", "index: N",
 * JSON {"choice":N}, etc. Returns -1 on parse failure.
 */
static int parse_arbiter_choice(const char *response, size_t input_count)
{
    if (!response)
        return -1;

    while (*response && isspace((unsigned char)*response))
        response++;

    for (const char *p = response; *p; p++) {
        if (isdigit((unsigned char)*p)) {
            int val = atoi(p);
            if (val >= 1 && (size_t)val <= input_count) {
                return val;
            }
        }
    }
    return -1;
}

/**
 * @brief P2.7: LLM arbitration core — build the prompt and call the LLM to pick the best candidate.
 *
 * @param svc LLM service handle (borrowed)
 * @param arbiter_model Arbiter model name (may be NULL; passed via prompt context)
 * @param inputs Candidate input array
 * @param input_count Number of candidates
 * @param out_choice [out] Chosen index (1-based; -1 on parse failure)
 * @return AIRY_SUCCESS or an error code
 */
static airy_err_t arbiter_llm_arbitrate(llm_service_t *svc, const char *arbiter_model,
                                        const char **inputs, size_t input_count, int *out_choice)
{
    if (!svc || !inputs || !out_choice)
        return AIRY_EINVAL;
    *out_choice = -1;

    const airy_llm_ops_t *llm_ops = are_ops_get_llm();
    if (!llm_ops || !llm_ops->service_complete) {
        AIRY_LOG_WARN("arbiter: LLM ops unavailable, falling back to inputs[0]");
        return AIRY_ENOTSUP;
    }

    size_t prompt_sz = 512 + input_count * (256 + 32);
    char *prompt = (char *)AIRY_MALLOC(prompt_sz);
    if (!prompt)
        return AIRY_ENOMEM;

    int wn = snprintf(prompt, prompt_sz,
                      "You are an impartial arbiter%s%s. "
                      "Multiple models produced inconsistent responses. "
                      "Select the BEST response by replying with ONLY its number (1-%zu).\n\n",
                      arbiter_model ? " using model " : "", arbiter_model ? arbiter_model : "",
                      input_count);
    if (wn <= 0 || (size_t)wn >= prompt_sz) {
        AIRY_FREE(prompt);
        return AIRY_EINVAL;
    }

    for (size_t i = 0; i < input_count; i++) {
        const char *cand = inputs[i] ? inputs[i] : "(empty)";
        size_t cand_len = strlen(cand);
        size_t truncate = cand_len > 256 ? 256 : cand_len;

        char buf[320];
        int bn = snprintf(buf, sizeof(buf), "Candidate %zu:\n%.*s\n\n", i + 1, (int)truncate, cand);
        if (bn <= 0)
            continue;

        size_t cur_len = strlen(prompt);
        size_t remaining = prompt_sz - cur_len - 1;
        if ((size_t)bn < remaining) {
            strncat(prompt, buf, remaining);
        }
    }

    size_t remaining = prompt_sz - strlen(prompt) - 1;
    strncat(prompt, "Reply with ONLY the number of the best candidate (1-", remaining);
    remaining = prompt_sz - strlen(prompt) - 1;
    char numbuf[32];
    snprintf(numbuf, sizeof(numbuf), "%zu).", input_count);
    strncat(prompt, numbuf, remaining);

    llm_message_t msgs[2];
    __builtin_memset(&msgs, 0, sizeof(msgs));
    msgs[0].role = "system";
    msgs[0].content = "You are an impartial arbiter.";
    msgs[1].role = "user";
    msgs[1].content = prompt;

    llm_request_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.model = arbiter_model;
    cfg.messages = msgs;
    cfg.message_count = 2;
    cfg.temperature = 0.2f;
    cfg.top_p = 1.0f;
    cfg.max_tokens = 256;
    cfg.stream = 0;

    llm_response_t *resp = NULL;
    int ret = llm_ops->service_complete(svc, &cfg, &resp);
    AIRY_FREE(prompt);

    if (ret != 0 || !resp || !resp->choices || resp->choice_count == 0 ||
        !resp->choices[0].content) {
        if (resp && llm_ops->response_free)
            llm_ops->response_free(resp);
        AIRY_LOG_WARN("arbiter: LLM service_complete failed (ret=%d)", ret);
        return AIRY_ESERVICE;
    }

    int choice = parse_arbiter_choice(resp->choices[0].content, input_count);
    AIRY_LOG_DEBUG("arbiter: LLM response='%s' parsed_choice=%d", resp->choices[0].content, choice);

    if (llm_ops->response_free)
        llm_ops->response_free(resp);

    if (choice < 1) {
        AIRY_LOG_WARN("arbiter: failed to parse choice from LLM response, falling back");
        return AIRY_ESERVICE;
    }

    *out_choice = choice;
    return AIRY_SUCCESS;
}

/**
 * @brief External arbitration function — P2.7: actually call LLM arbitration.
 */
static airy_err_t arbiter_coordinate(airy_coordinator_base_t *base,
                                     const airy_coordination_context_t *
                                         context,
                                     const char **inputs, size_t input_count, char **out_result)
{
    if (!base || !out_result)
        return AIRY_EINVAL;

    arbiter_data_t *data = (arbiter_data_t *)base->data;
    if (!data)
        return AIRY_EINVAL;

    airy_mtx_lock(data->lock);

    if (input_count == 0 || !inputs) {
        *out_result = AIRY_STRDUP("no_input");
        if (!*out_result) {
            airy_mtx_unlock(data->lock);
            return AIRY_ENOMEM;
        }
        airy_mtx_unlock(data->lock);
        return AIRY_SUCCESS;
    }

    if (data->human_callback) {
        char question[1024];
        snprintf(question, sizeof(question), "多个模型输出不一致，请选择最佳结果：\n");

        for (size_t i = 0; i < input_count && i < 5; i++) {
            char option[256];
            snprintf(option, sizeof(option), "%zu. %s\n", i + 1, inputs[i]);
            strncat(question, option, sizeof(question) - strlen(question) - 1);
        }

        char answer[512];
        data->human_callback(question, answer, sizeof(answer));

        int choice = atoi(answer);
        if (choice >= 1 && choice <= (int)input_count) {
            *out_result = AIRY_STRDUP(inputs[choice - 1]);
        } else {
            *out_result = AIRY_STRDUP("invalid_choice");
        }
        if (!*out_result) {
            airy_mtx_unlock(data->lock);
            return AIRY_ENOMEM;
        }
        airy_mtx_unlock(data->lock);
        return AIRY_SUCCESS;
    }

    /* P2.7 path 2: LLM arbitration — read config under lock, then call the
     * network outside the lock (avoid holding the lock during a long
     * blocking call; arbiter_llm_arbitrate is a pure function) */
    if (data->arbiter_model && base->llm) {
        const char *arbiter_model = data->arbiter_model;
        llm_service_t *llm = base->llm;
        airy_mtx_unlock(data->lock);

        int choice = -1;
        airy_err_t err = arbiter_llm_arbitrate(llm, arbiter_model, inputs, input_count, &choice);
        if (err == AIRY_SUCCESS && choice >= 1 && (size_t)choice <= input_count) {
            AIRY_LOG_INFO("arbiter: LLM selected candidate %d/%zu", choice, input_count);
            *out_result = AIRY_STRDUP(inputs[choice - 1]);
            return *out_result ? AIRY_SUCCESS : AIRY_ENOMEM;
        }
        AIRY_LOG_WARN("arbiter: LLM arbitration failed (err=%d choice=%d), "
                      "falling back to inputs[0]",
                      (int)err, choice);

        *out_result = AIRY_STRDUP(inputs[0]);
        return *out_result ? AIRY_SUCCESS : AIRY_ENOMEM;
    } else if (data->arbiter_model && !base->llm) {
        AIRY_LOG_WARN("arbiter: arbiter_model configured but base->llm is NULL "
                      "(create() 未注入 LLM 句柄), falling back to inputs[0]");
        *out_result = AIRY_STRDUP(inputs[0]);
        airy_mtx_unlock(data->lock);
        if (!*out_result)
            return AIRY_ENOMEM;
        return AIRY_SUCCESS;
    }

    *out_result = AIRY_STRDUP(inputs[0]);
    if (!*out_result) {
        airy_mtx_unlock(data->lock);
        return AIRY_ENOMEM;
    }

    airy_mtx_unlock(data->lock);
    return AIRY_SUCCESS;
}

/**
 * @brief Create an external arbitration coordinator.
 */
airy_err_t airy_coord_arbiter_create(const char *arbiter_model,
                                     void (*human_callback)(const char *question, char *answer,
                                                            size_t max_len),
                                     airy_coordinator_base_t **out_base)
{
    if (!out_base)
        return AIRY_EINVAL;

    airy_coordinator_base_t *base =
        (airy_coordinator_base_t *)AIRY_CALLOC(1, sizeof(airy_coordinator_base_t));
    if (!base)
        return AIRY_ENOMEM;

    arbiter_data_t *data = (arbiter_data_t *)AIRY_CALLOC(1, sizeof(arbiter_data_t));
    if (!data) {
        AIRY_FREE(base);
        return AIRY_ENOMEM;
    }

    data->lock = airy_mtx_create();
    if (!data->lock) {
        AIRY_FREE(data);
        AIRY_FREE(base);
        return AIRY_ENOMEM;
    }

    if (arbiter_model) {
        data->arbiter_model = AIRY_STRDUP(arbiter_model);
        if (!data->arbiter_model) {
            airy_mtx_free(data->lock);
            AIRY_FREE(data);
            AIRY_FREE(base);
            return AIRY_ENOMEM;
        }
    }

    data->human_callback = human_callback;

    base->data = data;
    base->llm = NULL;
    base->coordinate = arbiter_coordinate;
    base->destroy = arbiter_destroy;

    *out_base = base;
    return AIRY_SUCCESS;
}
