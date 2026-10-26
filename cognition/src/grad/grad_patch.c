// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_patch.c
 * @brief GRAD C6 增量修补域（0.1.20 §5.2b.3）。
 *
 * B 驳回 verdict 的 defects[] → Δk 修补 scope → 模型 A 修补产物与原
 * 计划合并 → 合并产物序列化为 schema v1 视图供复验：
 *   - scope = unique(defects.node_id)，经 grad_compute_closure 一阶闭包
 *     （scope + 全部依赖前驱 + 直接后继，GRAD §6.4 微分熵减）；
 *   - 闭包上界 min(GRAD_PATCH_MAX_NODES, ceil(N/2))，超界返回 AIRY_E2BIG
 *     由协调器整份重绘（repair_mode=full）；
 *   - 空 defects / 节点不可定位 / 空 scope 一律 AIRY_EINVAL（G55：模板
 *     patch 不触发重绘，交由 C 硬判定收敛）。
 *
 * 合并语义（§5.2b.3 merge）：
 *   - frozen（scope 外）节点被 sub 触碰时必须逐字段精确相等，否则
 *     AIRY_ERR_STATE_ERROR（patch_inconsistent，本轮作废，允许下轮
 *     完整生成，禁止重试修补）；
 *   - scope 节点以 sub 版本整节点替换（允许变更）；sub 缺席则保留；
 *   - sub 新节点仅当落入 scope 才允许插入（E-01 插入生产者场景），
 *     否则 AIRY_EINVAL；
 *   - entry points 合并后重算：所有无依赖节点，不信赖原列表。
 *
 * grad_defect_t 字段为定长缓冲，由 verdict 解析方保证 NUL 终止；
 * 计划节点字符串均为 STRDUP 产物，strlen 安全。
 */

#include "grad_internal.h"

/* ---------------- 拼接缓冲（估算容量 + 溢出兜底） ---------------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int over;
} pbuf_t;

static void pbuf_init(pbuf_t *b, size_t cap)
{
    b->buf = (char *)AIRY_MALLOC(cap);
    b->cap = cap;
    b->len = 0;
    b->over = 0;
    if (b->buf)
        b->buf[0] = '\0';
}

static void pbuf_puts(pbuf_t *b, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (b->over || !b->buf)
        return;
    if (b->len + n + 1 > b->cap) {
        b->over = 1;
        return;
    }
    AIRY_MEMCPY(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void pbuf_putc(pbuf_t *b, char c)
{
    char one[2] = {c, '\0'};
    pbuf_puts(b, one);
}

static void pbuf_put_i64(pbuf_t *b, int64_t v)
{
    char tmp[24];
    (void)snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    pbuf_puts(b, tmp);
}

static void pbuf_put_u32(pbuf_t *b, uint32_t v)
{
    char tmp[16];
    (void)snprintf(tmp, sizeof(tmp), "%u", (unsigned)v);
    pbuf_puts(b, tmp);
}

/* JSON 字符串转义：引号/反斜杠/短转义 + 控制字符 \u00XX */
static void pbuf_puts_esc(pbuf_t *b, const char *s)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        const char *esc = NULL;
        switch (c) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        default:
            break;
        }
        if (esc) {
            pbuf_puts(b, esc);
            continue;
        }
        if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            char u[7] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xF], hex[c & 0xF], '\0'};
            pbuf_puts(b, u);
            continue;
        }
        pbuf_putc(b, (char)c);
    }
}

/* ---------------- 计划节点域 ---------------- */

static size_t node_index(const airy_task_plan_t *plan, const char *id)
{
    if (id) {
        for (size_t i = 0; i < plan->task_plan_node_count; i++) {
            const airy_task_node_t *tn = plan->task_plan_nodes[i];
            if (tn && tn->task_node_id && strcmp(tn->task_node_id, id) == 0)
                return i;
        }
    }
    return plan->task_plan_node_count;
}

static int node_in_scope(const airy_task_node_t *tn, const char *const *scope,
                         size_t scope_count)
{
    if (!tn || !tn->task_node_id)
        return 0;
    for (size_t k = 0; k < scope_count; k++)
        if (strcmp(scope[k], tn->task_node_id) == 0)
            return 1;
    return 0;
}

static void strarr_free(char **arr, size_t n)
{
    for (size_t i = 0; i < n; i++)
        AIRY_FREE(arr[i]);
    AIRY_FREE(arr);
}

static void node_free(airy_task_node_t *n)
{
    if (!n)
        return;
    AIRY_FREE(n->task_node_id);
    AIRY_FREE(n->task_node_handler_name);
    AIRY_FREE(n->task_node_goal);
    AIRY_FREE(n->task_node_agent_role);
    strarr_free(n->task_node_depends_on, n->task_node_depends_count);
    strarr_free(n->task_node_inputs, n->task_node_inputs_count);
    strarr_free(n->task_node_outputs, n->task_node_outputs_count);
    AIRY_FREE(n);
}

/* 深拷贝；任一分配失败整体回收返回 NULL（不产半成品节点） */
static airy_task_node_t *node_clone(const airy_task_node_t *src)
{
    if (!src)
        return NULL;
    airy_task_node_t *n = (airy_task_node_t *)AIRY_CALLOC(1, sizeof(*n));
    if (!n)
        return NULL;

    int ok = 1;
    if (src->task_node_id) {
        n->task_node_id = AIRY_STRDUP(src->task_node_id);
        n->task_node_id_len = n->task_node_id ? strlen(n->task_node_id) : 0;
        ok = n->task_node_id != NULL;
    }
    if (ok && src->task_node_handler_name) {
        n->task_node_handler_name = AIRY_STRDUP(src->task_node_handler_name);
        ok = n->task_node_handler_name != NULL;
    }
    if (ok && src->task_node_goal) {
        n->task_node_goal = AIRY_STRDUP(src->task_node_goal);
        ok = n->task_node_goal != NULL;
    }
    if (ok && src->task_node_agent_role) {
        n->task_node_agent_role = AIRY_STRDUP(src->task_node_agent_role);
        n->task_node_role_len = n->task_node_agent_role ? strlen(n->task_node_agent_role) : 0;
        ok = n->task_node_agent_role != NULL;
    }
    if (ok && src->task_node_depends_count) {
        size_t dc = src->task_node_depends_count;
        n->task_node_depends_on = (char **)AIRY_CALLOC(dc, sizeof(char *));
        ok = n->task_node_depends_on != NULL;
        for (size_t i = 0; ok && i < dc; i++) {
            char *dup = src->task_node_depends_on[i] ? AIRY_STRDUP(src->task_node_depends_on[i])
                                                     : NULL;
            if (dup)
                n->task_node_depends_on[n->task_node_depends_count++] = dup;
            else
                ok = 0;
        }
    }
    if (ok && src->task_node_inputs_count) {
        size_t ic = src->task_node_inputs_count;
        n->task_node_inputs = (char **)AIRY_CALLOC(ic, sizeof(char *));
        ok = n->task_node_inputs != NULL;
        for (size_t i = 0; ok && i < ic; i++) {
            char *dup =
                src->task_node_inputs[i] ? AIRY_STRDUP(src->task_node_inputs[i]) : NULL;
            if (dup)
                n->task_node_inputs[n->task_node_inputs_count++] = dup;
            else
                ok = 0;
        }
    }
    if (ok && src->task_node_outputs_count) {
        size_t oc = src->task_node_outputs_count;
        n->task_node_outputs = (char **)AIRY_CALLOC(oc, sizeof(char *));
        ok = n->task_node_outputs != NULL;
        for (size_t i = 0; ok && i < oc; i++) {
            char *dup =
                src->task_node_outputs[i] ? AIRY_STRDUP(src->task_node_outputs[i]) : NULL;
            if (dup)
                n->task_node_outputs[n->task_node_outputs_count++] = dup;
            else
                ok = 0;
        }
    }
    if (!ok) {
        node_free(n);
        return NULL;
    }
    n->task_node_timeout_ms = src->task_node_timeout_ms;
    n->task_node_priority = src->task_node_priority;
    n->task_node_cost_time_ms = src->task_node_cost_time_ms;
    n->task_node_cost_mem_mb = src->task_node_cost_mem_mb;
    n->task_node_invariant_guard = src->task_node_invariant_guard;
    return n;
}

static int str_eq(const char *a, const char *b)
{
    return (a == b) || (a && b && strcmp(a, b) == 0);
}

static int strarr_eq(char *const *a, size_t an, char *const *b, size_t bn)
{
    if (an != bn)
        return 0;
    for (size_t i = 0; i < an; i++)
        if (!str_eq(a[i], b[i]))
            return 0;
    return 1;
}

/* frozen 一致性判据：全字段精确相等（含 timeout_ms/priority，§5.2b.3） */
static int node_equal(const airy_task_node_t *a, const airy_task_node_t *b)
{
    return str_eq(a->task_node_id, b->task_node_id) &&
           str_eq(a->task_node_handler_name, b->task_node_handler_name) &&
           str_eq(a->task_node_goal, b->task_node_goal) &&
           str_eq(a->task_node_agent_role, b->task_node_agent_role) &&
           a->task_node_timeout_ms == b->task_node_timeout_ms &&
           a->task_node_priority == b->task_node_priority &&
           a->task_node_cost_time_ms == b->task_node_cost_time_ms &&
           a->task_node_cost_mem_mb == b->task_node_cost_mem_mb &&
           a->task_node_invariant_guard == b->task_node_invariant_guard &&
           strarr_eq(a->task_node_depends_on, a->task_node_depends_count,
                     b->task_node_depends_on, b->task_node_depends_count) &&
           strarr_eq(a->task_node_inputs, a->task_node_inputs_count, b->task_node_inputs,
                     b->task_node_inputs_count) &&
           strarr_eq(a->task_node_outputs, a->task_node_outputs_count, b->task_node_outputs,
                     b->task_node_outputs_count);
}

static size_t strarr_cap(char *const *arr, size_t n)
{
    size_t cap = 0;
    for (size_t i = 0; i < n; i++)
        cap += 4 + (arr[i] ? 6 * strlen(arr[i]) : 0);
    return cap;
}

static size_t s_len(const char *s)
{
    return s ? strlen(s) : 0;
}

/* ---------------- Δk 修补 scope（§5.2b.3） ---------------- */

airy_err_t grad_patch_scope(const airy_task_plan_t *plan, const grad_defect_t *defects,
                            size_t defect_count, char ***out_scope, size_t *out_count,
                            size_t *out_closure_size)
{
    if (!plan || !defects || defect_count == 0 || !out_scope || !out_count)
        return AIRY_EINVAL;
    *out_scope = NULL;
    *out_count = 0;
    if (out_closure_size)
        *out_closure_size = 0;

    char **uniq = (char **)AIRY_CALLOC(defect_count, sizeof(char *));
    if (!uniq)
        return AIRY_ENOMEM;
    size_t n = 0;
    airy_err_t ret = AIRY_SUCCESS;

    for (size_t i = 0; i < defect_count && ret == AIRY_SUCCESS; i++) {
        const char *id = defects[i].node_id;
        if (!id[0] || node_index(plan, id) == plan->task_plan_node_count) {
            ret = AIRY_EINVAL; /* G55 契约：defect 必须可定位到计划节点 */
            break;
        }
        int seen = 0;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(uniq[k], id) == 0) {
                seen = 1;
                break;
            }
        }
        if (seen)
            continue;
        char *dup = AIRY_STRDUP(id);
        if (!dup) {
            ret = AIRY_ENOMEM;
            break;
        }
        uniq[n++] = dup;
    }

    int *closure = NULL;
    size_t closure_count = 0;
    if (ret == AIRY_SUCCESS) {
        ret = grad_compute_closure(plan, (const char *const *)uniq, n, &closure,
                                   &closure_count);
        if (ret == AIRY_SUCCESS) {
            size_t cap = GRAD_PATCH_MAX_NODES;
            size_t half = (plan->task_plan_node_count + 1) / 2;
            if (half < cap)
                cap = half;
            /* cap==0（空计划）与超界同义：Δk 相对全图过大，整份重绘 */
            if (cap == 0 || closure_count > cap)
                ret = AIRY_E2BIG;
            else if (out_closure_size)
                *out_closure_size = closure_count;
        }
    }

    if (ret != AIRY_SUCCESS) {
        for (size_t k = 0; k < n; k++)
            AIRY_FREE(uniq[k]);
        AIRY_FREE(uniq);
        AIRY_FREE(closure);
        return ret;
    }
    AIRY_FREE(closure);
    *out_scope = uniq;
    *out_count = n;
    return AIRY_SUCCESS;
}

/* ---------------- B_Model 修补 patch JSON ---------------- */

airy_err_t grad_patch_build(uint32_t iteration, const grad_defect_t *defects,
                            size_t defect_count, const char *const *scope,
                            size_t scope_count, char **out_json)
{
    if (!defects || defect_count == 0 || !scope || scope_count == 0 || !out_json)
        return AIRY_EINVAL; /* G55：空 scope 模板 patch 一律拒绝 */
    *out_json = NULL;

    size_t cap = 256;
    for (size_t i = 0; i < scope_count; i++)
        cap += 8 + 6 * strlen(scope[i] ? scope[i] : "");
    for (size_t i = 0; i < defect_count; i++)
        cap += 128 + 6 * (strlen(defects[i].node_id) + strlen(defects[i].code) +
                          strlen(defects[i].evidence) + strlen(defects[i].suggestion));

    pbuf_t b;
    pbuf_init(&b, cap);
    if (!b.buf)
        return AIRY_ENOMEM;

    pbuf_puts(&b, "{\"patch_id\":\"B_");
    pbuf_put_u32(&b, iteration);
    pbuf_puts(&b, "\",\"source\":\"B_Model\",\"repair_mode\":\"incremental\","
                  "\"affected_scope\":[");
    for (size_t i = 0; i < scope_count; i++) {
        if (i)
            pbuf_putc(&b, ',');
        pbuf_putc(&b, '"');
        pbuf_puts_esc(&b, scope[i]);
        pbuf_putc(&b, '"');
    }
    pbuf_puts(&b, "],\"defects\":[");
    for (size_t i = 0; i < defect_count; i++) {
        if (i)
            pbuf_putc(&b, ',');
        pbuf_puts(&b, "{\"node_id\":\"");
        pbuf_puts_esc(&b, defects[i].node_id);
        pbuf_puts(&b, "\",\"code\":\"");
        pbuf_puts_esc(&b, defects[i].code);
        pbuf_puts(&b, "\",\"evidence\":\"");
        pbuf_puts_esc(&b, defects[i].evidence);
        pbuf_puts(&b, "\",\"suggestion\":\"");
        pbuf_puts_esc(&b, defects[i].suggestion);
        pbuf_puts(&b, "\"}");
    }
    pbuf_puts(&b, "]}");

    if (b.over) {
        AIRY_FREE(b.buf);
        return AIRY_EUNKNOWN;
    }
    *out_json = b.buf;
    return AIRY_SUCCESS;
}

/* ---------------- 计划合并（§5.2b.3 merge） ---------------- */

airy_err_t grad_plan_merge(const airy_task_plan_t *plan, const airy_task_plan_t *sub,
                           const char *const *scope, size_t scope_count,
                           airy_task_plan_t **out_merged)
{
    if (!plan || !sub || !scope || scope_count == 0 || !out_merged)
        return AIRY_EINVAL;
    *out_merged = NULL;

    airy_task_plan_t *m = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(*m));
    if (!m)
        return AIRY_ENOMEM;
    m->task_plan_id = AIRY_STRDUP(plan->task_plan_id ? plan->task_plan_id : "grad_plan");
    m->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC(
        plan->task_plan_node_count + sub->task_plan_node_count, sizeof(airy_task_node_t *));
    if (!m->task_plan_id || !m->task_plan_nodes) {
        airy_task_plan_free(m);
        return AIRY_ENOMEM;
    }

    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *pn = plan->task_plan_nodes[i];
        if (!pn || !pn->task_node_id) {
            airy_task_plan_free(m);
            return AIRY_EINVAL;
        }
        int in_scope = node_in_scope(pn, scope, scope_count);
        size_t si = node_index(sub, pn->task_node_id);
        const airy_task_node_t *use = pn;
        if (si != sub->task_plan_node_count) {
            if (!in_scope && !node_equal(pn, sub->task_plan_nodes[si])) {
                /* frozen 节点被修补产物篡改 → 本轮作废，禁止重试修补 */
                airy_task_plan_free(m);
                return AIRY_ERR_STATE_ERROR;
            }
            if (in_scope)
                use = sub->task_plan_nodes[si];
        }
        airy_task_node_t *cn = node_clone(use);
        if (!cn) {
            airy_task_plan_free(m);
            return AIRY_ENOMEM;
        }
        m->task_plan_nodes[m->task_plan_node_count++] = cn;
    }

    /* sub 独有节点：仅 scope 内允许（E-01 插入生产者场景），越权拒绝 */
    for (size_t j = 0; j < sub->task_plan_node_count; j++) {
        const airy_task_node_t *sn = sub->task_plan_nodes[j];
        if (!sn || !sn->task_node_id) {
            airy_task_plan_free(m);
            return AIRY_EINVAL;
        }
        if (node_index(plan, sn->task_node_id) != plan->task_plan_node_count)
            continue;
        if (!node_in_scope(sn, scope, scope_count)) {
            airy_task_plan_free(m);
            return AIRY_EINVAL;
        }
        airy_task_node_t *cn = node_clone(sn);
        if (!cn) {
            airy_task_plan_free(m);
            return AIRY_ENOMEM;
        }
        m->task_plan_nodes[m->task_plan_node_count++] = cn;
    }

    /* entry 重算：无依赖节点集合（合并边界自洽，不信赖原列表） */
    size_t entries = 0;
    for (size_t i = 0; i < m->task_plan_node_count; i++)
        if (m->task_plan_nodes[i]->task_node_depends_count == 0)
            entries++;
    if (entries) {
        m->task_plan_entry_points = (char **)AIRY_CALLOC(entries, sizeof(char *));
        if (!m->task_plan_entry_points) {
            airy_task_plan_free(m);
            return AIRY_ENOMEM;
        }
        for (size_t i = 0; i < m->task_plan_node_count; i++) {
            airy_task_node_t *tn = m->task_plan_nodes[i];
            if (tn->task_node_depends_count != 0)
                continue;
            char *dup = AIRY_STRDUP(tn->task_node_id);
            if (!dup) {
                airy_task_plan_free(m);
                return AIRY_ENOMEM;
            }
            m->task_plan_entry_points[m->task_plan_entry_count++] = dup;
        }
    }

    *out_merged = m;
    return AIRY_SUCCESS;
}

/* ---------------- schema v1 视图序列化 ---------------- */

airy_err_t grad_plan_to_json(const airy_task_plan_t *plan, char **out_json)
{
    if (!plan || !out_json)
        return AIRY_EINVAL;
    *out_json = NULL;

    size_t cap = 128;
    cap += 8 + 6 * strlen(plan->task_plan_id ? plan->task_plan_id : "grad_plan");
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *tn = plan->task_plan_nodes[i];
        if (!tn)
            continue;
        cap += 224;
        cap += 6 * (s_len(tn->task_node_id) + s_len(tn->task_node_goal) +
                    s_len(tn->task_node_agent_role));
        cap += strarr_cap(tn->task_node_depends_on, tn->task_node_depends_count);
        cap += strarr_cap(tn->task_node_inputs, tn->task_node_inputs_count);
        cap += strarr_cap(tn->task_node_outputs, tn->task_node_outputs_count);
    }

    pbuf_t b;
    pbuf_init(&b, cap);
    if (!b.buf)
        return AIRY_ENOMEM;

    pbuf_puts(&b, "{\"plan_id\":\"");
    pbuf_puts_esc(&b, plan->task_plan_id ? plan->task_plan_id : "grad_plan");
    pbuf_puts(&b, "\",\"nodes\":[");
    int first = 1;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *tn = plan->task_plan_nodes[i];
        if (!tn)
            continue;
        if (!first)
            pbuf_putc(&b, ',');
        first = 0;
        pbuf_puts(&b, "{\"id\":\"");
        pbuf_puts_esc(&b, tn->task_node_id ? tn->task_node_id : "");
        pbuf_puts(&b, "\",\"goal\":\"");
        pbuf_puts_esc(&b, tn->task_node_goal ? tn->task_node_goal : "");
        pbuf_puts(&b, "\",\"role\":\"");
        pbuf_puts_esc(&b, tn->task_node_agent_role ? tn->task_node_agent_role : "act");
        pbuf_puts(&b, "\",\"depends\":[");
        for (size_t k = 0; k < tn->task_node_depends_count; k++) {
            if (k)
                pbuf_putc(&b, ',');
            pbuf_putc(&b, '"');
            pbuf_puts_esc(&b, tn->task_node_depends_on[k]);
            pbuf_putc(&b, '"');
        }
        pbuf_puts(&b, "],\"inputs\":[");
        for (size_t k = 0; k < tn->task_node_inputs_count; k++) {
            if (k)
                pbuf_putc(&b, ',');
            pbuf_putc(&b, '"');
            pbuf_puts_esc(&b, tn->task_node_inputs[k]);
            pbuf_putc(&b, '"');
        }
        pbuf_puts(&b, "],\"outputs\":[");
        for (size_t k = 0; k < tn->task_node_outputs_count; k++) {
            if (k)
                pbuf_putc(&b, ',');
            pbuf_putc(&b, '"');
            pbuf_puts_esc(&b, tn->task_node_outputs[k]);
            pbuf_putc(&b, '"');
        }
        pbuf_putc(&b, ']');
        if (tn->task_node_cost_time_ms > 0) {
            pbuf_puts(&b, ",\"cost_time_ms\":");
            pbuf_put_i64(&b, tn->task_node_cost_time_ms);
        }
        if (tn->task_node_cost_mem_mb > 0) {
            pbuf_puts(&b, ",\"cost_mem_mb\":");
            pbuf_put_i64(&b, tn->task_node_cost_mem_mb);
        }
        pbuf_puts(&b, ",\"invariant_guard\":");
        pbuf_puts(&b, tn->task_node_invariant_guard ? "true" : "false");
        /* 0 视为未设置（parse 侧同义），序列化为缺省值保 schema 值域 */
        pbuf_puts(&b, ",\"timeout_ms\":");
        pbuf_put_u32(&b, tn->task_node_timeout_ms > 0 ? tn->task_node_timeout_ms
                                                      : GRAD_TIMEOUT_DEFAULT_MS);
        pbuf_puts(&b, ",\"priority\":");
        pbuf_put_u32(&b, tn->task_node_priority);
        pbuf_putc(&b, '}');
    }
    pbuf_puts(&b, "],\"entry\":[");
    first = 1;
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *tn = plan->task_plan_nodes[i];
        if (!tn || tn->task_node_depends_count != 0)
            continue;
        if (!first)
            pbuf_putc(&b, ',');
        first = 0;
        pbuf_putc(&b, '"');
        pbuf_puts_esc(&b, tn->task_node_id ? tn->task_node_id : "");
        pbuf_putc(&b, '"');
    }
    pbuf_puts(&b, "]}");

    if (b.over) {
        AIRY_FREE(b.buf);
        return AIRY_EUNKNOWN;
    }
    *out_json = b.buf;
    return AIRY_SUCCESS;
}
