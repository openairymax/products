// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_patch.c
 *   GRAD C6 增量修补域单测（0.1.20 §5.2b.3 / §5.2b.4）。
 *
 * 覆盖四组契约：
 *   - B 裁决解析：合法全字段、scope 去重保序、逐条违例整份拒绝；
 *   - Δk 修补 scope：unique 去重、一阶闭包计数、不可定位 EINVAL、
 *     超界 E2BIG（整份重绘的唯一升级路径）；
 *   - patch 序列化：incremental 与 full 两种 repair_mode，affected_scope
 *     恒非空（G55 模板 patch 防线）；
 *   - 计划合并：scope 内整节点替换、frozen 篡改 STATE_ERROR、越权新节点
 *     EINVAL、scope 内新节点放行、entry points 重算。
 *
 * 补丁域是 G55 防线的唯一收敛出口，任何退化都必须在此处先红。
 */

#include "test_grad_internal.h"

/* 固定 8 节点计划：A→B→C→D 与 E→F→G→H 两条独立链。
 * 闭包上界 min(8, ceil(8/2)) = 4，可覆盖"恰好等于上界"的边界。 */
static airy_task_plan_t *mk_chain8(void)
{
    static const char *ids[8] = {"A", "B", "C", "D", "E", "F", "G", "H"};
    static const char *outs[8] = {"a.out", "b.out", "c.out", "d.out",
                                  "e.out", "f.out", "g.out", "h.out"};
    airy_task_plan_t *p = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(*p));
    if (!p)
        return NULL;
    p->task_plan_id = AIRY_STRDUP("p8");
    p->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(8, sizeof(airy_task_node_t *));
    if (!p->task_plan_id || !p->task_plan_nodes) {
        free_plan(p);
        return NULL;
    }
    for (size_t i = 0; i < 8; i++) {
        airy_task_node_t *n =
            make_node(ids[i], "step", "executor", NULL, 0, &outs[i], 1, 10, 0);
        if (!n) {
            free_plan(p);
            return NULL;
        }
        if (i > 0 && i != 4)
            add_dep(n, ids[i - 1]);
        p->task_plan_nodes[p->task_plan_node_count++] = n;
    }
    return p;
}

/* HUB + 6 直接后继：HUB 的一阶闭包 = 7 > min(8, ceil(7/2)) = 4 → E2BIG。 */
static airy_task_plan_t *mk_hub7(void)
{
    static const char *kids[6] = {"C1", "C2", "C3", "C4", "C5", "C6"};
    airy_task_plan_t *p = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(*p));
    if (!p)
        return NULL;
    p->task_plan_id = AIRY_STRDUP("hub7");
    p->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(7, sizeof(airy_task_node_t *));
    if (!p->task_plan_id || !p->task_plan_nodes) {
        free_plan(p);
        return NULL;
    }
    static const char *hub_out[] = {"hub.out"};
    airy_task_node_t *hub = make_node("HUB", "fanout", "planner", NULL, 0, hub_out, 1, 10, 0);
    if (!hub) {
        free_plan(p);
        return NULL;
    }
    p->task_plan_nodes[p->task_plan_node_count++] = hub;
    for (size_t i = 0; i < 6; i++) {
        airy_task_node_t *n =
            make_node(kids[i], "step", "executor", NULL, 0, NULL, 0, 10, 0);
        if (!n) {
            free_plan(p);
            return NULL;
        }
        add_dep(n, "HUB");
        p->task_plan_nodes[p->task_plan_node_count++] = n;
    }
    return p;
}

static airy_task_plan_t *mk_sub1(const char *id, const char *goal, const char *out, const char *dep)
{
    airy_task_plan_t *s = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(*s));
    if (!s)
        return NULL;
    s->task_plan_id = AIRY_STRDUP("sub");
    s->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(1, sizeof(airy_task_node_t *));
    if (!s->task_plan_id || !s->task_plan_nodes) {
        free_plan(s);
        return NULL;
    }
    const char *o[1];
    size_t oc = 0;
    if (out) {
        o[0] = out;
        oc = 1;
    }
    airy_task_node_t *n = make_node(id, goal, "executor", NULL, 0, oc ? o : NULL, oc, 10, 0);
    if (!n) {
        free_plan(s);
        return NULL;
    }
    if (dep)
        add_dep(n, dep);
    s->task_plan_nodes[s->task_plan_node_count++] = n;
    return s;
}

static const airy_task_node_t *find_node(const airy_task_plan_t *p, const char *id)
{
    for (size_t i = 0; i < p->task_plan_node_count; i++) {
        const airy_task_node_t *n = p->task_plan_nodes[i];
        if (n && n->task_node_id && strcmp(n->task_node_id, id) == 0)
            return n;
    }
    return NULL;
}

static void scope_free(char **s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        AIRY_FREE(s[i]);
    AIRY_FREE(s);
}

/* ==================== B 裁决契约 ==================== */

void test_verdict_contract_valid(void)
{
    airy_task_plan_t *p = mk_chain8();
    TEST_CHECK(p != NULL, "plan alloc failed");

    const char *js = "{\"verdict\":\"reject\",\"confidence\":0.8,\"e04\":\"fail\","
                     "\"defects\":[{\"node_id\":\"B\",\"code\":\"SEM\","
                     "\"evidence\":\"output drifts from goal\","
                     "\"suggestion\":\"realign B with goal\"}],"
                     "\"affected_scope\":[\"B\",\"A\",\"B\",\"GHOST\"],"
                     "\"opinion\":\"redo B\"}";
    grad_verdict_t v;
    airy_err_t err = grad_verdict_parse(js, p, &v);
    TEST_CHECK(err == AIRY_SUCCESS, "valid contract rejected");
    TEST_CHECK(v.kind == GRAD_VD_REJECT, "verdict kind mismatch");
    TEST_CHECK(v.e04 == GRAD_E04_FAIL, "e04 mismatch");
    TEST_CHECK(v.confidence > 0.79 && v.confidence < 0.81, "confidence mismatch");
    TEST_CHECK(v.defect_count == 1 && strcmp(v.defects[0].node_id, "B") == 0,
               "defect node mismatch");
    TEST_CHECK(strcmp(v.defects[0].code, "SEM") == 0, "defect code mismatch");
    TEST_CHECK(strcmp(v.defects[0].evidence, "output drifts from goal") == 0,
               "evidence mismatch");
    /* 未知节点丢弃、重复去重、保序 */
    TEST_CHECK(v.scope_count == 2 && strcmp(v.scope[0], "B") == 0 &&
                   strcmp(v.scope[1], "A") == 0,
               "affected_scope dedup/order mismatch");
    TEST_CHECK(strcmp(v.opinion, "redo B") == 0, "opinion mismatch");
    grad_verdict_free(&v);
    TEST_CHECK(v.defects == NULL && v.scope == NULL && v.scope_count == 0,
               "free did not reset");

    /* accept 无 defects/scope → 合法 */
    grad_verdict_t a;
    err = grad_verdict_parse("{\"verdict\":\"accept\",\"confidence\":1}", p, &a);
    TEST_CHECK(err == AIRY_SUCCESS && a.kind == GRAD_VD_ACCEPT, "accept contract rejected");
    TEST_CHECK(a.defect_count == 0 && a.scope_count == 0, "accept must carry no payload");
    TEST_CHECK(a.e04 == GRAD_E04_UNKNOWN, "e04 default must be unknown");
    grad_verdict_free(&a);

    free_plan(p);
    TEST_PASS("verdict contract accepts full payload with dedup");
}

void test_verdict_contract_invalid(void)
{
    static const char *const bad[] = {
        "{\"verdict\":\"maybe\",\"confidence\":0.5}",
        "{\"verdict\":\"reject\",\"confidence\":1.5}",
        "{\"verdict\":\"reject\",\"confidence\":-0.1}",
        "{\"verdict\":\"reject\",\"confidence\":\"abc\"}",
        "{\"verdict\":\"reject\"}",
        "{\"verdict\":\"reject\",\"confidence\":0.5,\"e04\":\"maybe\"}",
        "{\"verdict\":\"reject\",\"confidence\":0.5,"
        "\"defects\":[{\"node_id\":\"A\",\"code\":\"E-99\"}]}",
        "{\"verdict\":\"reject\",\"confidence\":0.5,"
        "\"defects\":[{\"node_id\":\"GHOST\",\"code\":\"SEM\"}]}",
        "{\"verdict\":\"reject\",\"confidence\":0.5,\"affected_scope\":\"A\"}",
        "plain text without json",
    };
    airy_task_plan_t *p = mk_chain8();
    TEST_CHECK(p != NULL, "plan alloc failed");

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        grad_verdict_t v;
        airy_err_t err = grad_verdict_parse(bad[i], p, &v);
        if (err != AIRY_EINVAL) {
            char msg[96];
            snprintf(msg, sizeof(msg), "case %zu not rejected (err=%d)", i, (int)err);
            TEST_FAIL(__func__, msg);
            free_plan(p);
            return;
        }
        grad_verdict_free(&v);
    }
    free_plan(p);
    TEST_PASS("contract violations rejected wholesale");
}

/* ==================== Δk 修补 scope ==================== */

void test_patch_scope_bounds(void)
{
    airy_task_plan_t *p = mk_chain8();
    TEST_CHECK(p != NULL, "plan alloc failed");

    grad_defect_t d;
    __builtin_memset(&d, 0, sizeof(d));
    char **scope = NULL;
    size_t n = 0;
    size_t clo = 0;

    /* A 无前驱、直接后继 B → 闭包 2 ≤ 上界 4 */
    strcpy(d.node_id, "A");
    strcpy(d.code, "E-01");
    airy_err_t err = grad_patch_scope(p, &d, 1, &scope, &n, &clo);
    TEST_CHECK(err == AIRY_SUCCESS, "head defect scope failed");
    TEST_CHECK(n == 1 && strcmp(scope[0], "A") == 0, "scope must be unique defect id");
    TEST_CHECK(clo == 2, "closure of A must be A + successor B");
    scope_free(scope, n);
    scope = NULL;

    /* C 前驱 A/B + 直接后继 D → 闭包 4，恰等于上界（不升级重绘） */
    strcpy(d.node_id, "C");
    err = grad_patch_scope(p, &d, 1, &scope, &n, &clo);
    TEST_CHECK(err == AIRY_SUCCESS, "boundary closure rejected");
    TEST_CHECK(n == 1 && clo == 4, "closure of C must be 4");
    scope_free(scope, n);
    scope = NULL;

    /* 重复 defect 去重 */
    grad_defect_t dup[2];
    __builtin_memset(dup, 0, sizeof(dup));
    strcpy(dup[0].node_id, "B");
    strcpy(dup[0].code, "E-01");
    strcpy(dup[1].node_id, "B");
    strcpy(dup[1].code, "SEM");
    err = grad_patch_scope(p, dup, 2, &scope, &n, &clo);
    TEST_CHECK(err == AIRY_SUCCESS && n == 1, "duplicate defects must dedup");
    scope_free(scope, n);
    scope = NULL;

    /* 不可定位节点 → EINVAL（G55：不得产出空 scope 模板 patch） */
    strcpy(d.node_id, "GHOST");
    err = grad_patch_scope(p, &d, 1, &scope, &n, &clo);
    TEST_CHECK(err == AIRY_EINVAL && scope == NULL, "unlocatable defect must be EINVAL");

    /* 空 defects → EINVAL */
    err = grad_patch_scope(p, NULL, 0, &scope, &n, NULL);
    TEST_CHECK(err == AIRY_EINVAL, "empty defects must be EINVAL");

    /* 闭包超界 → E2BIG（唯一升级路径：整份重绘） */
    airy_task_plan_t *hub = mk_hub7();
    TEST_CHECK(hub != NULL, "hub plan alloc failed");
    strcpy(d.node_id, "HUB");
    err = grad_patch_scope(hub, &d, 1, &scope, &n, &clo);
    TEST_CHECK(err == AIRY_E2BIG && scope == NULL, "oversize closure must be E2BIG");
    free_plan(hub);

    free_plan(p);
    TEST_PASS("patch scope bounds (EINVAL/E2BIG/closure)");
}

/* ==================== patch 序列化 ==================== */

void test_patch_build_incremental(void)
{
    grad_defect_t d;
    __builtin_memset(&d, 0, sizeof(d));
    strcpy(d.node_id, "B");
    strcpy(d.code, "E-01");
    strcpy(d.evidence, "missing a.out");
    strcpy(d.suggestion, "add A->B edge");

    const char *scope[] = {"B"};
    char *js = NULL;
    airy_err_t err = grad_patch_build(7, &d, 1, scope, 1, &js);
    TEST_CHECK(err == AIRY_SUCCESS && js != NULL, "incremental patch build failed");
    TEST_CHECK(strstr(js, "\"patch_id\":\"B_7\"") != NULL, "patch id mismatch");
    TEST_CHECK(strstr(js, "\"source\":\"B_Model\"") != NULL, "source mismatch");
    TEST_CHECK(strstr(js, "\"repair_mode\":\"incremental\"") != NULL, "mode mismatch");
    TEST_CHECK(strstr(js, "\"affected_scope\":[\"B\"]") != NULL, "scope mismatch");
    TEST_CHECK(strstr(js, "\"evidence\":\"missing a.out\"") != NULL, "evidence missing");
    TEST_CHECK(strstr(js, "\"suggestion\":\"add A->B edge\"") != NULL, "suggestion missing");
    AIRY_FREE(js);
    js = NULL;

    /* G55：空 defects / 空 scope 一律拒绝，不得产出模板 patch */
    js = NULL;
    TEST_CHECK(grad_patch_build(1, NULL, 0, scope, 1, &js) == AIRY_EINVAL,
               "empty defects must be EINVAL");
    TEST_CHECK(grad_patch_build(1, &d, 1, NULL, 0, &js) == AIRY_EINVAL,
               "null scope must be EINVAL");
    TEST_CHECK(grad_patch_build(1, &d, 1, scope, 0, &js) == AIRY_EINVAL,
               "empty scope must be EINVAL");
    TEST_PASS("incremental patch requires non-empty scope/defects");
}

void test_patch_full(void)
{
    airy_task_plan_t *p = mk_chain8();
    TEST_CHECK(p != NULL, "plan alloc failed");

    char *js = NULL;
    airy_err_t err = grad_patch_full(p, 3, NULL, 0, &js);
    TEST_CHECK(err == AIRY_SUCCESS && js != NULL, "full patch build failed");
    TEST_CHECK(strstr(js, "\"repair_mode\":\"full\"") != NULL, "mode mismatch");
    TEST_CHECK(strstr(js, "\"affected_scope\":[\"A\",\"B\",\"C\",\"D\",\"E\",\"F\",\"G\",\"H\"]") !=
                   NULL,
               "full scope must list every node");
    TEST_CHECK(strstr(js, "\"patch_id\":\"B_3\"") != NULL, "patch id mismatch");
    AIRY_FREE(js);
    js = NULL;

    TEST_CHECK(grad_patch_full(NULL, 1, NULL, 0, &js) == AIRY_EINVAL,
               "null plan must be EINVAL");
    free_plan(p);
    TEST_PASS("full redraw patch covers all nodes");
}

/* ==================== 计划合并 ==================== */

void test_plan_merge_scope(void)
{
    airy_task_plan_t *p = mk_chain8();
    TEST_CHECK(p != NULL, "plan alloc failed");
    const char *scope_b[] = {"B"};
    airy_task_plan_t *m = NULL;

    /* scope 内替换：B 的 goal/outputs 变更被接受，frozen 节点原样保留 */
    airy_task_plan_t *sub = mk_sub1("B", "patched step", "b.new", "A");
    TEST_CHECK(sub != NULL, "sub alloc failed");
    airy_err_t err = grad_plan_merge(p, sub, scope_b, 1, &m);
    TEST_CHECK(err == AIRY_SUCCESS && m != NULL, "merge failed");
    TEST_CHECK(m->task_plan_node_count == 8, "merge must keep node count");
    const airy_task_node_t *b = find_node(m, "B");
    TEST_CHECK(b != NULL && strcmp(b->task_node_goal, "patched step") == 0,
               "in-scope node not replaced");
    TEST_CHECK(b->task_node_outputs_count == 1 && strcmp(b->task_node_outputs[0], "b.new") == 0,
               "in-scope outputs not replaced");
    const airy_task_node_t *a = find_node(m, "A");
    TEST_CHECK(a != NULL && strcmp(a->task_node_outputs[0], "a.out") == 0,
               "frozen node mutated");
    /* entry 重算：两条链头 A/E（不信赖原列表） */
    TEST_CHECK(m->task_plan_entry_count == 2 && strcmp(m->task_plan_entry_points[0], "A") == 0 &&
                   strcmp(m->task_plan_entry_points[1], "E") == 0,
               "entry points not recomputed");

    char *view = NULL;
    TEST_CHECK(grad_plan_to_json(m, &view) == AIRY_SUCCESS && view != NULL,
               "plan view serialize failed");
    TEST_CHECK(strstr(view, "\"plan_id\":\"p8\"") != NULL &&
                   strstr(view, "\"entry\":[\"A\",\"E\"]") != NULL,
               "plan view mismatch");
    AIRY_FREE(view);
    free_plan(m);
    free_plan(sub);

    /* frozen 篡改：C 不在 scope 内被改 → STATE_ERROR（本轮作废） */
    airy_task_plan_t *sub2 = mk_sub1("C", "tampered", "c.out", "B");
    TEST_CHECK(sub2 != NULL, "sub2 alloc failed");
    m = NULL;
    err = grad_plan_merge(p, sub2, scope_b, 1, &m);
    TEST_CHECK(err == AIRY_ERR_STATE_ERROR && m == NULL, "frozen tamper must be STATE_ERROR");
    free_plan(sub2);

    /* 越权新节点：X 不在 scope → EINVAL */
    airy_task_plan_t *sub3 = mk_sub1("X", "ghost producer", "x.out", NULL);
    TEST_CHECK(sub3 != NULL, "sub3 alloc failed");
    err = grad_plan_merge(p, sub3, scope_b, 1, &m);
    TEST_CHECK(err == AIRY_EINVAL && m == NULL, "extra node outside scope must be EINVAL");

    /* scope 内新节点：允许插入（E-01 补生产者），entry 含 X */
    const char *scope_x[] = {"X"};
    err = grad_plan_merge(p, sub3, scope_x, 1, &m);
    TEST_CHECK(err == AIRY_SUCCESS && m != NULL, "in-scope new node rejected");
    TEST_CHECK(m->task_plan_node_count == 9, "new node not inserted");
    TEST_CHECK(m->task_plan_entry_count == 3 &&
                   strcmp(m->task_plan_entry_points[2], "X") == 0,
               "entry points missing new node");
    free_plan(m);
    free_plan(sub3);

    /* 空 scope → EINVAL */
    m = NULL;
    err = grad_plan_merge(p, sub3, NULL, 0, &m);
    TEST_CHECK(err == AIRY_EINVAL, "empty scope must be EINVAL");

    free_plan(p);
    TEST_PASS("plan merge scope/frozen/entry semantics");
}

/* ==================== 裁决写出往返（Gate 33） ==================== */

static int vd_same(const grad_verdict_t *a, const grad_verdict_t *b)
{
    if (a->kind != b->kind || a->e04 != b->e04 || a->confidence != b->confidence ||
        strcmp(a->opinion, b->opinion) != 0 || a->defect_count != b->defect_count ||
        a->scope_count != b->scope_count)
        return 0;
    for (size_t i = 0; i < a->defect_count; i++) {
        if (strcmp(a->defects[i].node_id, b->defects[i].node_id) != 0 ||
            strcmp(a->defects[i].code, b->defects[i].code) != 0 ||
            strcmp(a->defects[i].evidence, b->defects[i].evidence) != 0 ||
            strcmp(a->defects[i].suggestion, b->defects[i].suggestion) != 0)
            return 0;
    }
    for (size_t i = 0; i < a->scope_count; i++) {
        if (strcmp(a->scope[i], b->scope[i]) != 0)
            return 0;
    }
    return 1;
}

/* 单语料 100 次 parse→emit：每次输出必须与首轮规范形逐字节相同。 */
static void vd_rt_one(const airy_task_plan_t *plan, const char *src, const char *tag)
{
    grad_verdict_t base;
    TEST_CHECK(grad_verdict_parse(src, plan, &base) == AIRY_SUCCESS, tag);

    char *canon = NULL;
    if (grad_verdict_to_json(&base, &canon) != AIRY_SUCCESS || !canon) {
        grad_verdict_free(&base);
        TEST_FAIL(__func__, "canonical emit failed");
        return;
    }
    char *cur = AIRY_STRDUP(canon);
    if (!cur) {
        AIRY_FREE(canon);
        grad_verdict_free(&base);
        TEST_FAIL(__func__, "dup failed");
        return;
    }

    for (int i = 0; i < 100; i++) {
        grad_verdict_t v;
        if (grad_verdict_parse(cur, plan, &v) != AIRY_SUCCESS) {
            AIRY_FREE(cur);
            AIRY_FREE(canon);
            grad_verdict_free(&base);
            TEST_FAIL(__func__, "roundtrip parse failed");
            return;
        }
        char *next = NULL;
        int ok = grad_verdict_to_json(&v, &next) == AIRY_SUCCESS && next &&
                 strcmp(next, canon) == 0 && vd_same(&v, &base);
        grad_verdict_free(&v);
        AIRY_FREE(cur);
        cur = next;
        if (!ok) {
            AIRY_FREE(cur);
            AIRY_FREE(canon);
            grad_verdict_free(&base);
            TEST_FAIL(__func__, "roundtrip bytes drifted");
            return;
        }
    }
    AIRY_FREE(cur);
    AIRY_FREE(canon);
    grad_verdict_free(&base);
    TEST_PASS(tag);
}

void test_verdict_roundtrip(void)
{
    airy_task_plan_t *p = mk_chain8();
    TEST_CHECK(p != NULL, "plan alloc failed");

    vd_rt_one(p, "{\"verdict\":\"reject\",\"confidence\":0.8,\"e04\":\"fail\","
                 "\"defects\":[{\"node_id\":\"B\",\"code\":\"SEM\","
                 "\"evidence\":\"output drifts from goal\","
                 "\"suggestion\":\"realign B with goal\"},"
                 "{\"node_id\":\"C\",\"code\":\"E-01\"}],"
                 "\"affected_scope\":[\"B\",\"A\",\"B\",\"GHOST\"],"
                 "\"opinion\":\"redo B\"}",
             "roundtrip full payload");

    vd_rt_one(p, "{\"verdict\":\"accept\",\"confidence\":1}", "roundtrip minimal accept");

    vd_rt_one(p, "{\"verdict\":\"reject\",\"confidence\":\"0.25\",\"e04\":\"pass\","
                 "\"defects\":[{\"node_id\":\"H\",\"code\":\"E-03\"}]}",
             "roundtrip numeric string");

    /* 写出侧入参校验：NULL 与无 cJSON 构建之外的非法入参不得产出伪 JSON */
    char *out = (char *)"sentinel";
    TEST_CHECK(grad_verdict_to_json(NULL, &out) == AIRY_EINVAL && out == NULL,
               "NULL verdict must be EINVAL");
    grad_verdict_t empty;
    AIRY_MEMSET(&empty, 0, sizeof(empty));
    empty.confidence = 0.5;
    TEST_CHECK(grad_verdict_to_json(&empty, NULL) == AIRY_EINVAL, "NULL out must be EINVAL");

    free_plan(p);
    TEST_PASS("verdict emit ownership and argument guards");
}
