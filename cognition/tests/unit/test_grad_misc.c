// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_misc.c
 *   GRAD 杂项测试域：驳回补丁构造与 JSON 序列化、边界条件健壮性、
 *   决策链事件流（progress_cb）可见性、0.1.20 T3/T4 请求契约
 *   （Δ 渲染 prompt 分派 + 流式位透传）。
 */

#include "test_grad_internal.h"

/* 决策链事件流回调上下文（progress_cb，test_progress_events 使用） */
typedef struct {
    int s2_done;
    int verify_start;
    int verify_done;
    int done;
} grad_prog_ctx_t;

static void grad_prog_cb(int level, const char *event, const char *data, void *user_data)
{
    grad_prog_ctx_t *pc = (grad_prog_ctx_t *)user_data;
    (void)level;
    (void)data;
    if (!pc || !event)
        return;
    if (strcmp(event, "grad_s2_done") == 0)
        pc->s2_done++;
    else if (strcmp(event, "grad_verify_start") == 0)
        pc->verify_start++;
    else if (strcmp(event, "grad_verify_done") == 0)
        pc->verify_done++;
    else if (strcmp(event, "grad_done") == 0)
        pc->done++;
}

void test_build_patch(void)
{
    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    report.error = AIRY_GRAD_E01_CAUSAL_BREAK;
    snprintf(report.affected_node, sizeof(report.affected_node), "S_05");
    snprintf(report.missing_artifact, sizeof(report.missing_artifact), "Compiled_Binary.exe");

    char *patch = NULL;
    airy_err_t err = airy_grad_build_patch(&report, 2, &patch);
    if (err == AIRY_SUCCESS && patch && strstr(patch, "\"error_code\":\"E-01\"") &&
        strstr(patch, "\"affected_scope\":[\"S_05\"]") && strstr(patch, "Compiled_Binary.exe")) {
        TEST_PASS("E-01 rejection patch matches GRAD spec");
    } else {
        TEST_FAIL("build patch", patch ? patch : "NULL patch");
    }
    if (patch)
        AIRY_FREE(patch);

    __builtin_memset(&report, 0, sizeof(report));
    report.error = AIRY_GRAD_E02_DEADLOCK;
    snprintf(report.cycle_path, sizeof(report.cycle_path), "A -> B -> A");
    char *json = NULL;
    err = airy_grad_report_to_json(&report, &json);
    if (err == AIRY_SUCCESS && json && strstr(json, "E-02")) {
        TEST_PASS("report JSON serialization contains E-02");
    } else {
        TEST_FAIL("report JSON", json ? json : "NULL json");
    }
    if (json)
        AIRY_FREE(json);
}

void test_edge_cases(void)
{
    airy_grad_report_t report;
    __builtin_memset(&report, 0, sizeof(report));
    airy_err_t err = airy_grad_verify_plan(NULL, NULL, &report);
    if (err == AIRY_EINVAL) {
        TEST_PASS("NULL plan rejected");
    } else {
        TEST_FAIL("NULL plan", "expected EINVAL");
    }

    airy_task_plan_t *empty = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    __builtin_memset(&report, 0, sizeof(report));
    err = airy_grad_verify_plan(empty, NULL, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK) {
        TEST_PASS("empty plan passes");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected OK, got err=%d error=%d", (int)err, (int)report.error);
        TEST_FAIL("empty plan", msg);
    }
    free_plan(empty);

    const char *ig_out[] = {"s01.done"};
    airy_task_plan_t *ig = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    ig->task_plan_id = AIRY_STRDUP("ig_plan");
    ig->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    ig->task_plan_nodes[ig->task_plan_node_count++] =
        make_node("S_01", "guard", "executor", NULL, 0, ig_out, 1, 10, 1);
    __builtin_memset(&report, 0, sizeof(report));
    airy_grad_budget_t ig_budget = {600000, 1024};
    err = airy_grad_verify_plan(ig, &ig_budget, &report);
    if (err == AIRY_SUCCESS && report.error == AIRY_GRAD_OK &&
        report.guard_state == AIRY_GRAD_GUARD_NONE && report.invariant_guards == 1) {
        TEST_PASS("invariant_guard counted for semantic review");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected 1 guard, got %u (err=%d)",
                 (unsigned)report.invariant_guards, (int)err);
        TEST_FAIL("invariant_guard", msg);
    }
    free_plan(ig);
}

/* B1-e 分角色计量 fake：逐次递增 usage（seq 驱动），静态响应免释放
 * （测试二进制未注入 LLM ops，are_ops_get_llm()==NULL 约定，先例见
 * test_cog_review_strategy.c）。 */
typedef struct {
    int seq;
} ru_fake_t;

static llm_response_t g_ru_resp;
static llm_message_t g_ru_choices[1];

static int ru_fake_complete(void *ctx, const llm_request_config_t *cfg, llm_response_t **out)
{
    ru_fake_t *f = (ru_fake_t *)ctx;
    (void)cfg;
    f->seq++;
    g_ru_choices[0].role = "assistant";
    g_ru_choices[0].content = "ok";
    __builtin_memset(&g_ru_resp, 0, sizeof(g_ru_resp));
    g_ru_resp.choices = g_ru_choices;
    g_ru_resp.choice_count = 1;
    g_ru_resp.finish_reason = "stop"; /* C11：自然终结方为合法输出 */
    g_ru_resp.prompt_tokens = (uint32_t)(10 * f->seq);
    g_ru_resp.completion_tokens = (uint32_t)f->seq;
    g_ru_resp.total_tokens = (uint32_t)(11 * f->seq);
    *out = &g_ru_resp;
    return 0;
}

static void ru_call(grad_llm_ctx_t *ctx, airy_grad_role_e role)
{
    char *text = NULL;
    size_t len = 0;
    airy_err_t err = grad_llm_call(ctx, "sys", "usr", "model", role, &text, &len);
    if (err == AIRY_SUCCESS && text)
        AIRY_FREE(text);
    else
        TEST_FAIL("role usage call", "grad_llm_call failed");
}

/* C11 契约 fake：捕获最近一次请求配置，finish_reason 可编程。 */
typedef struct {
    const llm_request_config_t *last_cfg;
    const char *finish_reason;
} c11_fake_t;

static llm_response_t g_c11_resp;
static llm_message_t g_c11_choices[1];

static int c11_fake_complete(void *ctx, const llm_request_config_t *cfg, llm_response_t **out)
{
    c11_fake_t *f = (c11_fake_t *)ctx;
    f->last_cfg = cfg;
    g_c11_choices[0].role = "assistant";
    g_c11_choices[0].content = "{\"ok\":true}";
    __builtin_memset(&g_c11_resp, 0, sizeof(g_c11_resp));
    g_c11_resp.choices = g_c11_choices;
    g_c11_resp.choice_count = 1;
    /* fake 只读借用串：llm_response_t 不拥有 finish_reason，边界处显式转移 */
    g_c11_resp.finish_reason = (char *)f->finish_reason;
    *out = &g_c11_resp;
    return 0;
}

void test_grad_c11_contract(void)
{
    grad_llm_ctx_t ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx));
    c11_fake_t fake = {0};
    ctx.complete = c11_fake_complete;
    ctx.complete_ctx = &fake;

    char *text = NULL;
    size_t len = 0;
    char msg[160];

    /* 采样契约：温度 0 显式下发（temperature_set 哨兵）+ 固定非零种子。 */
    fake.finish_reason = "stop";
    airy_err_t err = grad_llm_call(&ctx, "sys", "usr", "model", AIRY_GRAD_ROLE_A, &text, &len);
    if (err != AIRY_SUCCESS || !text) {
        TEST_FAIL("c11 sampling", "stop call failed");
        return;
    }
    AIRY_FREE(text);
    if (fake.last_cfg->temperature == 0.0f && fake.last_cfg->temperature_set &&
        fake.last_cfg->seed == GRAD_SEED) {
        TEST_PASS("temp=0 explicit + fixed seed");
    } else {
        snprintf(msg, sizeof(msg), "temp=%.2f set=%d seed=%u", fake.last_cfg->temperature,
                 (int)fake.last_cfg->temperature_set, fake.last_cfg->seed);
        TEST_FAIL("c11 sampling", msg);
    }

    /* 完整性契约：缺终结/截断/过滤一律 ESERVICE，半截输出不外泄。 */
    static const char *bad[] = {NULL, "length", "content_filter"};
    static const char *bad_name[] = {"absent", "truncated", "content_filtered"};
    for (int i = 0; i < 3; i++) {
        fake.finish_reason = bad[i];
        text = NULL;
        len = 0;
        err = grad_llm_call(&ctx, "sys", "usr", "model", AIRY_GRAD_ROLE_A, &text, &len);
        if (err == AIRY_ESERVICE && !text)
            TEST_PASS(bad_name[i]);
        else {
            snprintf(msg, sizeof(msg), "err=%d text=%p", (int)err, (void *)text);
            TEST_FAIL(bad_name[i], msg);
        }
    }
}

void test_grad_role_usage(void)
{
    grad_llm_ctx_t ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx));
    ru_fake_t fake = {0};
    ctx.complete = ru_fake_complete;
    ctx.complete_ctx = &fake;

    ru_call(&ctx, AIRY_GRAD_ROLE_A); /* +10/+1/+11 */
    ru_call(&ctx, AIRY_GRAD_ROLE_B); /* +20/+2/+22 */
    ru_call(&ctx, AIRY_GRAD_ROLE_A); /* +30/+3/+33 */

    char msg[160];
    if (ctx.usage[AIRY_GRAD_ROLE_A].prompt == 40 && ctx.usage[AIRY_GRAD_ROLE_A].completion == 4 &&
        ctx.usage[AIRY_GRAD_ROLE_A].total == 44 && ctx.usage[AIRY_GRAD_ROLE_B].prompt == 20 &&
        ctx.usage[AIRY_GRAD_ROLE_B].completion == 2 && ctx.usage[AIRY_GRAD_ROLE_B].total == 22) {
        TEST_PASS("A/B role token usage isolated and accumulated");
    } else {
        snprintf(msg, sizeof(msg), "A={%u,%u,%u} B={%u,%u,%u}", ctx.usage[AIRY_GRAD_ROLE_A].prompt,
                 ctx.usage[AIRY_GRAD_ROLE_A].completion, ctx.usage[AIRY_GRAD_ROLE_A].total,
                 ctx.usage[AIRY_GRAD_ROLE_B].prompt, ctx.usage[AIRY_GRAD_ROLE_B].completion,
                 ctx.usage[AIRY_GRAD_ROLE_B].total);
        TEST_FAIL("role usage isolation", msg);
    }
}

/* T3/T4 契约 fake：在 complete 调用窗口内捕获请求要素（llm_cfg 为
 * grad_llm_call 栈变量，complete 返回后即失效，禁止事后回读），
 * 返回最小合法 plan JSON 保证 s2_plan 走真实解析路径。 */
typedef struct {
    int stream;
    char user_prompt[4096];
    int truncated;
} t34_fake_t;

static llm_response_t g_t34_resp;
static llm_message_t g_t34_choices[1];

static int t34_fake_complete(void *ctx, const llm_request_config_t *cfg, llm_response_t **out)
{
    t34_fake_t *f = (t34_fake_t *)ctx;
    f->stream = cfg->stream;
    f->user_prompt[0] = '\0';
    f->truncated = 0;
    if (cfg->message_count >= 2 && cfg->messages[1].content) {
        size_t cap = sizeof(f->user_prompt);
        size_t need = strlen(cfg->messages[1].content);
        size_t take = need >= cap ? cap - 1 : need;
        __builtin_memcpy(f->user_prompt, cfg->messages[1].content, take);
        f->user_prompt[take] = '\0';
        f->truncated = (take != need);
    }
    g_t34_choices[0].role = "assistant";
    g_t34_choices[0].content =
        "{\"nodes\":[{\"id\":\"S_01\",\"goal\":\"fix\",\"role\":\"act\",\"depends\":[]}],"
        "\"entry\":[\"S_01\"]}";
    __builtin_memset(&g_t34_resp, 0, sizeof(g_t34_resp));
    g_t34_resp.choices = g_t34_choices;
    g_t34_resp.choice_count = 1;
    g_t34_resp.finish_reason = (char *)"stop";
    *out = &g_t34_resp;
    return 0;
}

void test_grad_t3_patch_prompt(void)
{
    static const char *PATCH =
        "{\"patch_id\":\"p1\",\"source\":\"C_Model\",\"error_code\":\"E-01\","
        "\"affected_scope\":[\"S_01\"],\"missing_artifact\":\"x\",\"suggestion\":\"fix\"}";
    char msg[160];

    /* ON：驳回轮 prompt 走 Δ 渲染契约（T3 结构优先，G61 主手段）——
     * 只回 affected_scope 子集，禁止全量重绘文案。 */
    grad_llm_ctx_t ctx;
    t34_fake_t fake;
    __builtin_memset(&ctx, 0, sizeof(ctx));
    __builtin_memset(&fake, 0, sizeof(fake));
    ctx.complete = t34_fake_complete;
    ctx.complete_ctx = &fake;
    ctx.plan_id = "t3_test";
    ctx.incremental_patch = 1u;

    airy_task_plan_t *plan = NULL;
    airy_err_t err = grad_llm_s2_plan(NULL, PATCH, &plan, &ctx);
    TEST_CHECK(err == AIRY_SUCCESS && plan, "incremental s2_plan failed");
    free_plan(plan);
    if (fake.truncated) {
        TEST_FAIL("t3 delta prompt", "captured prompt truncated");
        return;
    }
    if (strstr(fake.user_prompt, "Output ONLY a patch plan JSON") &&
        strstr(fake.user_prompt, "affected_scope") &&
        !strstr(fake.user_prompt, "Generate the FULL corrected plan")) {
        TEST_PASS("delta render contract on");
    } else {
        snprintf(msg, sizeof(msg), "prompt head: %.100s", fake.user_prompt);
        TEST_FAIL("t3 delta prompt", msg);
    }

    /* OFF：回退全量重绘文案（回退开关路径不得为死代码）。 */
    __builtin_memset(&ctx, 0, sizeof(ctx));
    __builtin_memset(&fake, 0, sizeof(fake));
    ctx.complete = t34_fake_complete;
    ctx.complete_ctx = &fake;
    ctx.plan_id = "t3_test";

    plan = NULL;
    err = grad_llm_s2_plan(NULL, PATCH, &plan, &ctx);
    TEST_CHECK(err == AIRY_SUCCESS && plan, "full s2_plan failed");
    free_plan(plan);
    if (strstr(fake.user_prompt, "Generate the FULL corrected plan") &&
        !strstr(fake.user_prompt, "Output ONLY a patch plan JSON")) {
        TEST_PASS("full redraw fallback off");
    } else {
        TEST_FAIL("t3 full fallback", "unexpected prompt contract");
    }
}

void test_grad_t4_stream(void)
{
    grad_llm_ctx_t ctx;
    t34_fake_t fake;
    char *text = NULL;
    size_t len = 0;

    /* ON：ctx->stream 透传为请求流式位（chunk 消费面在机制闭包）。 */
    __builtin_memset(&ctx, 0, sizeof(ctx));
    __builtin_memset(&fake, 0, sizeof(fake));
    ctx.complete = t34_fake_complete;
    ctx.complete_ctx = &fake;
    ctx.stream = 1;

    airy_err_t err = grad_llm_call(&ctx, "sys", "usr", "model", AIRY_GRAD_ROLE_A, &text, &len);
    TEST_CHECK(err == AIRY_SUCCESS && text, "stream call failed");
    AIRY_FREE(text);
    if (fake.stream == 1) {
        TEST_PASS("stream bit passed through");
    } else {
        TEST_FAIL("t4 stream", "expected stream=1");
    }

    /* OFF：缺省安全（0 缺省），非流式路径字节级不变。 */
    __builtin_memset(&fake, 0, sizeof(fake));
    ctx.stream = 0;
    text = NULL;
    len = 0;
    err = grad_llm_call(&ctx, "sys", "usr", "model", AIRY_GRAD_ROLE_A, &text, &len);
    TEST_CHECK(err == AIRY_SUCCESS && text, "non-stream call failed");
    AIRY_FREE(text);
    if (fake.stream == 0) {
        TEST_PASS("stream off stays zero");
    } else {
        TEST_FAIL("t4 stream off", "expected stream=0");
    }
}

void test_progress_events(void)
{
    /* C5 any 语义：seed 节点须元数据完备，否则 verify 门 E-05 恒拒，
     * done 事件永不触发。 */
    const char *seed_out[] = {"s01.done"};
    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    seed->task_plan_id = AIRY_STRDUP("seed");
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    seed->task_plan_nodes[seed->task_plan_node_count++] =
        make_node("S_01", "execute", "executor", NULL, 0, seed_out, 1, 10, 0);
    seed->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (seed->task_plan_entry_points) {
        seed->task_plan_entry_points[0] = AIRY_STRDUP("S_01");
        seed->task_plan_entry_count = 1;
    }

    grad_prog_ctx_t pc;
    __builtin_memset(&pc, 0, sizeof(pc));

    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.s2_plan = grad_mock_s2;
    cfg.progress_cb = grad_prog_cb;
    cfg.progress_user_data = &pc;

    airy_grad_coordinator_t *coord = NULL;
    airy_err_t err = airy_grad_coordinator_create(&cfg, &coord);
    if (err != AIRY_SUCCESS || !coord) {
        TEST_FAIL("progress events", "create failed");
        free_plan(seed);
        return;
    }

    airy_task_plan_t *final = NULL;
    airy_grad_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    err = airy_grad_coordinator_execute(coord, NULL, seed, &final, &stats);

    char msg[192];
    if (err == AIRY_SUCCESS && pc.s2_done >= 1 && pc.verify_start >= 1 && pc.verify_done >= 1 &&
        pc.done >= 1) {
        snprintf(msg, sizeof(msg), "s2=%d vstart=%d vdone=%d done=%d", pc.s2_done,
                 pc.verify_start, pc.verify_done, pc.done);
        TEST_PASS("progress events fired");
        printf("        %s\n", msg);
    } else {
        snprintf(msg, sizeof(msg), "err=%d s2=%d vstart=%d vdone=%d done=%d", (int)err, pc.s2_done,
                 pc.verify_start, pc.verify_done, pc.done);
        TEST_FAIL("progress events", msg);
    }
    airy_grad_coordinator_destroy(coord);
    free_plan(seed);
}
