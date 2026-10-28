// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file plan_schema.c
 * @brief GRAD 计划 schema v1 校验实现（0.1.20 C1）。
 *
 * 两级校验：JSON 层（字段类型/取值域/长度界，逐节点独立判定）+
 * 结构层（跨节点引用一致性：id/outputs 唯一、depends 存在且无自环、
 * inputs 的 producer 落在 depends 传递闭包、entry 引用存在）。
 * 违例即整份拒绝，首个违例回填 grad_schema_err_t 供日志与测试定位。
 *
 * artifacts 的因果存在性（producer outputs 是否真含被引名字）不在
 * 本层判定——该语义判定属 C5 E-01；本层只保证拓扑引用完整。
 */

#include "plan_schema.h"

#include "grad_internal.h"
#include "logging.h"

#include <string.h>

#ifdef AIRY_HAS_CJSON

/* 角色合法值表（序数与 grad_plan_role_e 对应） */
static const char *const GRAD_PLAN_ROLES[GRAD_PLAN_ROLE_COUNT] = {
    "plan", "retrieve", "reason", "act", "verify",
};

/* 计划来源合法值表（序数与 grad_plan_origin_e 对应） */
static const char *const GRAD_PLAN_ORIGINS[] = {
    "generated", "seed", "fallback",
};

grad_plan_role_e grad_role_of(const char *role)
{
    if (!role)
        return GRAD_PLAN_ROLE_COUNT;
    for (int i = 0; i < GRAD_PLAN_ROLE_COUNT; i++) {
        if (strcmp(role, GRAD_PLAN_ROLES[i]) == 0)
            return (grad_plan_role_e)i;
    }
    return GRAD_PLAN_ROLE_COUNT;
}

static void grad_serr_set(grad_schema_err_t *err, int idx, const char *field,
                          const char *reason)
{
    if (!err)
        return;
    err->index = idx;
    snprintf(err->field, sizeof(err->field), "%s", field);
    snprintf(err->reason, sizeof(err->reason), "%s", reason);
}

/* [A-Za-z0-9_] 基集 + 可选 '.'/'-' 的非空限长字符串 */
static bool grad_pat(const char *s, size_t max_len, bool dot, bool dash)
{
    if (!s || !s[0])
        return false;
    size_t n = strlen(s);
    if (n > max_len)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || (dash && c == '-') ||
                  (dot && c == '.');
        if (!ok)
            return false;
    }
    return true;
}

/* 整数值 number（机制层 cost/timeout/priority 均为整型域） */
static bool grad_is_int(const cJSON *item)
{
    if (!cJSON_IsNumber(item))
        return false;
    double v = item->valuedouble;
    return v == (double)(int64_t)v;
}

/* 可选字符串数组：元素非空限长、数量受限 */
static bool grad_str_arr(const cJSON *arr, int max_count, size_t max_len)
{
    if (!cJSON_IsArray(arr))
        return false;
    int n = cJSON_GetArraySize(arr);
    if (n > max_count)
        return false;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(it) || !it->valuestring[0] ||
            strlen(it->valuestring) > max_len)
            return false;
    }
    return true;
}

/* 可选双维 cost：给出则正整数且不超上限（缺省留给 C4 推导标 inferred） */
static bool grad_cost_ok(const cJSON *nd, grad_schema_err_t *err, int idx)
{
    static const char *const keys[] = {"cost_time_ms", "cost_mem_mb"};
    for (int k = 0; k < 2; k++) {
        cJSON *c = cJSON_GetObjectItem(nd, keys[k]);
        if (!c)
            continue;
        if (!grad_is_int(c) || c->valuedouble <= 0.0 ||
            c->valuedouble > GRAD_COST_MAX) {
            grad_serr_set(err, idx, keys[k], "range");
            return false;
        }
    }
    return true;
}

static int grad_find_node(const cJSON *nodes, const char *id)
{
    int n = cJSON_GetArraySize(nodes);
    for (int i = 0; i < n; i++) {
        cJSON *nid = cJSON_GetObjectItem(cJSON_GetArrayItem(nodes, i), "id");
        if (cJSON_IsString(nid) && nid->valuestring &&
            strcmp(nid->valuestring, id) == 0)
            return i;
    }
    return -1;
}

/* from 的 depends 传递闭包是否含 target（from 不含自身 → 天然拒自环） */
static bool grad_dep_reach(const cJSON *nodes, int from, int target)
{
    bool visited[GRAD_MAX_NODES] = {false};
    int queue[GRAD_MAX_NODES];
    int head = 0, tail = 0;
    visited[from] = true;
    queue[tail++] = from;
    while (head < tail) {
        cJSON *deps =
            cJSON_GetObjectItem(cJSON_GetArrayItem(nodes, queue[head++]), "depends");
        if (!cJSON_IsArray(deps))
            continue;
        int n = cJSON_GetArraySize(deps);
        for (int d = 0; d < n; d++) {
            cJSON *dep = cJSON_GetArrayItem(deps, d);
            if (!cJSON_IsString(dep) || !dep->valuestring)
                continue;
            int di = grad_find_node(nodes, dep->valuestring);
            if (di == target)
                return true;
            if (di >= 0 && !visited[di]) {
                visited[di] = true;
                queue[tail++] = di;
            }
        }
    }
    return false;
}

/* 非 lit: 输入必须形如 {producer_id}.{artifact}，producer ∈ 闭包 */
static bool grad_input_ok(const cJSON *nodes, int idx, const char *input,
                          grad_schema_err_t *err)
{
    if (strncmp(input, "lit:", 4) == 0)
        return true;
    const char *dot = strchr(input, '.');
    if (!dot || dot == input || !dot[1]) {
        grad_serr_set(err, idx, "inputs", "format");
        return false;
    }
    size_t plen = (size_t)(dot - input);
    if (plen > GRAD_NODE_ID_MAX) {
        grad_serr_set(err, idx, "inputs", "format");
        return false;
    }
    char pid[GRAD_NODE_ID_MAX + 1];
    AIRY_MEMCPY(pid, input, plen);
    pid[plen] = '\0';
    if (!grad_pat(pid, GRAD_NODE_ID_MAX, false, true) ||
        !grad_pat(dot + 1, GRAD_ARTIFACT_MAX, false, true)) {
        grad_serr_set(err, idx, "inputs", "pattern");
        return false;
    }
    int pi = grad_find_node(nodes, pid);
    if (pi < 0) {
        grad_serr_set(err, idx, "inputs", "producer_missing");
        return false;
    }
    if (!grad_dep_reach(nodes, idx, pi)) {
        grad_serr_set(err, idx, "inputs", "producer_not_in_depends");
        return false;
    }
    return true;
}

/* JSON 层：单节点字段域检查（独立可判定项，不跨节点） */
static bool grad_check_node(const cJSON *nd, grad_schema_err_t *err, int idx)
{
    cJSON *id = cJSON_GetObjectItem(nd, "id");
    if (!cJSON_IsString(id) || !grad_pat(id->valuestring, GRAD_NODE_ID_MAX, false, true)) {
        grad_serr_set(err, idx, "id", cJSON_IsString(id) ? "pattern" : "missing");
        return false;
    }
    cJSON *goal = cJSON_GetObjectItem(nd, "goal");
    if (!cJSON_IsString(goal) || !goal->valuestring[0] ||
        strlen(goal->valuestring) > GRAD_GOAL_MAX) {
        grad_serr_set(err, idx, "goal", "range");
        return false;
    }
    cJSON *role = cJSON_GetObjectItem(nd, "role");
    if (!cJSON_IsString(role) || grad_role_of(role->valuestring) == GRAD_PLAN_ROLE_COUNT) {
        grad_serr_set(err, idx, "role", "enum");
        return false;
    }
    cJSON *deps = cJSON_GetObjectItem(nd, "depends");
    if (deps && !cJSON_IsNull(deps) &&
        !grad_str_arr(deps, GRAD_DEPENDS_MAX, GRAD_NODE_ID_MAX)) {
        grad_serr_set(err, idx, "depends", "range");
        return false;
    }
    cJSON *inputs = cJSON_GetObjectItem(nd, "inputs");
    if (inputs && !cJSON_IsNull(inputs) &&
        !grad_str_arr(inputs, GRAD_INPUTS_MAX, GRAD_INPUT_MAX)) {
        grad_serr_set(err, idx, "inputs", "range");
        return false;
    }
    /* outputs 可缺省（C4 推导补全），给出则逐项非空且模式合法 */
    cJSON *outputs = cJSON_GetObjectItem(nd, "outputs");
    if (outputs && !cJSON_IsNull(outputs) &&
        !grad_str_arr(outputs, GRAD_OUTPUTS_MAX, GRAD_ARTIFACT_MAX)) {
        grad_serr_set(err, idx, "outputs", "range");
        return false;
    }
    if (!grad_cost_ok(nd, err, idx))
        return false;
    cJSON *to = cJSON_GetObjectItem(nd, "timeout_ms");
    if (to && (!grad_is_int(to) || to->valuedouble < 1.0 ||
               to->valuedouble > (double)GRAD_TIMEOUT_MAX_MS)) {
        grad_serr_set(err, idx, "timeout_ms", "range");
        return false;
    }
    cJSON *prio = cJSON_GetObjectItem(nd, "priority");
    if (prio && (!grad_is_int(prio) || prio->valuedouble < 0.0 || prio->valuedouble > 255.0)) {
        grad_serr_set(err, idx, "priority", "range");
        return false;
    }
    cJSON *guard = cJSON_GetObjectItem(nd, "invariant_guard");
    if (guard && !cJSON_IsBool(guard)) {
        grad_serr_set(err, idx, "invariant_guard", "type");
        return false;
    }
    /* plan_origin 系统字段：可缺省（视为 generated），给出必须在枚举域 */
    cJSON *origin = cJSON_GetObjectItem(nd, "plan_origin");
    if (origin) {
        bool ok = false;
        if (cJSON_IsString(origin) && origin->valuestring) {
            for (int k = 0; k < 3 && !ok; k++)
                ok = strcmp(origin->valuestring, GRAD_PLAN_ORIGINS[k]) == 0;
        }
        if (!ok) {
            grad_serr_set(err, idx, "plan_origin", "enum");
            return false;
        }
    }
    return true;
}

/* 结构层：跨节点引用一致性 */
static bool grad_struct_ok(const cJSON *nodes, int n, grad_schema_err_t *err)
{
    for (int i = 0; i < n; i++) {
        cJSON *nd = cJSON_GetArrayItem(nodes, i);
        cJSON *id = cJSON_GetObjectItem(nd, "id");
        /* id 计划内唯一 */
        for (int j = i + 1; j < n; j++) {
            cJSON *oid = cJSON_GetObjectItem(cJSON_GetArrayItem(nodes, j), "id");
            if (strcmp(id->valuestring, oid->valuestring) == 0) {
                grad_serr_set(err, j, "id", "dup");
                return false;
            }
        }
        /* depends：逐项存在且无自环 */
        cJSON *deps = cJSON_GetObjectItem(nd, "depends");
        if (cJSON_IsArray(deps)) {
            int dc = cJSON_GetArraySize(deps);
            for (int d = 0; d < dc; d++) {
                cJSON *dep = cJSON_GetArrayItem(deps, d);
                int di = grad_find_node(nodes, dep->valuestring);
                if (di < 0) {
                    grad_serr_set(err, i, "depends", "missing_dep");
                    return false;
                }
                if (di == i) {
                    grad_serr_set(err, i, "depends", "self_dep");
                    return false;
                }
            }
        }
        /* inputs：producer ∈ depends 传递闭包（lit: 豁免） */
        cJSON *inputs = cJSON_GetObjectItem(nd, "inputs");
        if (cJSON_IsArray(inputs)) {
            int ic = cJSON_GetArraySize(inputs);
            for (int d = 0; d < ic; d++) {
                cJSON *it = cJSON_GetArrayItem(inputs, d);
                if (!grad_input_ok(nodes, i, it->valuestring, err))
                    return false;
            }
        }
        /* outputs：显式给出时计划内唯一 */
        cJSON *outs = cJSON_GetObjectItem(nd, "outputs");
        if (cJSON_IsArray(outs)) {
            int oc = cJSON_GetArraySize(outs);
            for (int a = 0; a < oc; a++) {
                for (int j = 0; j < n; j++) {
                    cJSON *jouts = cJSON_GetObjectItem(cJSON_GetArrayItem(nodes, j), "outputs");
                    if (!cJSON_IsArray(jouts))
                        continue;
                    int jc = cJSON_GetArraySize(jouts);
                    int from = (j == i) ? a + 1 : 0;
                    for (int b = from; b < jc; b++) {
                        cJSON *ja = cJSON_GetArrayItem(jouts, b);
                        if (strcmp(cJSON_GetArrayItem(outs, a)->valuestring,
                                   ja->valuestring) == 0) {
                            grad_serr_set(err, j, "outputs", "dup");
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

airy_err_t grad_plan_schema_validate(const cJSON *root, const char *plan_id,
                                     grad_schema_err_t *err)
{
    if (err) {
        err->index = -1;
        err->field[0] = '\0';
        err->reason[0] = '\0';
    }
    if (!root || !cJSON_IsObject(root)) {
        grad_serr_set(err, -1, "root", "type");
        return AIRY_EINVAL;
    }
    if (!grad_pat(plan_id, GRAD_PLAN_ID_MAX, true, true)) {
        grad_serr_set(err, -1, "plan_id", plan_id ? "pattern" : "missing");
        return AIRY_EINVAL;
    }
    cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
    if (!cJSON_IsArray(nodes)) {
        grad_serr_set(err, -1, "nodes", "missing");
        return AIRY_EINVAL;
    }
    int n = cJSON_GetArraySize(nodes);
    if (n < 1 || n > GRAD_MAX_NODES) {
        grad_serr_set(err, -1, "nodes", "range");
        return AIRY_EINVAL;
    }
    for (int i = 0; i < n; i++) {
        cJSON *nd = cJSON_GetArrayItem(nodes, i);
        /* 非对象节点单独定位：若交由 grad_check_node 处理，所有字段都
         * 「缺席」，违例会退化成无定位的整份拒绝，日志无法指认约束。 */
        if (!cJSON_IsObject(nd)) {
            grad_serr_set(err, i, "node", "type");
            return AIRY_EINVAL;
        }
        if (!grad_check_node(nd, err, i))
            return AIRY_EINVAL;
    }
    if (!grad_struct_ok(nodes, n, err))
        return AIRY_EINVAL;

    /* entry 挂在文档根（可选数组，引用存在的节点 id） */
    cJSON *entry = cJSON_GetObjectItem(root, "entry");
    if (entry) {
        if (!cJSON_IsArray(entry)) {
            grad_serr_set(err, -1, "entry", "type");
            return AIRY_EINVAL;
        }
        int ec = cJSON_GetArraySize(entry);
        for (int d = 0; d < ec; d++) {
            cJSON *ep = cJSON_GetArrayItem(entry, d);
            if (!cJSON_IsString(ep) || grad_find_node(nodes, ep->valuestring) < 0) {
                grad_serr_set(err, -1, "entry", "missing_ref");
                return AIRY_EINVAL;
            }
        }
    }
    return AIRY_SUCCESS;
}

#endif /* AIRY_HAS_CJSON */
