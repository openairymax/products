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
                         const char *model, airy_grad_role_e role, char **out_text,
                         size_t *out_len)
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
    /* C11：temperature_set 哨兵使 0.0 可显式下发（旧口径 0 被当缺省，
     * 机制面兜底 0.7）；固定种子保证同输入同结论。能力位由机制面
     * 盖印门控，本侧显式请求、机制侧裁剪。 */
    llm_cfg.temperature = 0.0f;
    llm_cfg.temperature_set = true;
    llm_cfg.seed = GRAD_SEED;
    llm_cfg.top_p = 1.0f;
    llm_cfg.max_tokens = (int)ctx->max_tokens;
    if (llm_cfg.max_tokens == 0)
        llm_cfg.max_tokens = 4096;
    /* T4（0.1.20）：流式位可配置（launch 透传）。机制闭包在 cfg->stream
     * 且回调就位时走流式分派，chunk 供 TTFB 计量与 U6 展示；聚合响应
     * 与非流式同构，下方 C11 完整性检查两态共用。 */
    llm_cfg.stream = ctx->stream ? 1 : 0;

    /* C12（§5.2b.9）：预算闸在排队之前（便宜守卫优先）——est×k 超出
     * 单次发送预算直接 AIRY_ERANGE（域关闭时零行为）；排队（进程级
     * 并发闸）超时 AIRY_EBUSY。两者同为 fail-closed：上游采纳模型 C
     * 的确定性判定。激活时同步回写发送上限（响应预留保障）。 */
    size_t est = grad_ctxb_est(system) + grad_ctxb_est(user);
    if (grad_ctxb_active(&ctx->ctxb)) {
        double pred = (double)est * ctx->ctxb.k;
        if (pred > (double)ctx->ctxb.send_budget) {
            AIRY_LOG_WARN("GRAD-LLM: over budget (est=%zu k=%.2f send=%u)", est,
                          ctx->ctxb.k, ctx->ctxb.send_budget);
            return AIRY_ERANGE;
        }
        uint32_t slot_left = ctx->ctxb.n_ctx_slot - (uint32_t)est;
        llm_cfg.max_tokens =
            (uint32_t)llm_cfg.max_tokens < slot_left ? llm_cfg.max_tokens : (int)slot_left;
        if (llm_cfg.max_tokens < 1)
            llm_cfg.max_tokens = 1;
    }

    airy_err_t gate = grad_ctxb_sem_acquire(ctx->max_concurrent, ctx->server_parallel,
                                            GRAD_INFLIGHT_QUEUE_MS);
    if (gate != AIRY_SUCCESS)
        return gate;

    /* 响应释放入口来自注入的 LLM ops 表（llm_d），表缺失时跳过释放。 */
    const airy_llm_ops_t *llm_ops = are_ops_get_llm();

    llm_response_t *resp = NULL;
    int ret = ctx->complete(ctx->complete_ctx, &llm_cfg, &resp);

    if (ret != 0 || !resp || !resp->choices || resp->choice_count == 0 ||
        !resp->choices[0].content) {
        if (resp && llm_ops && llm_ops->response_free)
            llm_ops->response_free(resp);
        grad_ctxb_sem_release();
        AIRY_LOG_WARN("GRAD-LLM: call failed (ret=%d)", ret);
        return AIRY_ESERVICE;
    }

    /* C11 非流式完整性（策略侧）：机制面对非流式不判（max_tokens 截断
     * 属合法消费），GRAD 下游是 JSON 契约解析器——截断/内容过滤/缺终结
     * 必须在此显式失败，禁止半截输出流入 plan/verdict 解析。 */
    const char *fr = resp->finish_reason;
    if (!fr || llm_finish_is_truncated(fr) ||
        strcmp(fr, LLM_FINISH_CONTENT_FILTER) == 0) {
        if (llm_ops && llm_ops->response_free)
            llm_ops->response_free(resp);
        grad_ctxb_sem_release();
        AIRY_LOG_WARN("GRAD-LLM: incomplete output (finish_reason=%s)",
                      fr ? fr : "absent");
        return AIRY_ESERVICE;
    }

    size_t len = strlen(resp->choices[0].content);
    char *text = (char *)AIRY_MALLOC(len + 1);
    if (!text) {
        if (llm_ops && llm_ops->response_free)
            llm_ops->response_free(resp);
        grad_ctxb_sem_release();
        return AIRY_ENOMEM;
    }
    __builtin_memcpy(text, resp->choices[0].content, len);
    text[len] = '\0';

    /* 0.1.20 B1-e：真实 token 消耗按调用角色分槽累计（思考 token 保留），
     * 供 feedback / 成本复算区分 prefill/decode 归属模型 A/B。 */
    ctx->usage[role].prompt += resp->prompt_tokens;
    ctx->usage[role].completion += resp->completion_tokens;
    ctx->usage[role].total += resp->total_tokens;

    /* C12：真实 prompt token 回馈 k（EWMA），后续轮次预算收敛到真值。 */
    grad_ctxb_k_update(&ctx->ctxb, est, resp->prompt_tokens);
    grad_ctxb_sem_release();

    *out_text = text;
    *out_len = len;
    if (llm_ops && llm_ops->response_free)
        llm_ops->response_free(resp);
    return AIRY_SUCCESS;
}
