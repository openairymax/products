// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_llm_call.c
 * @brief GRAD 统一 LLM 调用域
 *
 * 经 airy_grad_complete_fn 补全闭包发起调用（M5-4 闭包化解耦——策略侧
 * 不持有机制核 LLM 句柄）；响应释放经注入的 LLM ops 表。逐次累计真实
 * token 消耗（思考 token 保留）。
 */

#include "grad_internal.h"

#include "airy_llm_ops.h"

airy_err_t grad_llm_call(grad_llm_ctx_t *ctx, const char *system, const char *user,
                         const char *model, char **out_text, size_t *out_len)
{
    if (!ctx || !system || !user || !out_text || !out_len)
        return AIRY_EINVAL;

    *out_text = NULL;
    *out_len = 0;

    if (!ctx->complete)
        return AIRY_ESERVICE;

    llm_message_t msgs[2];
    __builtin_memset(&msgs, 0, sizeof(msgs));
    msgs[0].role = "system";
    msgs[0].content = system;
    msgs[1].role = "user";
    msgs[1].content = user;

    llm_request_config_t llm_cfg;
    __builtin_memset(&llm_cfg, 0, sizeof(llm_cfg));
    llm_cfg.model = model;
    llm_cfg.messages = msgs;
    llm_cfg.message_count = 2;
    llm_cfg.temperature = 0.2f;
    llm_cfg.top_p = 1.0f;
    llm_cfg.max_tokens = (int)ctx->max_tokens;
    if (llm_cfg.max_tokens == 0)
        llm_cfg.max_tokens = 4096;
    llm_cfg.stream = 0;

    /* 响应释放入口来自注入的 LLM ops 表（llm_d），表缺失时跳过释放。 */
    const airy_llm_ops_t *llm_ops = are_ops_get_llm();

    llm_response_t *resp = NULL;
    int ret = ctx->complete(ctx->complete_ctx, &llm_cfg, &resp);

    if (ret != 0 || !resp || !resp->choices || resp->choice_count == 0 ||
        !resp->choices[0].content) {
        if (resp && llm_ops && llm_ops->response_free)
            llm_ops->response_free(resp);
        AIRY_LOG_WARN("GRAD-LLM: call failed (ret=%d)", ret);
        return AIRY_ESERVICE;
    }

    size_t len = strlen(resp->choices[0].content);
    char *text = (char *)AIRY_MALLOC(len + 1);
    if (!text) {
        if (llm_ops && llm_ops->response_free)
            llm_ops->response_free(resp);
        return AIRY_ENOMEM;
    }
    __builtin_memcpy(text, resp->choices[0].content, len);
    text[len] = '\0';

    /* 2.1.1.6 修复：保留真实 token 消耗（思考 token），逐次累计到 ctx */
    ctx->prompt_tokens += resp->prompt_tokens;
    ctx->completion_tokens += resp->completion_tokens;
    ctx->total_tokens += resp->total_tokens;

    *out_text = text;
    *out_len = len;
    if (llm_ops && llm_ops->response_free)
        llm_ops->response_free(resp);
    return AIRY_SUCCESS;
}
