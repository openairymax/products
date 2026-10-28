// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_run.c
 *   grad_run() 生产入口端到端终态测试（0.1.20 P0-7 / C7）
 *
 * 验证目标（推理链强化详细设计 §4.9 四路径 + G45 验收）：
 *   1. 完整批判环：seed 零驳回过门 → converged；E-05 驳回后修复收敛
 *      → repaired；
 *   2. 轻载档（lightweight=1）：仅 C 确定性四验、零生成 token →
 *      lightweight（不得并入 converged）；
 *   3. LLM 出口缺席 → no_llm（降级显式申报，不再是零值静默返回）；
 *   4. B 缺席（C11 残缺输出）→ b_unavailable；B 裁决契约违例 → b_invalid
 *      （计划按 C 放行，但语义复核缺失不得记为 converged）；
 *   5. 未收敛（J-16 无增益停止 / E-02 环计划）→ fail_closed，计划不放行；
 *   6. 降级产物（fallback_ 前缀计划）→ fail_closed + degraded。
 *
 * 全部用例经 grad_run() 生产入口装配（真协调器 + 真 LLM 调用域），
 * 仅 LLM 补全闭包被替身为确定性 fake——不绕过任何被验证逻辑。
 */

#include "test_grad_internal.h"
#include "grad_strategy.h"

/* B 终裁：高置信 accept（θ_B 之上，直接采纳）。 */
#define GR_ACC "{\"verdict\":\"accept\",\"confidence\":0.95}"
/* B 终裁：非法 verdict 值 → 契约违例（b_invalid 路径）。 */
#define GR_BAD "{\"verdict\":\"maybe\",\"confidence\":0.9}"
/* A 计划：单节点元数据完备（过 E-01/E-03/E-05）。 */
#define GR_PLAN_OK                                                              \
    "{\"nodes\":[{\"id\":\"S_01\",\"goal\":\"execute\",\"role\":\"act\","       \
    "\"outputs\":[\"seed.done\"],\"cost_time_ms\":10}],\"entry\":[\"S_01\"]}"
/* A 计划：两节点合法拓扑但 inputs 引用了前驱未产出的工件 → E-01 恒拒。 */
#define GR_PLAN_E01                                                             \
    "{\"nodes\":["                                                              \
    "{\"id\":\"S_01\",\"goal\":\"collect\",\"role\":\"retrieve\","             \
    "\"outputs\":[\"doc\"],\"cost_time_ms\":10},"                               \
    "{\"id\":\"S_02\",\"goal\":\"write\",\"role\":\"act\",\"depends\":[\"S_01\"]," \
    "\"inputs\":[\"S_01.ghost\"],\"outputs\":[\"report\"],\"cost_time_ms\":10}" \
    "],\"entry\":[\"S_01\"]}"

/* LLM 补全闭包替身：按 system prompt 分流 A/B，返回预置 JSON 与终结因。 */
typedef struct {
    int a_calls;
    int b_calls;
    const char *a_json;
    const char *b_json;
    const char *a_fin;
    const char *b_fin;
} gr_fake_t;

static int gr_fake_cb(void *ctx, const llm_request_config_t *cfg, llm_response_t **out)
{
    static llm_message_t ch[1];
    static llm_response_t rsp;
    gr_fake_t *f = (gr_fake_t *)ctx;
    if (!f || !cfg || !out || cfg->message_count < 2 || !cfg->messages[0].content)
        return -1;

    const int is_b = strstr(cfg->messages[0].content, "(Model B)") != NULL;
    const char *body = is_b ? f->b_json : f->a_json;
    const char *fin = is_b ? f->b_fin : f->a_fin;
    if (is_b)
        f->b_calls++;
    else
        f->a_calls++;
    if (!body)
        return -1;

    __builtin_memset(&rsp, 0, sizeof(rsp));
    __builtin_memset(ch, 0, sizeof(ch));
    ch[0].role = "assistant";
    ch[0].content = body;
    rsp.choices = ch;
    rsp.choice_count = 1;
    /* fake 只读借用串：llm_response_t 不拥有 finish_reason。 */
    rsp.finish_reason = (char *)(fin ? fin : "stop");
    rsp.prompt_tokens = 10;
    rsp.completion_tokens = 5;
    rsp.total_tokens = 15;
    *out = &rsp;
    return 0;
}

static void gr_launch(airy_grad_launch_t *l, gr_fake_t *f, int light)
{
    __builtin_memset(l, 0, sizeof(*l));
    l->complete = gr_fake_cb;
    l->complete_ctx = f;
    l->input = "make report";
    l->lightweight = light;
}

/* 生产入口统一装配：workspace/plan_id 留空 → 决策链 trace 零 I/O。 */
static airy_err_t gr_exec(gr_fake_t *f, int light, airy_task_plan_t *seed,
                          airy_grad_outcome_t *out)
{
    airy_grad_launch_t l;
    gr_launch(&l, f, light);
    __builtin_memset(out, 0, sizeof(*out));
    return grad_run(&l, seed, out);
}

/* 裸 seed：无 outputs / 零 cost → E-05 元数据不足恒拒。 */
static airy_task_plan_t *gr_bare_seed(const char *id)
{
    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!seed)
        return NULL;
    seed->task_plan_id = AIRY_STRDUP("seed_bare");
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    seed->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (!seed->task_plan_nodes || !seed->task_plan_entry_points) {
        free_plan(seed);
        return NULL;
    }
    seed->task_plan_nodes[seed->task_plan_node_count++] =
        make_node(id, "execute", "executor", NULL, 0, NULL, 0, 0, 0);
    seed->task_plan_entry_points[0] = AIRY_STRDUP(id);
    seed->task_plan_entry_count = 1;
    return seed;
}

/* 环 seed：A↔B 互依赖 → E-02 死锁（安全守门，轻载档同样生效）。 */
static airy_task_plan_t *gr_cyc_seed(void)
{
    static const char *ao[] = {"a.out"};
    static const char *bo[] = {"b.out"};
    airy_task_plan_t *seed = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!seed)
        return NULL;
    seed->task_plan_id = AIRY_STRDUP("seed_cyc");
    seed->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(2, sizeof(airy_task_node_t *));
    seed->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (!seed->task_plan_nodes || !seed->task_plan_entry_points) {
        free_plan(seed);
        return NULL;
    }
    airy_task_node_t *a = make_node("A", "a", "executor", NULL, 0, ao, 1, 10, 0);
    airy_task_node_t *b = make_node("B", "b", "executor", NULL, 0, bo, 1, 10, 0);
    add_dep(a, "B");
    add_dep(b, "A");
    seed->task_plan_nodes[seed->task_plan_node_count++] = a;
    seed->task_plan_nodes[seed->task_plan_node_count++] = b;
    seed->task_plan_entry_points[0] = AIRY_STRDUP("A");
    seed->task_plan_entry_count = 1;
    return seed;
}

/* 路径 1：seed 首轮过门，B 高置信采纳 → converged（零驳回、零 A 调用）。 */
void test_run_conv(void)
{
    airy_task_plan_t *seed = make_seed_single("S_01", "seed_conv");
    TEST_CHECK(seed != NULL, "seed alloc failed");
    gr_fake_t f = {.b_json = GR_ACC};
    airy_grad_outcome_t oc;
    airy_err_t err = gr_exec(&f, 0, seed, &oc);
    TEST_CHECK(err == AIRY_SUCCESS, "converged run returns success");
    TEST_CHECK(oc.converged == 1, "converged flag set");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_CONVERGED, "status must be converged");
    TEST_CHECK(oc.stats.rejections == 0, "zero rejections expected");
    TEST_CHECK(f.a_calls == 0, "model A must not be called");
    TEST_CHECK(f.b_calls == 1, "model B must arbitrate once");
    TEST_CHECK(oc.final_plan && oc.final_plan != seed, "final is a deep copy");
    if (oc.final_plan)
        free_plan(oc.final_plan);
    free_plan(seed);
    TEST_PASS("seed 首轮过门申报 converged");
}

/* 路径 2：E-05 驳回 → A 重生成合规计划 → repaired（有驳回但验证链完整）。 */
void test_run_fix(void)
{
    airy_task_plan_t *seed = gr_bare_seed("S_01");
    TEST_CHECK(seed != NULL, "bare seed alloc failed");
    gr_fake_t f = {.a_json = GR_PLAN_OK, .b_json = GR_ACC};
    airy_grad_outcome_t oc;
    airy_err_t err = gr_exec(&f, 0, seed, &oc);
    TEST_CHECK(err == AIRY_SUCCESS, "repaired run returns success");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_REPAIRED, "status must be repaired");
    TEST_CHECK(oc.stats.rejections == 1, "one rejection expected");
    TEST_CHECK(oc.stats.e05_count >= 1, "E-05 must be counted");
    TEST_CHECK(f.a_calls == 1, "model A regenerates once");
    TEST_CHECK(f.b_calls == 1, "model B arbitrates once");
    TEST_CHECK(oc.final_plan != NULL, "repaired plan must be released");
    if (oc.final_plan)
        free_plan(oc.final_plan);
    free_plan(seed);
    TEST_PASS("驳回修复收敛申报 repaired");
}

/* 路径 3：A 恒产出 E-01 断裂计划 → J-16 无增益停止 → fail_closed。 */
void test_run_closed(void)
{
    gr_fake_t f = {.a_json = GR_PLAN_E01, .b_json = GR_ACC};
    airy_grad_outcome_t oc;
    airy_err_t err = gr_exec(&f, 0, NULL, &oc);
    TEST_CHECK(err != AIRY_SUCCESS, "non-converged must not return success");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_FAIL_CLOSED, "status must be fail_closed");
    TEST_CHECK(oc.final_plan == NULL, "defective plan must not be released");
    TEST_CHECK(oc.stats.stopped == 1, "J-16 no-progress stop expected");
    TEST_CHECK(f.a_calls == 2, "skeleton + one regeneration");
    TEST_CHECK(f.b_calls == 0, "E-01 non-guard must not call B");
    TEST_PASS("无增益停止申报 fail_closed");
}

/* 路径 4：降级产物（fallback_ 前缀）检出 → fail_closed + degraded。 */
void test_run_fb(void)
{
    airy_task_plan_t *seed = make_seed_single("S_01", "fallback_seed");
    TEST_CHECK(seed != NULL, "seed alloc failed");
    gr_fake_t f = {.b_json = GR_ACC};
    airy_grad_outcome_t oc;
    airy_err_t err = gr_exec(&f, 0, seed, &oc);
    TEST_CHECK(err != AIRY_SUCCESS, "fallback product must not succeed");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_FAIL_CLOSED, "status must be fail_closed");
    TEST_CHECK(oc.degraded == 1, "degraded flag must be set");
    TEST_CHECK(oc.final_plan == NULL, "fallback plan must be dropped");
    TEST_CHECK(f.a_calls == 0, "model A must not be called");
    TEST_CHECK(f.b_calls == 1, "model B must arbitrate once");
    TEST_PASS("fallback 产物申报 fail_closed");
}

/* 路径 5：轻载档 seed 过门 → lightweight（B 缺席、A 零调用）。 */
void test_run_light(void)
{
    airy_task_plan_t *seed = make_seed_single("S_01", "seed_light");
    TEST_CHECK(seed != NULL, "seed alloc failed");
    gr_fake_t f = {.b_json = GR_ACC};
    airy_grad_outcome_t oc;
    airy_err_t err = gr_exec(&f, 1, seed, &oc);
    TEST_CHECK(err == AIRY_SUCCESS, "lightweight run passes deterministic checks");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_LIGHTWEIGHT, "status must be lightweight");
    TEST_CHECK(f.a_calls == 0, "lightweight must not spend generation tokens");
    TEST_CHECK(f.b_calls == 0, "lightweight must skip semantic arbitration");
    TEST_CHECK(oc.final_plan != NULL, "verified seed must be released");
    if (oc.final_plan)
        free_plan(oc.final_plan);
    free_plan(seed);
    TEST_PASS("轻载档申报 lightweight");
}

/* 路径 6：轻载档遇 E-02 环计划 → fail_closed 且零生成（安全守门生效）。 */
void test_run_lcyc(void)
{
    airy_task_plan_t *seed = gr_cyc_seed();
    TEST_CHECK(seed != NULL, "cycle seed alloc failed");
    gr_fake_t f = {.a_json = GR_PLAN_OK};
    airy_grad_outcome_t oc;
    airy_err_t err = gr_exec(&f, 1, seed, &oc);
    TEST_CHECK(err != AIRY_SUCCESS, "cyclic plan must not converge");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_FAIL_CLOSED, "status must be fail_closed");
    TEST_CHECK(oc.final_plan == NULL, "cyclic plan must not be released");
    TEST_CHECK(f.a_calls == 0, "single round must not regenerate");
    free_plan(seed);
    TEST_PASS("轻载档环检测仍 fail-closed");
}

/* 路径 7：LLM 出口缺席 → no_llm（显式降级，不跑批判环）。 */
void test_run_nollm(void)
{
    airy_task_plan_t *seed = make_seed_single("S_01", "seed_nollm");
    TEST_CHECK(seed != NULL, "seed alloc failed");
    airy_grad_launch_t l;
    gr_launch(&l, NULL, 0);
    l.complete = NULL;
    airy_grad_outcome_t oc;
    __builtin_memset(&oc, 0, sizeof(oc));
    airy_err_t err = grad_run(&l, seed, &oc);
    TEST_CHECK(err != AIRY_SUCCESS, "absent LLM must not converge");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_NO_LLM, "status must be no_llm");
    TEST_CHECK(oc.degraded == 1, "degraded flag must be set");
    TEST_CHECK(oc.final_plan == NULL, "no plan may be released");
    free_plan(seed);
    TEST_PASS("LLM 缺席申报 no_llm");
}

/* 路径 8：B 输出被截断（C11）→ 缺席 → b_unavailable（C 判定仍放行）。 */
void test_run_bgone(void)
{
    airy_task_plan_t *seed = make_seed_single("S_01", "seed_bgone");
    TEST_CHECK(seed != NULL, "seed alloc failed");
    gr_fake_t f = {.b_json = GR_ACC, .b_fin = "length"};
    airy_grad_outcome_t oc;
    airy_err_t err = gr_exec(&f, 0, seed, &oc);
    TEST_CHECK(err == AIRY_SUCCESS, "C verdict still releases the plan");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_B_UNAVAILABLE, "status must be b_unavailable");
    TEST_CHECK(oc.stats.arbiter_unavailable == 1, "B absence must be counted");
    TEST_CHECK(f.b_calls == 1, "model B attempted once");
    TEST_CHECK(oc.final_plan != NULL, "plan released under C verdict");
    if (oc.final_plan)
        free_plan(oc.final_plan);
    free_plan(seed);
    TEST_PASS("B 缺席申报 b_unavailable");
}

/* 路径 9：B 裁决契约违例 → b_invalid（不采纳其结论，不得记 converged）。 */
void test_run_bbad(void)
{
    airy_task_plan_t *seed = make_seed_single("S_01", "seed_bbad");
    TEST_CHECK(seed != NULL, "seed alloc failed");
    gr_fake_t f = {.b_json = GR_BAD};
    airy_grad_outcome_t oc;
    airy_err_t err = gr_exec(&f, 0, seed, &oc);
    TEST_CHECK(err == AIRY_SUCCESS, "C verdict still releases the plan");
    TEST_CHECK(oc.stats.status == GRAD_STATUS_B_INVALID, "status must be b_invalid");
    TEST_CHECK(oc.stats.arbiter_invalid == 1, "B contract violation must be counted");
    TEST_CHECK(f.b_calls == 1, "model B attempted once");
    TEST_CHECK(oc.final_plan != NULL, "plan released under C verdict");
    if (oc.final_plan)
        free_plan(oc.final_plan);
    free_plan(seed);
    TEST_PASS("B 违例申报 b_invalid");
}
