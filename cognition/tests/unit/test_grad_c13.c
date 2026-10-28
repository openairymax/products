// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_c13.c
 *   GRAD C13 失败路径 B 定向仲裁测试域（0.1.20 §5.2b.11 / §5.4 G56）。
 *
 * 覆盖：
 *   ① E-04 失败必须调用 B 一次，且权威七字段完备（token_budget > 0）；
 *   ② B 超时 / 非 JSON → 回退 C 判定，记 arbiter_internal_error，不阻塞；
 *   ③ salvageable=true 但 scope 空 → patch_invalid 回退全量重绘；
 *   ④ arbiter_on_failure=off → B 零调用；
 *   ⑤ 失败路径预算不足 → 跳过仲裁（arbiter_skipped_budget）；
 *   P1-11 反向用例：LLM 不可用（complete=NULL）→ 真实 s1_failure 与
 *   主路径 s1_arbiter 均 fail-closed 显式上报，verdict 不外泄；
 *   J-16 连续两轮缺陷码与范围相同 → 无进展 fail-closed 停止；
 *   黄金集（tests/grad/golden）：wanted 15 例 + parse 17 例，假阳性率 ≤5%。
 */

#include "test_grad_internal.h"

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

#define C13_EV_MAX 64
#define C13_EV_LEN 48

/* 进度事件采集（含可控一次性睡眠，用于预算跳过用例的确定性） */
typedef struct {
    char ev[C13_EV_MAX][C13_EV_LEN];
    int n;
    int invalid;
    int skip_ev;
    int err_ev;
    uint32_t sleep_ms;
    int slept;
} c13_evt_t;

/* B 失败路径调用记录 */
typedef struct {
    const char *verdict; /* NULL = 放弃豁免（按 C 判据继续） */
    airy_err_t err;
    int calls;
    int fields_ok;
    uint32_t round;
    int64_t token_budget;
} c13_bctx_t;

typedef struct {
    grad_mock_ctx_t s2;
    c13_bctx_t b;
    c13_evt_t ev;
    airy_gccp_goal_t goal;
    char ep[64];
    airy_grad_stats_t stats;
    airy_err_t err;
    airy_task_plan_t *final;
} c13_rig_t;

typedef struct {
    grad_s2_plan_fn s2;
    uint32_t policy;
    int64_t time_ms; /* 0 = 默认预算 */
} c13_run_cfg_t;

static airy_err_t c13_mock_fail(const grad_arb_fail_in_t *in, char **out_verdict, void *user_data)
{
    c13_bctx_t *b = (c13_bctx_t *)user_data;
    if (!b || !in || !out_verdict)
        return AIRY_EINVAL;
    b->calls++;
    b->round = in->round;
    b->token_budget = in->token_budget;
    b->fields_ok = in->goal_json && in->goal_json[0] && in->c_report_json &&
                   in->c_report_json[0] && in->failed_node_json && in->failed_node_json[0] &&
                   in->neighbors_json && in->neighbors_json[0] && in->round_history_json &&
                   in->round_history_json[0] && in->token_budget > 0;
    if (b->err != AIRY_SUCCESS)
        return b->err;
    if (!b->verdict)
        return AIRY_SUCCESS;
    *out_verdict = AIRY_STRDUP(b->verdict);
    return *out_verdict ? AIRY_SUCCESS : AIRY_ENOMEM;
}

static void c13_progress(int level, const char *event, const char *data, void *user_data)
{
    c13_evt_t *e = (c13_evt_t *)user_data;
    (void)level;
    (void)data;
    if (!e || !event)
        return;
    if (e->n < C13_EV_MAX)
        snprintf(e->ev[e->n++], C13_EV_LEN, "%s", event);
    if (strcmp(event, "grad_patch_invalid") == 0)
        e->invalid++;
    else if (strcmp(event, "grad_arbiter_failure_skip") == 0)
        e->skip_ev++;
    else if (strcmp(event, "grad_arbiter_failure_error") == 0)
        e->err_ev++;
    if (!e->slept && e->sleep_ms > 0 && strcmp(event, "grad_verify_done") == 0) {
        e->slept = 1;
        (void)airy_time_sleep_ms(e->sleep_ms);
    }
}

static void c13_rig_init(c13_rig_t *r)
{
    __builtin_memset(r, 0, sizeof(*r));
    snprintf(r->ep, sizeof(r->ep), "%s", "deploy the web service");
    r->goal.goal_endpoint = r->ep;
    r->b.verdict = "{\"verdict\":\"accept\"}";
}

static void c13_rig_run(c13_rig_t *r, const c13_run_cfg_t *rc, airy_task_plan_t *seed)
{
    airy_grad_config_t cfg = AIRY_GRAD_CONFIG_DEFAULTS;
    cfg.max_iterations = 3;
    cfg.s2_plan = rc->s2;
    cfg.s2_user_data = &r->s2;
    cfg.s1_arbiter = NULL; /* 仅失败路径：s1_user_data 归 s1_failure */
    cfg.s1_user_data = &r->b;
    cfg.s1_failure = c13_mock_fail;
    cfg.arbiter_on_failure = rc->policy;
    cfg.progress_cb = c13_progress;
    cfg.progress_user_data = &r->ev;
    if (rc->time_ms > 0)
        cfg.budget.time_ms = rc->time_ms;

    airy_grad_coordinator_t *coord = NULL;
    r->err = airy_grad_coordinator_create(&cfg, &coord);
    if (r->err != AIRY_SUCCESS)
        return;
    r->err = airy_grad_coordinator_execute(coord, &r->goal, seed, &r->final, &r->stats);
    airy_grad_coordinator_destroy(coord);
}

static airy_task_plan_t *c13_new_plan(const char *id)
{
    airy_task_plan_t *p = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!p)
        return NULL;
    p->task_plan_id = AIRY_STRDUP(id);
    p->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(3, sizeof(airy_task_node_t *));
    if (!p->task_plan_id || !p->task_plan_nodes) {
        free_plan(p);
        return NULL;
    }
    return p;
}

static void c13_entry(airy_task_plan_t *p, const char *id)
{
    p->task_plan_entry_points = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (!p->task_plan_entry_points)
        return;
    p->task_plan_entry_points[0] = AIRY_STRDUP(id);
    p->task_plan_entry_count = p->task_plan_entry_points[0] ? 1 : 0;
}

/* chain3：N1 → N2 → N3（无 guard，E-01/E-02/E-03/E-05 全过） */
static airy_task_plan_t *c13_chain3(int64_t cost)
{
    const char *n1o[] = {"alpha"};
    const char *n2i[] = {"N1.alpha"};
    const char *n2o[] = {"beta"};
    const char *n3i[] = {"N2.beta"};
    const char *n3o[] = {"gamma"};
    airy_task_plan_t *p = c13_new_plan("c13_chain3");
    if (!p)
        return NULL;
    airy_task_node_t *n1 = make_node("N1", "produce alpha", "executor", NULL, 0, n1o, 1, cost, 0);
    airy_task_node_t *n2 = make_node("N2", "produce beta", "executor", n2i, 1, n2o, 1, cost, 0);
    airy_task_node_t *n3 = make_node("N3", "produce gamma", "executor", n3i, 1, n3o, 1, cost, 0);
    if (!n1 || !n2 || !n3) {
        free_plan(p);
        return NULL;
    }
    add_dep(n2, "N1");
    add_dep(n3, "N2");
    p->task_plan_nodes[p->task_plan_node_count++] = n1;
    p->task_plan_nodes[p->task_plan_node_count++] = n2;
    p->task_plan_nodes[p->task_plan_node_count++] = n3;
    c13_entry(p, "N1");
    return p;
}

/* guard3：N1 → G1(guard) → N3；G1 outputs 与 goal 关键词零交集 → E-04 */
static airy_task_plan_t *c13_guard3(int64_t cost)
{
    const char *n1o[] = {"alpha"};
    const char *gi[] = {"N1.alpha"};
    const char *go[] = {"flux"};
    const char *n3i[] = {"G1.flux"};
    const char *n3o[] = {"gamma"};
    airy_task_plan_t *p = c13_new_plan("c13_guard3");
    if (!p)
        return NULL;
    airy_task_node_t *n1 = make_node("N1", "produce alpha", "executor", NULL, 0, n1o, 1, cost, 0);
    airy_task_node_t *g1 = make_node("G1", "guard flux", "executor", gi, 1, go, 1, cost, 1);
    airy_task_node_t *n3 = make_node("N3", "produce gamma", "executor", n3i, 1, n3o, 1, cost, 0);
    if (!n1 || !g1 || !n3) {
        free_plan(p);
        return NULL;
    }
    add_dep(g1, "N1");
    add_dep(n3, "G1");
    p->task_plan_nodes[p->task_plan_node_count++] = n1;
    p->task_plan_nodes[p->task_plan_node_count++] = g1;
    p->task_plan_nodes[p->task_plan_node_count++] = n3;
    c13_entry(p, "N1");
    return p;
}

/* 低成本收敛产物（预算用例：修复后每节点 cost=1，不触发 E-03） */
static airy_err_t c13_small_s2(const airy_gccp_goal_t *goal, const char *patch_json,
                               airy_task_plan_t **out_plan, void *user_data)
{
    grad_mock_ctx_t *mc = (grad_mock_ctx_t *)user_data;
    const char *out[] = {"s01.done"};
    (void)goal;
    (void)patch_json;
    if (mc)
        mc->s2_call_count++;
    airy_task_plan_t *p = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!p)
        return AIRY_ENOMEM;
    p->task_plan_id = AIRY_STRDUP("c13_small");
    p->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    if (!p->task_plan_id || !p->task_plan_nodes) {
        free_plan(p);
        return AIRY_ENOMEM;
    }
    p->task_plan_nodes[p->task_plan_node_count++] =
        make_node("S_01", "execute", "executor", NULL, 0, out, 1, 1, 0);
    c13_entry(p, "S_01");
    *out_plan = p;
    return AIRY_SUCCESS;
}

/* 用例①：E-04 失败必须给 B 一次定向仲裁，七字段完备 */
void test_c13_e04_calls(void)
{
    c13_rig_t rig;
    c13_rig_init(&rig);
    airy_task_plan_t *seed = c13_guard3(10);
    TEST_CHECK(seed != NULL, "seed alloc failed");

    c13_run_cfg_t rc = {.s2 = grad_mock_s2, .policy = AIRY_GRAD_AOF_E04_ONLY, .time_ms = 0};
    c13_rig_run(&rig, &rc, seed);
    free_plan(seed);

    TEST_CHECK(rig.err == AIRY_SUCCESS, "expected converged run");
    TEST_CHECK(rig.stats.converged == 1, "expected converged");
    TEST_CHECK(rig.stats.arbiter_failure_calls == 1, "B must be called once on E-04");
    TEST_CHECK(rig.b.calls == 1, "mock B call count mismatch");
    TEST_CHECK(rig.b.fields_ok == 1, "seven-field failure input incomplete");
    TEST_CHECK(rig.b.round == 0, "B round index mismatch");
    TEST_CHECK(rig.b.token_budget > 0, "token_budget must be positive");
    TEST_CHECK(rig.stats.arbiter_internal_error == 0, "unexpected internal error");
    TEST_CHECK(rig.stats.arbiter_failure_override == 0, "accept must keep C patch");
    TEST_CHECK(rig.s2.s2_call_count >= 1, "S2 must regenerate after rejection");
    if (rig.final)
        free_plan(rig.final);
    TEST_PASS("E-04 failure calls B once with complete seven fields");
}

/* 用例②：B 超时 / 非 JSON → 回退 C 判定，记 internal_error，不阻塞 */
static void c13_case_fallback(airy_err_t err, const char *verdict, const char *name)
{
    c13_rig_t rig;
    c13_rig_init(&rig);
    rig.b.err = err;
    rig.b.verdict = verdict;
    airy_task_plan_t *seed = c13_guard3(10);
    if (!seed) {
        TEST_FAIL(name, "seed alloc failed");
        return;
    }
    c13_run_cfg_t rc = {.s2 = grad_mock_s2, .policy = AIRY_GRAD_AOF_E04_ONLY, .time_ms = 0};
    c13_rig_run(&rig, &rc, seed);
    free_plan(seed);

    char msg[192];
    if (rig.err == AIRY_SUCCESS && rig.stats.converged &&
        rig.stats.arbiter_internal_error == 1 && rig.stats.arbiter_failure_override == 0 &&
        rig.b.calls == 1) {
        TEST_PASS(name);
    } else {
        snprintf(msg, sizeof(msg),
                 "expected fallback to C, got err=%d conv=%u int_err=%u ovr=%u calls=%d",
                 (int)rig.err, (unsigned)rig.stats.converged,
                 (unsigned)rig.stats.arbiter_internal_error,
                 (unsigned)rig.stats.arbiter_failure_override, rig.b.calls);
        TEST_FAIL(name, msg);
    }
    if (rig.final)
        free_plan(rig.final);
}

void test_c13_fallback(void)
{
    c13_case_fallback(AIRY_ETIMEDOUT, NULL, "B timeout falls back to C patch");
    c13_case_fallback(AIRY_SUCCESS, "not json", "B non-JSON falls back to C patch");
}

/* 用例③：salvageable=true 但 scope 空 → patch_invalid + 全量重绘 */
void test_c13_patch_invalid(void)
{
    c13_rig_t rig;
    c13_rig_init(&rig);
    rig.b.verdict = "{\"verdict\":\"reject\",\"defect_class\":\"SEM\",\"salvageable\":true,"
                    "\"affected_scope\":[],\"suggestion\":\"drift\"}";
    airy_task_plan_t *seed = c13_guard3(10);
    TEST_CHECK(seed != NULL, "seed alloc failed");

    c13_run_cfg_t rc = {.s2 = grad_mock_s2, .policy = AIRY_GRAD_AOF_E04_ONLY, .time_ms = 0};
    c13_rig_run(&rig, &rc, seed);
    free_plan(seed);

    TEST_CHECK(rig.err == AIRY_SUCCESS, "expected converged run");
    TEST_CHECK(rig.stats.converged == 1, "expected converged");
    TEST_CHECK(rig.stats.arbiter_failure_override == 1, "full redraw must count as override");
    TEST_CHECK(rig.stats.arbiter_internal_error == 0, "contract-valid verdict is not an error");
    TEST_CHECK(rig.ev.invalid == 1, "grad_patch_invalid must be emitted once");
    TEST_CHECK(rig.s2.s2_call_count >= 1, "S2 must redraw the plan");
    if (rig.final)
        free_plan(rig.final);
    TEST_PASS("empty scope with salvageable=true triggers patch_invalid full redraw");
}

/* 用例④：arbiter_on_failure=off → B 零调用 */
void test_c13_policy_off(void)
{
    c13_rig_t rig;
    c13_rig_init(&rig);
    airy_task_plan_t *seed = c13_guard3(10);
    TEST_CHECK(seed != NULL, "seed alloc failed");

    c13_run_cfg_t rc = {.s2 = grad_mock_s2, .policy = AIRY_GRAD_AOF_OFF, .time_ms = 0};
    c13_rig_run(&rig, &rc, seed);
    free_plan(seed);

    TEST_CHECK(rig.err == AIRY_SUCCESS, "expected converged run");
    TEST_CHECK(rig.stats.converged == 1, "expected converged");
    TEST_CHECK(rig.stats.arbiter_failure_calls == 0, "policy off must not call B");
    TEST_CHECK(rig.b.calls == 0, "mock B must stay untouched");
    TEST_CHECK(rig.stats.arbiter_skipped_budget == 0, "off is not a budget skip");
    if (rig.final)
        free_plan(rig.final);
    TEST_PASS("arbiter_on_failure=off keeps B out of the failure path");
}

/* 用例⑤：失败路径预算不足 → 跳过仲裁（不调用 B） */
void test_c13_budget_skip(void)
{
    c13_rig_t rig;
    c13_rig_init(&rig);
    rig.ev.sleep_ms = 20; /* 验证阶段后已耗时 ≥ budget.time_ms */
    airy_task_plan_t *seed = c13_guard3(1);
    TEST_CHECK(seed != NULL, "seed alloc failed");

    c13_run_cfg_t rc = {.s2 = c13_small_s2, .policy = AIRY_GRAD_AOF_E04_ONLY, .time_ms = 5};
    c13_rig_run(&rig, &rc, seed);
    free_plan(seed);

    TEST_CHECK(rig.ev.slept == 1, "sleep probe not triggered");
    TEST_CHECK(rig.err == AIRY_SUCCESS, "expected converged run");
    TEST_CHECK(rig.stats.converged == 1, "expected converged");
    TEST_CHECK(rig.stats.arbiter_skipped_budget == 1, "budget guard must skip B");
    TEST_CHECK(rig.b.calls == 0, "B must not be invoked under budget pressure");
    TEST_CHECK(rig.ev.skip_ev == 1, "grad_arbiter_failure_skip event missing");
    if (rig.final)
        free_plan(rig.final);
    TEST_PASS("budget exhaustion skips failure arbitration");
}

/* J-16：连续两轮 (E-02, ∅) → 无进展即停，fail-closed */
void test_c13_j16_stop(void)
{
    c13_rig_t rig;
    c13_rig_init(&rig);
    c13_run_cfg_t rc = {.s2 = grad_mock_s2_bad, .policy = AIRY_GRAD_AOF_E04_ONLY, .time_ms = 0};
    c13_rig_run(&rig, &rc, NULL);

    TEST_CHECK(rig.stats.stopped == 1, "J-16 must stop the loop");
    TEST_CHECK(rig.stats.total_rounds == 2, "stopped run counts two rounds");
    TEST_CHECK(rig.err == AIRY_ETIMEDOUT, "fail-closed returns ETIMEDOUT");
    TEST_CHECK(rig.final == NULL, "defective plan must not leak out");
    TEST_CHECK(rig.stats.converged == 0, "must not be converged");
    TEST_PASS("J-16 stops on repeated defect code and scope");
}

/* P1-11（Gate 33 反向用例）：注入 LLM 不可用（complete=NULL）→ 真实
 * s1_failure 与主路径 s1_arbiter 均须显式上报（非 0），B 故障不得
 * 静默降级为豁免；错误码透传与主路径同语义，verdict 不外泄。 */
void test_c13_s1_failclosed(void)
{
    grad_llm_ctx_t ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx)); /* complete=NULL → AIRY_ESERVICE */
    const char *ep = "deploy the web service";
    airy_gccp_goal_t goal;
    __builtin_memset(&goal, 0, sizeof(goal));
    goal.goal_endpoint = ep;

    airy_task_plan_t *plan = c13_guard3(10);
    TEST_CHECK(plan != NULL, "plan alloc failed");

    airy_grad_report_t rep;
    __builtin_memset(&rep, 0, sizeof(rep));
    TEST_CHECK(airy_grad_verify_plan_g(plan, NULL, &goal, &rep) == AIRY_SUCCESS,
               "verify must run");

    grad_arb_fail_in_t fin;
    __builtin_memset(&fin, 0, sizeof(fin));
    fin.goal_json = "{\"goal_endpoint\":\"deploy the web service\"}";
    fin.c_report_json = "{\"error\":0}";
    fin.failed_node_json = "{\"task_node_id\":\"G1\"}";
    fin.neighbors_json = "{}";
    fin.round_history_json = "[]";
    fin.round = 0;
    fin.token_budget = 1024;

    char *verdict = NULL;
    airy_err_t err = grad_llm_s1_failure(&fin, &verdict, &ctx);
    TEST_CHECK(err == AIRY_ESERVICE, "s1_failure must surface LLM unavailability");
    TEST_CHECK(verdict == NULL, "no verdict may leak out on failure");

    char *verdict2 = NULL;
    airy_err_t err2 = grad_llm_s1_arbiter(&goal, &rep, plan, &verdict2, &ctx);
    TEST_CHECK(err2 == AIRY_ESERVICE, "s1_arbiter must surface LLM unavailability");
    TEST_CHECK(verdict2 == NULL, "no verdict may leak out on failure");

    free_plan(plan);
    TEST_PASS("LLM unavailable fails closed on both arbitration paths (P1-11)");
}

#ifdef AIRY_HAS_CJSON
static char *c13_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) <= 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)AIRY_MALLOC((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

static airy_grad_error_t c13_code_of(const char *s)
{
    if (!s)
        return AIRY_GRAD_OK;
    if (strcmp(s, "E-01") == 0)
        return AIRY_GRAD_E01_CAUSAL_BREAK;
    if (strcmp(s, "E-02") == 0)
        return AIRY_GRAD_E02_DEADLOCK;
    if (strcmp(s, "E-03") == 0)
        return AIRY_GRAD_E03_RESOURCE_OVER;
    if (strcmp(s, "E-04") == 0)
        return AIRY_GRAD_E04_GUARD_DRIFT;
    if (strcmp(s, "E-05") == 0)
        return AIRY_GRAD_E05_VERIFY_INSUFFICIENT;
    return AIRY_GRAD_OK;
}

static uint32_t c13_policy_of(const char *s)
{
    if (s && strcmp(s, "off") == 0)
        return AIRY_GRAD_AOF_OFF;
    if (s && strcmp(s, "full") == 0)
        return AIRY_GRAD_AOF_FULL;
    return AIRY_GRAD_AOF_E04_ONLY;
}

static const char *c13_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItem((cJSON *)obj, key);
    return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

/* 逐例比对 grad_arb_fail_wanted：返回失配数（-1 = 文件不可用） */
static int c13_gold_wanted(const airy_task_plan_t *chain, const airy_task_plan_t *guard, int *cnt)
{
    char *txt = c13_read_file(GRAD_GOLDEN_DIR "/wanted.json");
    if (!txt)
        return -1;
    cJSON *root = cJSON_Parse(txt);
    AIRY_FREE(txt);
    if (!root)
        return -1;
    int bad = 0;
    const cJSON *c = NULL;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "cases"))
    {
        const char *name = c13_str(c, "name");
        const char *node = c13_str(c, "node");
        const char *plan_name = c13_str(c, "plan");
        const cJSON *exp = cJSON_GetObjectItem(c, "expect");
        const airy_task_plan_t *plan = (plan_name && strcmp(plan_name, "guard3") == 0) ? guard : chain;
        int got = grad_arb_fail_wanted(c13_policy_of(c13_str(c, "policy")),
                                       c13_code_of(c13_str(c, "code")), plan, node);
        if (!exp || got != exp->valueint) {
            printf("  [FAIL] golden wanted %s: got %d\n", name ? name : "?", got);
            bad++;
        }
        (*cnt)++;
    }
    cJSON_Delete(root);
    return bad;
}

/* 逐例比对 grad_arb_fail_parse；假阳性 = 非法样本被判合法，门槛 5% */
static int c13_gold_parse(const airy_task_plan_t *chain, int *cnt, int *inv)
{
    char *txt = c13_read_file(GRAD_GOLDEN_DIR "/parse.json");
    if (!txt)
        return -1;
    cJSON *root = cJSON_Parse(txt);
    AIRY_FREE(txt);
    if (!root)
        return -1;
    int bad = 0, invalid = 0, fp = 0;
    const cJSON *c = NULL;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "cases"))
    {
        const char *name = c13_str(c, "name");
        const char *json = c13_str(c, "json");
        const char *expect = c13_str(c, "expect");
        int want_valid = expect && strcmp(expect, "valid") == 0;
        (*cnt)++;
        if (!want_valid)
            invalid++;
        grad_arb_fail_t f;
        __builtin_memset(&f, 0, sizeof(f));
        int got_valid = grad_arb_fail_parse(json, chain, &f) == AIRY_SUCCESS;
        if (!want_valid) {
            if (got_valid) {
                printf("  [FAIL] golden parse %s: invalid sample accepted\n", name ? name : "?");
                fp++;
                bad++;
            }
        } else if (!got_valid) {
            printf("  [FAIL] golden parse %s: valid sample rejected\n", name ? name : "?");
            bad++;
        } else {
            const cJSON *kind = cJSON_GetObjectItem(c, "kind");
            const cJSON *scope = cJSON_GetObjectItem(c, "scope");
            const cJSON *salv = cJSON_GetObjectItem(c, "salv");
            const char *sugg = c13_str(c, "suggestion");
            if (kind && cJSON_IsString(kind)) {
                int want_acc = strcmp(kind->valuestring, "accept") == 0;
                if ((f.kind == GRAD_VD_ACCEPT) != want_acc) {
                    printf("  [FAIL] golden parse %s: kind mismatch\n", name ? name : "?");
                    bad++;
                }
            }
            if (scope && f.scope_count != (size_t)scope->valueint) {
                printf("  [FAIL] golden parse %s: scope %zu != %d\n", name ? name : "?",
                       f.scope_count, scope->valueint);
                bad++;
            }
            if (salv && f.salvageable != salv->valueint) {
                printf("  [FAIL] golden parse %s: salvageable mismatch\n", name ? name : "?");
                bad++;
            }
            if (sugg && strcmp(f.suggestion, sugg) != 0) {
                printf("  [FAIL] golden parse %s: suggestion dropped\n", name ? name : "?");
                bad++;
            }
        }
        grad_arb_fail_free(&f);
    }
    cJSON_Delete(root);
    *inv = invalid;
    if (invalid > 0 && fp * 20 > invalid) {
        printf("  [FAIL] false positive rate %d/%d exceeds 5%%\n", fp, invalid);
        bad++;
    }
    return bad;
}
#endif /* AIRY_HAS_CJSON */

/* 黄金集：wanted 15 例 + parse 17 例（总数 ≥20，假阳性 ≤5%） */
void test_c13_golden(void)
{
#ifndef AIRY_HAS_CJSON
    printf("  [SKIP] golden set requires cJSON\n");
    TEST_PASS("golden set skipped (no cJSON)");
#else
    airy_task_plan_t *chain = c13_chain3(10);
    airy_task_plan_t *guard = c13_guard3(10);
    TEST_CHECK(chain != NULL && guard != NULL, "golden plan alloc failed");
    int nw = 0, np = 0, ninv = 0;

    int bad_w = c13_gold_wanted(chain, guard, &nw);
    int bad_p = c13_gold_parse(chain, &np, &ninv);
    free_plan(chain);
    free_plan(guard);

    TEST_CHECK(bad_w >= 0, "wanted.json unavailable (GRAD_GOLDEN_DIR?)");
    TEST_CHECK(bad_p >= 0, "parse.json unavailable (GRAD_GOLDEN_DIR?)");
    TEST_CHECK(nw + np >= 20, "golden set too small (<20 cases)");
    TEST_CHECK(ninv > 0, "golden parse set has no negative samples");
    TEST_CHECK(bad_w == 0, "golden wanted mismatches");
    TEST_CHECK(bad_p == 0, "golden parse mismatches");
    TEST_PASS("golden set (wanted + parse) all cases match");
#endif
}
