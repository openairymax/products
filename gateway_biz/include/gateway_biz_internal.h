/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
/**
 * @file gateway_biz_internal.h
 * @brief Internal shared definitions of the gateway business handler
 *        (module-private, not for external export).
 *
 * Splits the 2608-line gateway_business_handler.c into four files by
 * single responsibility; this header carries the shared contract among
 * them:
 *   - gateway_biz_forward.c  namespace forwarding (L2 protocol client + whitelist)
 *   - gateway_biz_agent.c    agent.run orchestration (dual-think + tool loop + cancel)
 *   - gateway_biz_backend.c  MCP/OpenAI/A2A protocol backends
 *   - gateway_business_handler.c  main dispatch and ctx lifecycle
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H
#define AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <cjson/cJSON.h>

#include "gateway_business_handler.h"
#include "gateway_mcp_server.h"

#include "airy_budget.h"
#include "airy_defaults.h"
#include "airy_memory.h"
#include "atomic_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 转发预算统一取预算树（0.1.21 C-21）：gateway 是 llm_d / tool_d /
 * agent_d 的调用方，三个值不再写死。语义分层——
 *   llm   : server 层（llm_d 内部一次完整调用 + 重试的背压上限）
 *   tool  : tool 的外层（内层 60s 兜底，外层留 25% 余量）
 *   run   : client 层（agent.run 可长跑，只有它跟随最外层预算）
 * 写死字面量曾使 gateway 90s 与 llm_d 85s 之间只剩 5s 余量。 */
#define GW_LLM_DEFAULT_TIMEOUT_MS ((int)airy_budget_ms(AIRY_BUDGET_SERVER))
/* think 转发预算：think 是 server 的被调用方，取 server 层（≥ think × 1.2）。 */
#define GW_THINK_TIMEOUT_MS ((int)airy_budget_ms(AIRY_BUDGET_SERVER))
#define GW_LLM_MAX_RESP 1048576
/* LLM daemon 端点缺省端口：SSoT 引用，真值唯一定义于 commons/include/airy_defaults.h */
#define GW_LLM_DEFAULT_TCP_PORT AIRY_PORT_LLM_D
#define GW_EXTERNAL_AGENT_ID "external"

/* tool 外层预算：tool_d 内层兜底取 tool 层，调用方留 25% 余量。 */
#define GW_TOOL_TIMEOUT_MS ((int)airy_budget_outer(airy_budget_ms(AIRY_BUDGET_TOOL)))

/* @brief Gateway business context: resolved daemon socket paths + model config
 *
 * All daemon endpoints are resolved once at create time
 * (env override -> $AIRY_RUNTIME_DIR/<name>.sock -> <name>.sock). */
struct gateway_business_ctx_s {
    char llm_sock_path[256];
    char llm_tcp_addr[64];
    uint16_t llm_tcp_port;
    char tool_sock_path[256];
    char agent_sock_path[256];
    char mem_sock_path[256];
    char sched_sock_path[256];
    char think_sock_path[256];
    char a2a_sock_path[256];
    char notify_sock_path[256];
    char market_sock_path[256];
    char hook_sock_path[256];
    char monit_sock_path[256];
    char channel_sock_path[256];
    char cupolas_sock_path[256];
    char maths_sock_path[256];
    char default_model[128];

    gateway_shutdown_fn_t on_shutdown;
    void *shutdown_user_data;
};

/* @brief Namespace forwarding rule: <ns>.<method> -> target daemon <method>
 *
 * 0.1.6 P1-4 收敛：能力存在性校验统一走能力注册表（gateway_cap_registry.h，
 * cap_key 单一权威源）。daemon 端点由 svc dispatch 钩子按命名空间路由表
 * 解析（gateway_biz_svcdispatch.c），无需在此维护。
 * wire 方法名取自注册表（method 字段）——请求前缀与目标命名空间
 * 可不一致（plugin.* → tool 命名空间 plugin_* 方法），字符串截取不再成立。 */
typedef struct {
    const char *ns;      /* 目标 daemon 命名空间（裸名，无尾点） */
    int timeout_ms;      /* 转发超时 */
    const char *method;  /* 目标 daemon 侧 wire 方法名（注册表权威值） */
} gw_ns_forward_rule_t;

/* ---- gateway_biz_tools.c（内置 MCP 工具注册 SSoT, 2026-08-30 S-6）----
 * 内置工具 JSON schema 唯一权威源；返回注册失败数（0 = 全成功）。 */
int gw_biz_mcp_register_tools(gw_mcp_server_t *mcp, void *user_data);

/* ---- gateway_biz_forward.c (L2 protocol client + namespace forwarding) ---- */
char *jsonrpc_error(int code, const char *msg, const cJSON *id);
char *jsonrpc_emit(cJSON *rroot, const cJSON *req_id);
char *gw_svc_call(const char *sock_path, const char *method, const char *params_json,
                  int timeout_ms);
int gw_acl_check_tool(const gateway_business_ctx_t *ctx, const char *tool_name);
char *handle_ns_forward(cJSON *root, const gw_ns_forward_rule_t *rule);
char *handle_mem_call(cJSON *root);
char *handle_llm_list_models(cJSON *root, const gateway_business_ctx_t *ctx);
char *handle_tool_approval_call(cJSON *root, const gateway_business_ctx_t *ctx,
                                const char *tool_method);

/* ---- gateway_pep_cache.c (M2-S5 PEP 裁定缓存：epoch 失效键) ---- */
#include "gateway_pep_cache.h"

/* ---- gateway_biz_svcdispatch.c (微核心服务统一派发钩子, 2026-08-25) ---- */
int gw_sys_svc_dispatch_init(gateway_business_ctx_t *ctx);
void gw_sys_svc_dispatch_cleanup(void);

/* ---- gateway_biz_supactivate.c (V13.2 按需激活单向通知) ---- */
#include "gateway_biz_supactivate.h"

/* ---- gateway_biz_hall.c (hall.* — task board / event stream / chain) ---- */
char *handle_hall_call(cJSON *root, gateway_business_ctx_t *ctx);

/* ---- gateway_biz_agent.c (agent.run / agent.cancel 转发) ----
 * agent.run 引擎（会话注册表/编排/工具循环）已迁 agent_d；gateway 仅转发。 */
char *handle_agent_run(cJSON *root, gateway_business_ctx_t *ctx);
char *handle_agent_cancel(cJSON *root, gateway_business_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H */
