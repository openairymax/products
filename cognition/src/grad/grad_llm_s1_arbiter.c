// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_llm_s1_arbiter.c
 * @brief GRAD 模型 B（t1-f）情境仲裁域
 *
 * 目标 G + 模型 C 验证报告 → LLM 终裁（C2 契约，0.1.20 §5.2b.4）。
 *
 * 0.1.19 的两处结构性缺陷在此收口：
 *   1) prompt 只拼"目标 + 报告"，计划节点从不进入 B 的视野——B 只能凭
 *      报告里单个 affected_node 猜测，驳回意见因此缺乏可定位证据，退化
 *      为固定模板 patch；
 *   2) 装配没有预算概念，长计划直接把上下文顶爆。
 *
 * 现按六级优先级装配（预算由 C12 char_cap 派生，域关闭时回退
 * GRAD_ARBITER_PROMPT_MAX_CHARS）：
 *   1. goal（不裁；超 GRAD_ARBITER_GOAL_MAX_CHARS 截断并标 goal_truncated）
 *   2. C 验证报告（不裁）
 *   3. 审查焦点节点全文（guard 节点 ∪ 报告 affected_node，上限 8）
 *   4. 其余节点摘要（id/role/prio/deps；自最低 priority 起丢弃并计数）
 *   5. 轮次历史（≤3，保留最近 1 条）
 *   6. guard 节点 id(goal)（与第 3 项合并去重）
 *
 * priority 的数值语义是"越大越关键"（255=verify、254=qa、150=缺省），
 * 因此裁剪自数值最小者起、同值按节点原序，装配结果确定可复现。
 *
 * LLM 不可用时返回空裁决，协调器采纳 C 的确定性判定（保守策略）。
 */

#include "grad_internal.h"

#include <stdarg.h>

/* 焦点节点上限：全文注入的节点数，防止 guard 密集计划撑爆 prompt */
#define ARB_FOCUS_MAX 8

#define ARB_FOOTER "\n\nRender your verdict now as the single JSON object defined above."

static const char GRAD_S1_ARBITER_SYSTEM_PROMPT[] =
    "You are the contextual arbiter (Model B) in a goal-anchored planning system. "
    "A deterministic verifier (Model C) has inspected a task plan DAG. "
    "Judge whether the plan actually reaches the confirmed goal G.\n"
    "Reply with ONLY one JSON object (no markdown fences, no prose):\n"
    "{\"verdict\":\"accept|reject\",\"confidence\":0.0-1.0,"
    "\"e04\":\"pass|fail|unknown\","
    "\"defects\":[{\"node_id\":\"<plan node id>\","
    "\"code\":\"E-01|E-02|E-03|E-04|E-05|SEM\","
    "\"evidence\":\"...\",\"suggestion\":\"...\"}],"
    "\"affected_scope\":[\"<plan node id>\"],\"opinion\":\"...\"}\n"
    "Rules:\n"
    "- verdict=reject requires at least one defect whose node_id is copied "
    "verbatim from the plan, and a non-empty affected_scope listing the node "
    "ids to repair. Never reject with an empty affected_scope.\n"
    "- code: E-01 causality break, E-02 deadlock, E-03 resource overflow, "
    "E-04 purpose drift, E-05 insufficient verification metadata, "
    "SEM semantic mismatch with G.\n"
    "- confidence is a number in [0,1]. Low confidence forfeits the ruling "
    "to Model C.\n"
    "- e04: fail when an invariant-guard node protects entities G never "
    "mentions, pass when all guards align with G, unknown when the plan has "
    "no guard node.\n"
    "- verdict=accept must still be the full contract object; never answer "
    "outside it.";

/**
 * @brief 可增长拼接缓冲（OOM 后置 bad，后续写入静默丢弃）。
 *
 * 与 grad_patch.c 的定长 pbuf 不同：本文件的片段长度由计划内容决定，
 * 必须在运行期扩展；bad 标志让失败只需在收尾处检查一次。
 */
typedef struct abuf {
    char *p;
    size_t len;
    size_t cap;
    int bad;
} abuf_t;

static int ab_init(abuf_t *b)
{
    b->cap = 256;
    b->len = 0;
    b->bad = 0;
    b->p = (char *)AIRY_MALLOC(b->cap);
    if (!b->p) {
        b->cap = 0;
        b->bad = 1;
        return -1;
    }
    b->p[0] = '\0';
    return 0;
}

static int ab_grow(abuf_t *b, size_t need)
{
    if (b->bad)
        return -1;
    if (b->len + need + 1 <= b->cap)
        return 0;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + need + 1)
        nc *= 2;
    char *np = (char *)AIRY_REALLOC(b->p, nc);
    if (!np) {
        b->bad = 1;
        return -1;
    }
    b->p = np;
    b->cap = nc;
    return 0;
}

static void ab_add(abuf_t *b, const char *s)
{
    if (!s || !s[0] || b->bad)
        return;
    size_t n = strlen(s);
    if (ab_grow(b, n) != 0)
        return;
    AIRY_MEMCPY(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void ab_put(abuf_t *b, const char *fmt, ...)
{
    if (b->bad)
        return;
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->bad = 1;
        return;
    }
    if ((size_t)n < sizeof(tmp)) {
        ab_add(b, tmp);
        return;
    }
    if (ab_grow(b, (size_t)n) != 0)
        return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

static void ab_free(abuf_t *b)
{
    if (!b || !b->p)
        return;
    AIRY_FREE(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
    b->bad = 0;
}

/** @brief UTF-8 安全截断：退掉被截断的多字节序列首字节。 */
static size_t u8_cut(const char *s, size_t n)
{
    while (n > 0 && (s[n - 1] & 0xC0) == 0x80)
        n--;
    if (n > 0 && (s[n - 1] & 0x80))
        n--;
    return n;
}

/** @brief 焦点判定：guard 节点 ∪ 报告点名的受影响节点。 */
static int nd_focus(const airy_task_node_t *n, const airy_grad_report_t *rep)
{
    if (n->task_node_invariant_guard)
        return 1;
    if (rep->affected_node[0] && n->task_node_id &&
        strcmp(n->task_node_id, rep->affected_node) == 0)
        return 1;
    return 0;
}

static void nd_deps(abuf_t *b, const airy_task_node_t *n)
{
    ab_add(b, "[");
    for (size_t i = 0; i < n->task_node_depends_count; i++)
        ab_put(b, "%s%s", i ? "," : "",
               (n->task_node_depends_on && n->task_node_depends_on[i])
                   ? n->task_node_depends_on[i]
                   : "?");
    ab_add(b, "]");
}

static void nd_sig(abuf_t *b, const char *label, char **items, size_t count)
{
    ab_put(b, "%s=[", label);
    for (size_t i = 0; i < count; i++)
        ab_put(b, "%s%s", i ? "," : "", (items && items[i]) ? items[i] : "?");
    ab_add(b, "]");
}

/** @brief 焦点节点全文：B 需要看到 goal/依赖/签名才能给出可定位缺陷。 */
static void nd_full(abuf_t *b, const airy_task_node_t *n)
{
    ab_put(b, "- %s | role=%s | prio=%u | timeout=%ums | guard=%u\n",
           n->task_node_id ? n->task_node_id : "?",
           n->task_node_agent_role ? n->task_node_agent_role : "-",
           (unsigned)n->task_node_priority, (unsigned)n->task_node_timeout_ms,
           (unsigned)n->task_node_invariant_guard);
    ab_put(b, "  goal: %s\n", n->task_node_goal ? n->task_node_goal : "-");
    ab_add(b, "  depends: ");
    nd_deps(b, n);
    ab_add(b, "\n  ");
    nd_sig(b, "in", n->task_node_inputs, n->task_node_inputs_count);
    ab_add(b, " ");
    nd_sig(b, "out", n->task_node_outputs, n->task_node_outputs_count);
    ab_add(b, "\n");
}

static void nd_sum(abuf_t *b, const airy_task_node_t *n)
{
    /* §5.2b.9 S3：摘要行 goal 截 120 字符（UTF-8 安全），截断加注。 */
    const char *g = n->task_node_goal ? n->task_node_goal : "";
    char gc[121];
    int cut = 0;
    if (strlen(g) > 120) {
        size_t n8 = u8_cut(g, 120);
        AIRY_MEMCPY(gc, g, n8);
        gc[n8] = '\0';
        g = gc;
        cut = 1;
    }
    ab_put(b, "- %s | role=%s | prio=%u | goal=%s%s | depends=",
           n->task_node_id ? n->task_node_id : "?",
           n->task_node_agent_role ? n->task_node_agent_role : "-",
           (unsigned)n->task_node_priority, g, cut ? "[cut]" : "");
    nd_deps(b, n);
    ab_add(b, "\n");
}

/** @brief 追加一轮裁决到环形历史（保留最近 GRAD_ARBITER_MAX_HISTORY 条）。 */
static void hist_push(grad_llm_ctx_t *ctx, const grad_verdict_t *v)
{
    if (!ctx || !v)
        return;
    char line[GRAD_ARBITER_HIST_LINE];
    const char *code = v->defect_count ? v->defects[0].code : "none";
    const char *node = v->defect_count ? v->defects[0].node_id : "-";
    int n = snprintf(line, sizeof(line), "round=%u verdict=%s defects=%zu first=%s@%s",
                     (unsigned)ctx->round_index,
                     (v->kind == GRAD_VD_ACCEPT) ? "accept" : "reject", v->defect_count,
                     code, node);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(line))
        line[sizeof(line) - 1] = '\0';

    if (ctx->round_hist_count < GRAD_ARBITER_MAX_HISTORY) {
        AIRY_MEMCPY(ctx->round_hist[ctx->round_hist_count], line, strlen(line) + 1);
        ctx->round_hist_count++;
        return;
    }
    for (uint32_t i = 1; i < GRAD_ARBITER_MAX_HISTORY; i++)
        AIRY_MEMCPY(ctx->round_hist[i - 1], ctx->round_hist[i], GRAD_ARBITER_HIST_LINE);
    AIRY_MEMCPY(ctx->round_hist[GRAD_ARBITER_MAX_HISTORY - 1], line, strlen(line) + 1);
}

/**
 * @brief 构建 B 的 user prompt（六级装配，见文件头）。
 *
 * @param over_budget [out] C12 激活且不可裁核心超预算时置 1（返回 NULL）
 * @param out_omitted [out] 预算内放不下而被丢弃的摘要节点数（可 NULL）
 * @ownership 返回串由调用方 AIRY_FREE；OOM 或超预算返回 NULL。
 */
static char *arb_build_prompt(const airy_gccp_goal_t *goal, const airy_grad_report_t *report,
                              const airy_task_plan_t *plan, const grad_llm_ctx_t *ctx,
                              int *over_budget, size_t *out_omitted)
{
    if (over_budget)
        *over_budget = 0;
    if (out_omitted)
        *out_omitted = 0;

    char *goal_json = grad_goal_to_json(goal, ctx);
    const char *gfrag = goal_json ? goal_json : "null";
    int goal_cut = 0;
    if (strlen(gfrag) > GRAD_ARBITER_GOAL_MAX_CHARS) {
        goal_json[u8_cut(goal_json, GRAD_ARBITER_GOAL_MAX_CHARS)] = '\0';
        goal_cut = 1;
    }

    char *report_json = NULL;
    if (airy_grad_report_to_json(report, &report_json) != AIRY_SUCCESS || !report_json) {
        /* OOM 时退化为空对象：报告仅用于注入与落盘，不影响契约解析 */
        report_json = NULL;
    }

    abuf_t core = {0}, sums = {0}, guards = {0}, hist = {0};
    char *out = NULL;
    int init_ok = ab_init(&core);
    init_ok |= ab_init(&sums);
    init_ok |= ab_init(&guards);
    init_ok |= ab_init(&hist);
    if (init_ok != 0)
        goto done;

    ab_add(&core, "You arbitrate a GRAD task plan against its confirmed goal.\n\n"
                  "Confirmed goal (G):\n");
    ab_add(&core, gfrag);
    if (goal_cut)
        ab_add(&core, "\n[goal_truncated]");
    ab_add(&core, "\n\nVerification report (Model C):\n");
    ab_add(&core, report_json ? report_json : "{}");
    ab_add(&core, "\n\nFocus nodes (full text; every defect node_id must exist in "
                  "this plan):\n");

    size_t focus_n = 0;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *n = plan->task_plan_nodes[i];
        if (!n || !nd_focus(n, report))
            continue;
        if (focus_n >= ARB_FOCUS_MAX)
            continue;
        nd_full(&core, n);
        focus_n++;
    }
    if (focus_n == 0)
        ab_add(&core, "(none)\n");

    /* 未被全文渲染的 guard 节点：id(goal) 供 E-04 目的漂移评估（去重） */
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *n = plan->task_plan_nodes[i];
        if (!n || !n->task_node_invariant_guard || nd_focus(n, report))
            continue;
        ab_put(&guards, "%s (%s)\n", n->task_node_id ? n->task_node_id : "?",
               n->task_node_goal ? n->task_node_goal : "");
    }

    for (uint32_t i = 0; i < ctx->round_hist_count; i++)
        ab_put(&hist, "%s\n", ctx->round_hist[i]);

    /* 预算（C12 §5.2b.9）：cap 由 n_ctx_slot 派生（域关闭时回退
     * GRAD_ARBITER_PROMPT_MAX_CHARS）。1-3 级不可裁；cap 内放不下核心
     * 时返回 NULL + over_budget=1（fail-closed，B 侧弃权、采纳 C 的
     * 确定性判定）。域关闭时保持旧行为（允许溢出、丢摘要），零回归。 */
    size_t footer = sizeof(ARB_FOOTER) - 1;
    size_t limit = grad_ctxb_char_cap(&ctx->ctxb);
    size_t base = core.len + guards.len + hist.len + footer + 128;
    if (base > limit) {
        if (grad_ctxb_active(&ctx->ctxb)) {
            if (over_budget)
                *over_budget = 1;
            goto done;
        }
        AIRY_LOG_WARN("GRAD-B: prompt core %zu exceeds budget %d, summaries dropped", base,
                      GRAD_ARBITER_PROMPT_MAX_CHARS);
        limit = base;
    }

    /* 第 4 级：其余节点摘要，自最低 priority 起丢弃（同值按原序） */
    size_t shown = 0, omitted = 0;
    int full = 0;
    for (int p = 255; p >= 0; p--) {
        for (size_t i = 0; i < plan->task_plan_node_count; i++) {
            const airy_task_node_t *n = plan->task_plan_nodes[i];
            if (!n || nd_focus(n, report) || (int)n->task_node_priority != p)
                continue;
            if (full) {
                omitted++;
                continue;
            }
            abuf_t line;
            if (ab_init(&line) != 0) {
                full = 1;
                omitted++;
                continue;
            }
            nd_sum(&line, n);
            if (!line.bad && base + sums.len + line.len <= limit) {
                ab_add(&sums, line.p);
                shown++;
            } else {
                full = 1;
                omitted++;
            }
            ab_free(&line);
        }
    }
    if (out_omitted)
        *out_omitted = omitted;

    out = (char *)AIRY_MALLOC(core.len + sums.len + guards.len + hist.len + footer + 256);
    if (out) {
        abuf_t buf;
        buf.p = out;
        buf.len = 0;
        buf.cap = core.len + sums.len + guards.len + hist.len + footer + 256;
        buf.bad = 0;
        out[0] = '\0';

        ab_add(&buf, core.p);
        ab_put(&buf, "\nOther nodes (summary; %zu shown, %zu omitted, lowest priority "
                     "dropped first):\n", shown, omitted);
        ab_add(&buf, sums.len ? sums.p : "(none)\n");
        ab_add(&buf, "\nInvariant-guard nodes (E-04 scope):\n");
        ab_add(&buf, guards.len ? guards.p : "none\n");
        if (hist.len) {
            ab_add(&buf, "\nRound history (most recent last; do not repeat a fixed "
                         "defect):\n");
            ab_add(&buf, hist.p);
        }
        ab_add(&buf, ARB_FOOTER);
    }

done:
    ab_free(&core);
    ab_free(&sums);
    ab_free(&guards);
    ab_free(&hist);
    if (goal_json)
        AIRY_FREE(goal_json);
    if (report_json)
        AIRY_FREE(report_json);
    return out;
}

airy_err_t grad_llm_s1_arbiter(const airy_gccp_goal_t *goal, const airy_grad_report_t *report,
                               const airy_task_plan_t *plan, char **out_verdict, void *user_data)
{
    if (!out_verdict)
        return AIRY_EINVAL;
    *out_verdict = NULL;

    grad_llm_ctx_t *ctx = (grad_llm_ctx_t *)user_data;
    if (!ctx)
        return AIRY_EINVAL;
    if (!report || !plan)
        return AIRY_EINVAL;
    /* ctx 为装配主来源；入参 goal 与之同源，缺失时回落到 ctx */
    const airy_gccp_goal_t *g = goal ? goal : ctx->goal;

    /* 与 b_arbiter 同轮落盘报告：verify → arbitrate → fix 决策链完整 */
    if (ctx->workspace_root && ctx->plan_id) {
        char *report_json = NULL;
        if (airy_grad_report_to_json(report, &report_json) == AIRY_SUCCESS && report_json) {
            char *c_dir = grad_ws_dir(ctx);
            if (c_dir) {
                char c_fname[64];
                snprintf(c_fname, sizeof(c_fname), "round_%u.json",
                         (unsigned)ctx->round_index);
                grad_ws_write_file(c_dir, "c_verify", c_fname, report_json,
                                   strlen(report_json));
                grad_llm_trace_append(ctx, "c_verify", report_json);
                AIRY_FREE(c_dir);
            }
            AIRY_FREE(report_json);
        }
    }

    int over_budget = 0;
    size_t omitted = 0;
    char *prompt = arb_build_prompt(g, report, plan, ctx, &over_budget, &omitted);

    /* C12：预算快照落决策链（仅激活时），供回溯审计与容量调参。 */
    if (grad_ctxb_active(&ctx->ctxb)) {
        char tb[192];
        snprintf(tb, sizeof(tb),
                 "{\"n_ctx_slot\":%u,\"send_budget\":%u,\"prompt_chars\":%zu,"
                 "\"over_budget\":%d,\"omitted_nodes\":%zu}",
                 ctx->ctxb.n_ctx_slot, ctx->ctxb.send_budget,
                 prompt ? strlen(prompt) : 0, over_budget, omitted);
        grad_llm_trace_append(ctx, "ctx_budget", tb);
    }

    if (!prompt) {
        if (over_budget) {
            /* C12 fail-closed：不可裁核心超预算，B 弃权（返回 ERANGE，
             * 协调器按错误处理即采纳 C 的确定性判定）。 */
            AIRY_LOG_WARN("GRAD-B: prompt over C12 budget, deferring to Model C");
            return AIRY_ERANGE;
        }
        /* C7 R4：装配 OOM 显式上报（此前返回 SUCCESS 静默采纳 C 判定，
         * B 故障在事件流不可归因）。协调器计 arbiter_unavailable 并发
         * grad_arbiter_unavailable 事件，C 判定语义不变。 */
        AIRY_LOG_WARN("GRAD-B: prompt assembly failed (OOM), deferring to Model C");
        return AIRY_ENOMEM;
    }

    char *verdict = NULL;
    size_t verdict_len = 0;
    airy_err_t err = grad_llm_call(ctx, GRAD_S1_ARBITER_SYSTEM_PROMPT, prompt,
                                   ctx->s1_verify_model, AIRY_GRAD_ROLE_B, &verdict,
                                   &verdict_len);
    AIRY_FREE(prompt);

    if (err != AIRY_SUCCESS || !verdict) {
        /* C7 R4：服务故障/空裁决显式上报（错误码透传，协调器计
         * arbiter_unavailable 并发事件），C 确定性判定语义不变。 */
        AIRY_LOG_WARN("GRAD-B: arbiter LLM unavailable (err=%d), deferring to Model C",
                      (int)err);
        return err != AIRY_SUCCESS ? err : AIRY_ESERVICE;
    }

    if (ctx->workspace_root && ctx->plan_id) {
        grad_llm_trace_append(ctx, "b_arbiter", verdict);
        char *dir = grad_ws_dir(ctx);
        if (dir) {
            char fname[64];
            /* P13：与 c_verify 共用当前 round_index（round 号由 S2 推进） */
            snprintf(fname, sizeof(fname), "round_%u.json", (unsigned)ctx->round_index);
            grad_ws_write_file(dir, "b_arbiter", fname, verdict, verdict_len);
            AIRY_FREE(dir);
        }
    }

    /* 轮次历史只记录契约合规的裁决：非法输出由协调器记
     * arbiter_parse_error 并降级，不应污染后续轮次记忆。 */
    grad_verdict_t vd;
    if (grad_verdict_parse(verdict, plan, &vd) == AIRY_SUCCESS) {
        hist_push(ctx, &vd);
        grad_verdict_free(&vd);
    }

    *out_verdict = verdict;
    return AIRY_SUCCESS;
}

/* ── C 失败路径定向仲裁（C13 §5.2b.11） ─────────────────────────────── */

static const char GRAD_S1_FAILURE_SYSTEM_PROMPT[] =
    "You are the contextual arbiter (Model B) in a goal-anchored planning system. "
    "Model C (deterministic verifier) has REJECTED a task plan DAG. You do not "
    "overturn that rejection: your only job is to say how the plan must be "
    "repaired.\n"
    "Reply with ONLY one JSON object (no markdown fences, no prose):\n"
    "{\"verdict\":\"accept|reject\","
    "\"defect_class\":\"E-01|E-02|E-03|E-04|E-05|SEM\","
    "\"salvageable\":true|false,\"affected_scope\":[\"<plan node id>\"],"
    "\"suggestion\":\"...\"}\n"
    "Rules:\n"
    "- verdict=accept: you agree the repair is purely mechanical, so Model A "
    "applies Model C's template patch unchanged.\n"
    "- verdict=reject requires a non-empty affected_scope whose ids are copied "
    "verbatim from the failed node or its listed neighbours. Never reject with "
    "an empty affected_scope.\n"
    "- defect_class: E-01 causality break, E-02 deadlock, E-03 resource "
    "overflow, E-04 purpose drift, E-05 insufficient verification metadata, "
    "SEM semantic mismatch with the goal.\n"
    "- salvageable=true: patching affected_scope is enough. salvageable=false: "
    "the plan must be redrawn from scratch.\n"
    "- suggestion is mandatory for verdict=reject: one sentence naming the "
    "concrete edit (which node, which field).\n"
    "- Do not repeat a defect already listed in the round history unless you "
    "explain why this plan cannot avoid it.";

#define ARB_FAIL_FOOTER "\n\nRender your verdict now as the single JSON object defined above."
#define ARB_FAIL_SCOPE_MAX 4
#define ARB_FAIL_SCOPE_CHARS 96

/**
 * @brief 失败路径 prompt 装配：核心三段（goal/C 报告/失败节点）不可裁。
 *
 * 消偏三规则要求 ①②③ 恒在；相邻节点与轮次历史属可裁证据，整体超
 * GRAD_ARBITER_PROMPT_MAX_CHARS 时一并丢弃并显式标注（提示 B 不得据此
 * 假设邻域为空）。token_budget 仅由协调器用于记账，硬上限仍是字符数。
 */
static char *arb_fail_prompt(const grad_arb_fail_in_t *in, int with_ext)
{
    abuf_t b;
    if (ab_init(&b) != 0)
        return NULL;

    ab_add(&b, "Model C rejected this plan. Decide how to repair it.\n\n"
               "Confirmed goal (G):\n");
    ab_add(&b, (in->goal_json && in->goal_json[0]) ? in->goal_json : "null");
    ab_put(&b, "\n\nVerification report (Model C, round %u):\n", (unsigned)(in->round + 1));
    ab_add(&b, (in->c_report_json && in->c_report_json[0]) ? in->c_report_json : "{}");
    ab_add(&b, "\n\nFailed node (full text, depth 1):\n");
    ab_add(&b, (in->failed_node_json && in->failed_node_json[0]) ? in->failed_node_json : "{}");

    if (with_ext) {
        ab_add(&b, "\n\nDirect neighbours (predecessors then successors, max 4 each):\n");
        ab_add(&b, (in->neighbors_json && in->neighbors_json[0]) ? in->neighbors_json : "{}");
        ab_add(&b, "\n\nPrevious rounds (most recent last, max 3):\n");
        ab_add(&b, (in->round_history_json && in->round_history_json[0])
                       ? in->round_history_json
                       : "[]");
        ab_add(&b, "\nRejected rounds above were judged by Model C, not by you. "
                   "Do NOT reproduce the same defect. If the same defect recurs, "
                   "state why the plan cannot avoid it.");
    } else {
        ab_add(&b, "\n\n[neighbours and round history omitted: prompt budget]");
    }
    ab_add(&b, ARB_FAIL_FOOTER);

    if (b.bad) {
        ab_free(&b);
        return NULL;
    }
    return b.p;
}

airy_err_t grad_llm_s1_failure(const grad_arb_fail_in_t *in, char **out_verdict,
                               void *user_data)
{
    if (!out_verdict)
        return AIRY_EINVAL;
    *out_verdict = NULL;
    if (!in)
        return AIRY_EINVAL;

    grad_llm_ctx_t *ctx = (grad_llm_ctx_t *)user_data;
    if (!ctx)
        return AIRY_EINVAL;

    /* 失败轮的 C 报告同样落盘：否则决策链在 reject 分支上缺失证据 */
    if (ctx->workspace_root && ctx->plan_id && in->c_report_json) {
        char *dir = grad_ws_dir(ctx);
        if (dir) {
            char fname[64];
            snprintf(fname, sizeof(fname), "round_%u.json", (unsigned)(in->round + 1));
            grad_ws_write_file(dir, "c_verify", fname, in->c_report_json,
                               strlen(in->c_report_json));
            AIRY_FREE(dir);
        }
    }

    char *prompt = arb_fail_prompt(in, 1);
    if (prompt && strlen(prompt) > GRAD_ARBITER_PROMPT_MAX_CHARS) {
        AIRY_FREE(prompt);
        prompt = arb_fail_prompt(in, 0);
        if (prompt)
            AIRY_LOG_WARN("GRAD-B: failure prompt over budget, neighbours/history dropped");
    }
    if (!prompt) {
        AIRY_LOG_WARN("GRAD-B: failure prompt assembly failed (OOM), deferring to Model C");
        return AIRY_SUCCESS;
    }

    char *verdict = NULL;
    size_t verdict_len = 0;
    airy_err_t err = grad_llm_call(ctx, GRAD_S1_FAILURE_SYSTEM_PROMPT, prompt,
                                   ctx->s1_verify_model, AIRY_GRAD_ROLE_B, &verdict,
                                   &verdict_len);
    AIRY_FREE(prompt);

    if (err != AIRY_SUCCESS || !verdict) {
        AIRY_LOG_WARN("GRAD-B: failure arbitration LLM unavailable (err=%d), C patch kept",
                      (int)err);
        return AIRY_SUCCESS;
    }

    if (ctx->workspace_root && ctx->plan_id) {
        grad_llm_trace_append(ctx, "b_arbiter_fail", verdict);
        char *dir = grad_ws_dir(ctx);
        if (dir) {
            char fname[64];
            snprintf(fname, sizeof(fname), "round_%u_fail.json", (unsigned)(in->round + 1));
            grad_ws_write_file(dir, "b_arbiter", fname, verdict, verdict_len);
            AIRY_FREE(dir);
        }
    }

    *out_verdict = verdict;
    return AIRY_SUCCESS;
}

airy_err_t grad_llm_round_note(void *ctx, const grad_round_note_t *note)
{
    grad_llm_ctx_t *c = (grad_llm_ctx_t *)ctx;
    if (!c || !note)
        return AIRY_EINVAL;

    /* affected_scope 紧凑呈现：最多 4 个 id，其余以 +N 归一（行长有界） */
    char scope[ARB_FAIL_SCOPE_CHARS];
    scope[0] = '\0';
    size_t used = 0;
    size_t shown = 0;
    for (size_t i = 0; i < note->scope_count && shown < ARB_FAIL_SCOPE_MAX; i++) {
        const char *id = (note->scope && note->scope[i]) ? note->scope[i] : "";
        int n = snprintf(scope + used, sizeof(scope) - used, "%s%s", shown ? "," : "", id);
        if (n < 0 || (size_t)n >= sizeof(scope) - used)
            break;
        used += (size_t)n;
        shown++;
    }
    if (note->scope_count > shown && used + 4 < sizeof(scope))
        snprintf(scope + used, sizeof(scope) - used, ",+%zu", note->scope_count - shown);

    char line[GRAD_ARBITER_HIST_LINE];
    int n = snprintf(line, sizeof(line),
                     "round %u: rejected by C [%s]; affected_scope=[%s]; arbiter=%s",
                     (unsigned)(note->round + 1),
                     (note->codes && note->codes[0]) ? note->codes : "-", scope,
                     (note->arbiter && note->arbiter[0]) ? note->arbiter : "accept");
    if (n < 0)
        return AIRY_EUNKNOWN;
    if ((size_t)n >= sizeof(line))
        line[sizeof(line) - 1] = '\0';

    if (c->a_hist_count < GRAD_ARBITER_MAX_HISTORY) {
        AIRY_MEMCPY(c->a_hist[c->a_hist_count], line, strlen(line) + 1);
        c->a_hist_count++;
        return AIRY_SUCCESS;
    }
    for (uint32_t i = 1; i < GRAD_ARBITER_MAX_HISTORY; i++)
        AIRY_MEMCPY(c->a_hist[i - 1], c->a_hist[i], GRAD_ARBITER_HIST_LINE);
    AIRY_MEMCPY(c->a_hist[GRAD_ARBITER_MAX_HISTORY - 1], line, strlen(line) + 1);
    return AIRY_SUCCESS;
}
