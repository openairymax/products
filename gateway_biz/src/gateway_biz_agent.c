// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_agent.c
 * @brief Gateway agent.run / agent.cancel 转发域。
 *
 * agent.run 进程内引擎（会话注册表 / GCCP 双思考 / 编排 / ReAct 工具循环 /
 * mem 持久化 / hall 事件）已迁入 agent_d（agent_run_engine.c +
 * agent_run_loop.c），gateway 本文件仅做协议转发：
 *   - agent.run        -> agent_d "run"（参数原样透传，响应 id 重写）
 *   - agent.cancel     -> agent_d "run_cancel"（session_id 透传）
 * gateway 不再承载任何 agent.run 业务分支（K-1 纯翻译）。
 */

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"
#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* agent.run 长耗时（LLM 往返 + 工具循环），转发取预算树最外层 client 层。 */
#define GW_RUN_FWD_TIMEOUT_MS airy_budget_ms(AIRY_BUDGET_CLIENT)

/**
 * @brief agent 域转发管道单源：params 序列化 -> SYS_SVC_CALL 派发 ->
 *        响应解析 -> jsonrpc_emit（响应 id 重写为请求 id）。
 * @return jsonrpc 响应文本（调用方接管所有权）；失败返回 jsonrpc_error 文本。
 */
static char *agent_svc_forward(cJSON *root, const char *method, long timeout_ms)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    char *params_str = params ? cJSON_PrintUnformatted(params) : AIRY_STRDUP("{}");
    if (!params_str)
        return jsonrpc_error(-32603, "Out of memory", id);

    /* 架构约束 2026-08-25 "必须走 syscall": agent 域经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("agent", method, params_str, timeout_ms, &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp)
        return jsonrpc_error(-32603, "Agent service unreachable", id);

    cJSON *rroot = cJSON_Parse(resp);
    if (!rroot) {
        char head[81];
        size_t i = 0;
        for (; i < sizeof(head) - 1 && resp[i]; i++) {
            unsigned char ch = (unsigned char)resp[i];
            head[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
        }
        head[i] = '\0';
        AIRY_LOG_WARN("agent forward: malformed response method=%s len=%zu head=%s", method,
                      strlen(resp), head);
        AIRY_FREE(resp);
        return jsonrpc_error(-32603, "Agent service returned invalid response", id);
    }
    AIRY_FREE(resp);

    return jsonrpc_emit(rroot, id);
}

/**
 * @brief agent.run 转发：解析最小入参契约（prompt 存在性校验），其余
 *        参数原样透传 agent_d "run"，响应 id 重写为请求 id。
 */
char *handle_agent_run(cJSON *root, gateway_business_ctx_t *ctx)
{
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    const char *prompt = NULL;
    if (params) {
        cJSON *p = cJSON_GetObjectItem(params, "prompt");
        if (cJSON_IsString(p)) {
            prompt = p->valuestring;
        } else {
            cJSON *messages = cJSON_GetObjectItem(params, "messages");
            cJSON *m0 = (messages && cJSON_GetArraySize(messages) > 0) ?
                            cJSON_GetArrayItem(messages, 0) :
                            NULL;
            cJSON *c = m0 ? cJSON_GetObjectItem(m0, "content") : NULL;
            if (cJSON_IsString(c))
                prompt = c->valuestring;
        }
    }
    if (!prompt || !*prompt)
        return jsonrpc_error(-32602, "Invalid params: missing prompt", id);

    return agent_svc_forward(root, "run", GW_RUN_FWD_TIMEOUT_MS);
}

/**
 * @brief task.submit 转发（C-15 统一任务编排入口）：goal / resume_token
 *        二者其一存在即原样透传 agent_d "task_submit"。模式裁决、GCCP
 *        门禁、计划宣告与执行均在 daemon 内完成；未确认目标返回
 *        needs_intent（零工具调用），gateway 不做任何业务判断。
 */
char *handle_task_submit(cJSON *root, gateway_business_ctx_t *ctx)
{
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (!params)
        return jsonrpc_error(-32602, "Invalid params: missing params", id);

    cJSON *goal = cJSON_GetObjectItem(params, "goal");
    cJSON *resume = cJSON_GetObjectItem(params, "resume_token");
    int has_goal = cJSON_IsString(goal) && goal->valuestring && goal->valuestring[0];
    int has_resume = cJSON_IsString(resume) && resume->valuestring && resume->valuestring[0];
    if (!has_goal && !has_resume)
        return jsonrpc_error(-32602, "Invalid params: missing goal or resume_token", id);

    return agent_svc_forward(root, "task_submit", GW_RUN_FWD_TIMEOUT_MS);
}

/**
 * @brief agent.cancel 转发：params.session_id 原样透传 agent_d "run_cancel"，
 *        响应 id 重写为请求 id。
 */
char *handle_agent_cancel(cJSON *root, gateway_business_ctx_t *ctx)
{
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *sid = params ? cJSON_GetObjectItem(params, "session_id") : NULL;
    if (!cJSON_IsString(sid) || !sid->valuestring || !*sid->valuestring)
        return jsonrpc_error(-32602, "Invalid params: missing session_id", id);

    return agent_svc_forward(root, "run_cancel", GW_TOOL_TIMEOUT_MS);
}
