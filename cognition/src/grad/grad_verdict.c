// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_verdict.c
 * @brief GRAD 模型 B 裁决契约解析（0.1.20 C2 / §5.2b.4 输出强校验）。
 *
 * 0.1.19 的仲裁解析是松散的：verdict/confidence/e04 逐字段按需读取，
 * 缺失或类型异常时静默取缺省值。这带来两类漏洞——非法 JSON 被当作
 * accept 放行（假收敛），以及驳回意见没有 defects 可投影（patch 退化
 * 为固定模板，C6 无输入可修补）。本文件把 B 的输出收口为单一严格
 * 契约：任一字段违例整份拒绝，由调用方记 arbiter_parse_error 并降级
 * "采纳 C 判定"，绝不把非法输入读成 accept。
 *
 * 字段域（权威定义见 0.1.20 方案 L777）：
 *   verdict         accept|reject（必填）
 *   confidence      number 或数字串，∈[0,1]（必填）
 *   e04             pass|fail|unknown（缺省 unknown）
 *   defects[]       {node_id, code, evidence, suggestion}，node_id 必须
 *                   存在于被裁决计划，code ∈ {E-01..E-05, SEM}
 *   affected_scope  node_id 数组，元素必须存在，去重保序
 *   opinion         自由文本（仅审计用，不参与判定）
 *
 * 有界性：defects[] 超 GRAD_ARBITER_MAX_DEFECTS 截断（保裁决本体，
 * 丢弃溢出条目并 WARN）；affected_scope[] 上限为计划节点数。字段长度
 * 超出定长缓冲一律视为契约违例（node_id 尤其不能截断——截断后无法
 * 与计划节点对应，C6 修补会落到错误节点）。
 */

#include "grad_internal.h"

#include <errno.h>

/** @brief 缺陷码合法域（E-05 充分性 + SEM 语义缺陷，与 C 侧判据对齐）。 */
static const char *const GRAD_VD_CODES[] = {"E-01", "E-02", "E-03", "E-04", "E-05", "SEM"};

#ifdef AIRY_HAS_CJSON

static int vd_code_ok(const char *code)
{
    if (!code)
        return 0;
    for (size_t i = 0; i < sizeof(GRAD_VD_CODES) / sizeof(GRAD_VD_CODES[0]); i++) {
        if (strcmp(code, GRAD_VD_CODES[i]) == 0)
            return 1;
    }
    return 0;
}

static int vd_node_exists(const airy_task_plan_t *plan, const char *id)
{
    if (!plan || !id || !id[0])
        return 0;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *n = plan->task_plan_nodes[i];
        if (n && n->task_node_id && strcmp(n->task_node_id, id) == 0)
            return 1;
    }
    return 0;
}

/** @brief verdict 字段：仅 accept/reject 两值。 */
static int vd_kind(const cJSON *root, grad_vd_kind_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "verdict");
    if (!cJSON_IsString(v) || !v->valuestring)
        return -1;
    if (strcmp(v->valuestring, "accept") == 0) {
        *out = GRAD_VD_ACCEPT;
        return 0;
    }
    if (strcmp(v->valuestring, "reject") == 0) {
        *out = GRAD_VD_REJECT;
        return 0;
    }
    return -1;
}

/**
 * @brief confidence 字段：数字或数字串，且落在 [0,1]。
 *
 * 数字串容错是历史行为（LLM 常输出 "0.9"），但类型非法或越界一律
 * 失败——越界置信度会让 θ_B 折让逻辑失真。
 */
static int vd_confidence(const cJSON *root, double *out)
{
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "confidence");
    if (!c)
        return -1;
    double v;
    if (cJSON_IsNumber(c)) {
        v = c->valuedouble;
    } else if (cJSON_IsString(c) && c->valuestring && c->valuestring[0]) {
        char *end = NULL;
        errno = 0;
        v = strtod(c->valuestring, &end);
        if (errno != 0 || !end || *end != '\0')
            return -1;
    } else {
        return -1;
    }
    /* NaN 比较恒假：一并拒绝 */
    if (!(v >= 0.0 && v <= 1.0))
        return -1;
    *out = v;
    return 0;
}

/** @brief e04 字段：缺省 unknown；出现则必须三值之一。 */
static int vd_e04(const cJSON *root, grad_e04_t *out)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "e04");
    *out = GRAD_E04_UNKNOWN;
    if (!e)
        return 0;
    if (cJSON_IsString(e) && e->valuestring) {
        if (strcmp(e->valuestring, "pass") == 0) {
            *out = GRAD_E04_PASS;
            return 0;
        }
        if (strcmp(e->valuestring, "fail") == 0) {
            *out = GRAD_E04_FAIL;
            return 0;
        }
        if (strcmp(e->valuestring, "unknown") == 0) {
            *out = GRAD_E04_UNKNOWN;
            return 0;
        }
    }
    return -1;
}

/** @brief 定长字段拷贝：超界即契约违例（不静默截断语义标识）。 */
static int vd_copy(char *dst, size_t cap, const cJSON *item)
{
    const char *s = (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
    if (!s) {
        dst[0] = '\0';
        return 0;
    }
    size_t n = strlen(s);
    if (n >= cap)
        return -1;
    AIRY_MEMCPY(dst, s, n);
    dst[n] = '\0';
    return 0;
}

/** @brief 单条缺陷：node_id/code 必填且必须可定位，evidence/suggestion 可选。 */
static int vd_defect(const cJSON *item, const airy_task_plan_t *plan, grad_defect_t *d)
{
    if (!cJSON_IsObject(item))
        return -1;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "node_id");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "code");
    if (!cJSON_IsString(id) || !id->valuestring || !vd_node_exists(plan, id->valuestring))
        return -1;
    if (!cJSON_IsString(code) || !code->valuestring || !vd_code_ok(code->valuestring))
        return -1;

    AIRY_MEMSET(d, 0, sizeof(*d));
    if (vd_copy(d->node_id, sizeof(d->node_id), id) != 0)
        return -1;
    if (vd_copy(d->code, sizeof(d->code), code) != 0)
        return -1;
    if (vd_copy(d->evidence, sizeof(d->evidence),
                cJSON_GetObjectItemCaseSensitive(item, "evidence")) != 0)
        return -1;
    if (vd_copy(d->suggestion, sizeof(d->suggestion),
                cJSON_GetObjectItemCaseSensitive(item, "suggestion")) != 0)
        return -1;
    return 0;
}

static int vd_scope_has(char **scope, size_t count, const char *id)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(scope[i], id) == 0)
            return 1;
    }
    return 0;
}

/** @brief affected_scope[]：元素必须存在于计划，去重保序。 */
static airy_err_t vd_scope(const cJSON *root, const airy_task_plan_t *plan, grad_verdict_t *out)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "affected_scope");
    if (!arr)
        return AIRY_SUCCESS;
    if (!cJSON_IsArray(arr))
        return AIRY_EINVAL;

    int n = cJSON_GetArraySize(arr);
    if (n <= 0)
        return AIRY_SUCCESS;
    if ((size_t)n > plan->task_plan_node_count)
        n = (int)plan->task_plan_node_count;

    char **scope = (char **)AIRY_CALLOC((size_t)n, sizeof(char *));
    if (!scope)
        return AIRY_ENOMEM;

    size_t count = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0]) {
            AIRY_FREE(scope);
            return AIRY_EINVAL;
        }
        if (!vd_node_exists(plan, it->valuestring) || vd_scope_has(scope, count, it->valuestring))
            continue;
        char *dup = AIRY_STRDUP(it->valuestring);
        if (!dup) {
            for (size_t k = 0; k < count; k++)
                AIRY_FREE(scope[k]);
            AIRY_FREE(scope);
            return AIRY_ENOMEM;
        }
        scope[count++] = dup;
    }
    if (count == 0) {
        AIRY_FREE(scope);
        return AIRY_SUCCESS;
    }
    out->scope = scope;
    out->scope_count = count;
    return AIRY_SUCCESS;
}

/** @brief defects[]：逐条强校验；超出上界截断（裁决本体优先保留）。 */
static airy_err_t vd_defects(const cJSON *root, const airy_task_plan_t *plan, grad_verdict_t *out)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "defects");
    if (!arr)
        return AIRY_SUCCESS;
    if (!cJSON_IsArray(arr) || plan->task_plan_node_count == 0)
        return AIRY_EINVAL;

    int total = cJSON_GetArraySize(arr);
    if (total <= 0)
        return AIRY_SUCCESS;
    size_t n = (size_t)total;
    if (n > GRAD_ARBITER_MAX_DEFECTS) {
        AIRY_LOG_WARN("GRAD-B: defects overflow (%zu > %d), truncated",
                      n, GRAD_ARBITER_MAX_DEFECTS);
        n = GRAD_ARBITER_MAX_DEFECTS;
    }

    grad_defect_t *defects = (grad_defect_t *)AIRY_CALLOC(n, sizeof(grad_defect_t));
    if (!defects)
        return AIRY_ENOMEM;

    for (size_t i = 0; i < n; i++) {
        if (vd_defect(cJSON_GetArrayItem(arr, (int)i), plan, &defects[i]) != 0) {
            AIRY_FREE(defects);
            return AIRY_EINVAL;
        }
    }
    out->defects = defects;
    out->defect_count = n;
    return AIRY_SUCCESS;
}

#endif /* AIRY_HAS_CJSON */

void grad_verdict_free(grad_verdict_t *v)
{
    if (!v)
        return;
    AIRY_FREE(v->defects);
    if (v->scope) {
        for (size_t i = 0; i < v->scope_count; i++)
            AIRY_FREE(v->scope[i]);
        AIRY_FREE(v->scope);
    }
    AIRY_MEMSET(v, 0, sizeof(*v));
}

airy_err_t grad_verdict_parse(const char *json, const airy_task_plan_t *plan,
                              grad_verdict_t *out)
{
    if (!json || !plan || !out)
        return AIRY_EINVAL;
    AIRY_MEMSET(out, 0, sizeof(*out));
    if (!json[0] || plan->task_plan_node_count == 0)
        return AIRY_EINVAL;

#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return AIRY_EINVAL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return AIRY_EINVAL;
    }

    grad_verdict_t v;
    AIRY_MEMSET(&v, 0, sizeof(v));
    const cJSON *opinion = cJSON_GetObjectItemCaseSensitive(root, "opinion");
    if (cJSON_IsString(opinion) && opinion->valuestring &&
        vd_copy(v.opinion, sizeof(v.opinion), opinion) != 0) {
        cJSON_Delete(root);
        return AIRY_EINVAL;
    }

    airy_err_t err;
    if (vd_kind(root, &v.kind) != 0 || vd_confidence(root, &v.confidence) != 0 ||
        vd_e04(root, &v.e04) != 0) {
        err = AIRY_EINVAL;
    } else {
        err = vd_defects(root, plan, &v);
        if (err == AIRY_SUCCESS)
            err = vd_scope(root, plan, &v);
    }
    cJSON_Delete(root);

    if (err != AIRY_SUCCESS) {
        grad_verdict_free(&v);
        return err;
    }
    *out = v;
    return AIRY_SUCCESS;
#else
    /* 无 cJSON：契约解析不可用，按服务缺失降级（调用方采纳 C 判定），
     * 不做"永远 accept"之类的伪解析。 */
    AIRY_LOG_WARN("GRAD-B: verdict contract requires cJSON, deferring to Model C");
    return AIRY_ESERVICE;
#endif
}
