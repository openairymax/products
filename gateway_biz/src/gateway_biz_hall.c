// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_hall.c
 * @brief Gateway hall.* namespace: task board / event stream / decision chain.
 *
 * Unlike the other namespaces (pure <ns>.<method> forwarding to a daemon),
 * hall.* is implemented directly inside the gateway: it reads the persisted
 * single-source-of-truth files that the runtime writes under AIRY_HOME —
 *   - work hall board : $AIRY_HOME/data/agentrt/state/work_hall_state.json
 *   - hall store      : $AIRY_HOME/data/agentrt/hall/<tenant>/<task>/<cat>/events.json
 * and merges in the live agent roster from agent_d (agent.list). Any client
 * (Rust TUI / C CLI / HTTP) therefore sees the same board / event stream /
 * decision chain without owning the in-process hall handle.
 *
 * Event ordering: files are named {tenant}.{task}.{category}.{ts_utc}.{seq}.json;
 * seq counts within one category directory, so it is not a global order key.
 * gseq is the ledger-wide monotonic write counter and resumes from a persisted
 * watermark across process restarts, so the replay/stream order is
 * (ts_utc, gseq) — with seq as the fallback for legacy files lacking gseq.
 * ts_utc is UTC YYYYMMDDTHHMMSSmmm (fixed width, lexicographically ordered).
 *
 * Methods:
 *   hall.board   ()                        -> {entries, agents}
 *   hall.tasks   ({limit?})                -> {tasks, total}
 *   hall.replay  ({task_id, category?})    -> {task_id, events, total}
 *   hall.stream  ({limit?})                -> {events, total}
 */

#include "gateway_biz_internal.h"

#include "gateway_hall_store.h"
#include "syscalls.h"

#include "hall_event.h"
#include "io.h"
#include "logging.h"
#include "platform.h"
#include "airy_dirent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW_HALL_STATE_REL "agentrt/state/work_hall_state.json"
#define GW_HALL_DEFAULT_STREAM_LIMIT 512
#define GW_HALL_MAX_LIMIT 8192

/* ── file I/O helpers ─────────────────────────────────────────────── */

static void gw_hall_state_path(char *buf, size_t cap)
{
    /* 收敛到 data/agentrt/state（AIRY_HOME 路径体系），与写入方
     * work_hall_persist.c 一致。 */
    const char *data = airy_data_dir();
    if (data && data[0])
        snprintf(buf, cap, "%s/%s", data, GW_HALL_STATE_REL);
    else
        snprintf(buf, cap, "%s", GW_HALL_STATE_REL);
}

static void gw_hall_root(char *buf, size_t cap)
{
    const char *data = airy_data_dir();
    if (data && data[0])
        snprintf(buf, cap, "%s/%s", data, HALL_EVT_ROOT_REL);
    else
        snprintf(buf, cap, "%s", HALL_EVT_ROOT_REL);
}

/* ── JSON-RPC result envelope (shared by hall.* methods) ──────────── */

static char *gw_hall_respond(const cJSON *id, cJSON *result)
{
    cJSON *out = cJSON_CreateObject();
    if (!out) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id))
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    else
        cJSON_AddNullToObject(out, "id");
    cJSON_AddItemToObject(out, "result", result);
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s;
}

/* ── growable event array ─────────────────────────────────────────── */

typedef struct {
    char ts_utc[HALL_EVT_TS_LEN]; /* sort key 1 (fixed width, lexical) */
    unsigned long seq;            /* per-category counter (legacy fallback) */
    unsigned long long gseq;      /* sort key 2: ledger-wide write order */
    char *json;                   /* compact event JSON (OWNER) */
} gw_hall_evt_t;

typedef struct {
    gw_hall_evt_t *items;
    size_t count;
    size_t cap;
} gw_hall_evt_list_t;

static int gw_hall_evt_reserve(gw_hall_evt_list_t *l, size_t extra)
{
    if (l->count + extra <= l->cap)
        return 0;
    size_t ncap = l->cap ? l->cap * 2 : 64;
    while (ncap < l->count + extra)
        ncap *= 2;
    gw_hall_evt_t *ni = (gw_hall_evt_t *)AIRY_REALLOC(l->items, ncap * sizeof(*ni));
    if (!ni)
        return -1;
    l->items = ni;
    l->cap = ncap;
    return 0;
}

static void gw_hall_evt_push(gw_hall_evt_list_t *l, const char *ts_utc, unsigned long seq,
                             unsigned long long gseq, char *json)
{
    if (gw_hall_evt_reserve(l, 1) != 0) {
        AIRY_FREE(json);
        return;
    }
    gw_hall_evt_t *e = &l->items[l->count++];
    AIRY_STRNCPY_TERM(e->ts_utc, ts_utc ? ts_utc : "", sizeof(e->ts_utc));
    e->seq = seq;
    e->gseq = gseq;
    e->json = json;
}

/* Ordering: (ts_utc, gseq). gseq is the ledger-wide write counter, so it is
 * the only key that orders events written into different category
 * directories within the same millisecond. Legacy files without gseq fall
 * back to their per-directory seq (still correct within one category). */
static int gw_hall_evt_before(const gw_hall_evt_t *a, const gw_hall_evt_t *b)
{
    int c = strcmp(a->ts_utc, b->ts_utc);
    if (c != 0)
        return c < 0;
    unsigned long long ga = a->gseq ? a->gseq : (unsigned long long)a->seq;
    unsigned long long gb = b->gseq ? b->gseq : (unsigned long long)b->seq;
    return ga < gb;
}

/* insertion sort by (ts_utc, gseq) — hall event counts are small (KBs) */
static void gw_hall_evt_sort(gw_hall_evt_list_t *l)
{
    for (size_t i = 1; i < l->count; i++) {
        gw_hall_evt_t key = l->items[i];
        size_t j = i;
        while (j > 0 && gw_hall_evt_before(&key, &l->items[j - 1]))
            j--;
        if (j != i) {
            for (size_t k = i; k > j; k--)
                l->items[k] = l->items[k - 1];
            l->items[j] = key;
        }
    }
}

static void gw_hall_evt_free_all(gw_hall_evt_list_t *l)
{
    for (size_t i = 0; i < l->count; i++)
        AIRY_FREE(l->items[i].json);
    AIRY_FREE(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

/* Scan one hall file: parse its name + build the event. Returns 0 when the
 * file contributed an event, -1 otherwise (skipped). */
static int gw_hall_collect_file(const char *path, const char *name, gw_hall_evt_list_t *out)
{
    hall_evt_parts_t parts;
    if (hall_evt_parse(name, &parts) != 0)
        return -1;
    char ts_utc[HALL_EVT_TS_LEN];
    size_t tlen = parts.ts_utc_len;
    if (tlen >= sizeof(ts_utc))
        tlen = sizeof(ts_utc) - 1;
    AIRY_MEMCPY(ts_utc, parts.ts_utc, tlen);
    ts_utc[tlen] = '\0';

    char *json = hall_evt_flat_file(path);
    if (!json)
        return -1;
    /* gseq lives in the file header; the flatten step puts it ahead of
     * `content`, so the ledger-wide write counter is available without a
     * second parse. */
    gw_hall_evt_push(out, ts_utc, hall_evt_seq(name), hall_evt_gseq_of(json), json);
    return 0;
}

/* Recursively scan the hall root. Directory layout:
 *   <root>/<tenant>/<task>/<category>/events.json   (depth 0=root,1=tenant,2=task,3=cat)
 * `tenant_filter`/`task_filter`/`cat_filter` are optional exact matches
 * (NULL = any); files are collected into `out`. */

typedef struct {
    int depth;
    const char *tf, *kf, *cf;
    gw_hall_evt_list_t *out;
} gw_hall_walk_ctx_t;

static void gw_hall_walk_visit(const char *dir, const char *name, const char *sub, void *ud)
{
    gw_hall_walk_ctx_t *x = ud;
    if (x->depth == 3) {
        gw_hall_collect_file(sub, name, x->out);
        return;
    }
    /* depth 0/1/2: subdirectories (tenant/task/category) */
    if ((x->depth == 0 && x->tf && strcmp(name, x->tf) != 0) ||
        (x->depth == 1 && x->kf && strcmp(name, x->kf) != 0) ||
        (x->depth == 2 && x->cf && strcmp(name, x->cf) != 0))
        return;
    gw_hall_walk_ctx_t nx = *x;
    nx.depth++;
    gw_hall_dir_walk(sub, gw_hall_walk_visit, &nx);
}

static void gw_hall_walk(const char *dir, int depth, const char *tenant_filter,
                         const char *task_filter, const char *cat_filter, gw_hall_evt_list_t *out)
{
    gw_hall_walk_ctx_t x = { depth, tenant_filter, task_filter, cat_filter, out };
    gw_hall_dir_walk(dir, gw_hall_walk_visit, &x);
}

/* ── method implementations ───────────────────────────────────────── */

static cJSON *gw_hall_entry_find(cJSON *entries, const char *exec_id)
{
    for (cJSON *e = entries->child; e; e = e->next) {
        const cJSON *id = cJSON_GetObjectItem(e, "execution_id");
        if (cJSON_IsString(id) && strcmp(id->valuestring, exec_id) == 0)
            return e;
    }
    return NULL;
}

/* 工作大厅只认任务账本的四个类别。hall 事件目录同时承载网关自身的
 * 其他审计流（如 PEP 能力判定 category=cap、task_id=gateway），它们不是
 * 任务，派生进看板会留下一条永远 running 的幽灵条目。 */
static bool gw_hall_board_is_task_category(const cJSON *cat)
{
    if (!cJSON_IsString(cat))
        return false;
    const char *c = cat->valuestring;
    return strcmp(c, "blueprint") == 0 || strcmp(c, "progress") == 0 ||
           strcmp(c, "issue") == 0 || strcmp(c, "result") == 0;
}

/* 看板事件回退（0.1.21 L-9）：work_hall_state.json 只记被调度器派发的任务，
 * 同步 agent.run 不经过调度、失败与取消此前也不落终态事件，看板因此恒空
 * 或恒挂 running。board 为空时按 hall 事件派生：同一 execution_id 以最新
 * run_result 定终态，未出现终态即为 running。字段对齐 TUI 的
 * HallBoardEntry 契约，不塞类型不匹配的数值 task_id。 */
static void gw_hall_board_fill(cJSON *entries)
{
    char root[GW_HALL_PATH_MAX];
    gw_hall_root(root, sizeof(root));

    gw_hall_evt_list_t evts = {0};
    gw_hall_walk(root, 0, NULL, NULL, NULL, &evts);
    gw_hall_evt_sort(&evts);

    for (size_t i = 0; i < evts.count; i++) {
        cJSON *e = cJSON_Parse(evts.items[i].json);
        if (!e)
            continue;
        /* 事件在 hall.* 出口已由 hall_evt_flat_file 摊平：task_id 与
         * content 都在顶层，没有 "file" 信封（replay/stream 同形态）。 */
        const cJSON *content = cJSON_GetObjectItem(e, "content");
        const cJSON *tid = cJSON_GetObjectItem(e, "task_id");
        if (!cJSON_IsString(tid) || !tid->valuestring[0] ||
            !gw_hall_board_is_task_category(cJSON_GetObjectItem(e, "category"))) {
            cJSON_Delete(e);
            continue;
        }

        cJSON *ent = gw_hall_entry_find(entries, tid->valuestring);
        if (!ent) {
            ent = cJSON_CreateObject();
            if (!ent) {
                cJSON_Delete(e);
                break;
            }
            cJSON_AddStringToObject(ent, "execution_id", tid->valuestring);
            cJSON_AddStringToObject(ent, "workflow_name", "agent.run");
            cJSON_AddStringToObject(ent, "state", "running");
            cJSON_AddNumberToObject(ent, "progress", 0);
            cJSON_AddItemToArray(entries, ent);
        }

        const cJSON *ev = cJSON_GetObjectItem(content, "event");
        if (!cJSON_IsString(ev) || !ev->valuestring) {
            cJSON_Delete(e);
            continue;
        }

        /* C-18：账本同源后，终态由 agent_d 的 task_state 事件给出
         * （completed/failed/cancelled），progress 事件给出过程进度；
         * run_result 仅作旧数据兼容。事件已按 (ts_utc, gseq) 排序，
         * 后者自然覆盖前者。 */
        if (strcmp(ev->valuestring, "task_state") == 0) {
            const cJSON *st = cJSON_GetObjectItem(content, "state");
            const cJSON *pg = cJSON_GetObjectItem(content, "progress");
            if (cJSON_IsString(st) && st->valuestring) {
                const char *state = st->valuestring;
                bool terminal = strcmp(state, "completed") == 0 ||
                                strcmp(state, "failed") == 0 ||
                                strcmp(state, "cancelled") == 0;
                cJSON_ReplaceItemInObject(ent, "state", cJSON_CreateString(
                                                              terminal ? state : "running"));
                cJSON_ReplaceItemInObject(
                    ent, "progress",
                    cJSON_CreateNumber(cJSON_IsNumber(pg) ? pg->valuedouble : (terminal ? 1 : 0)));
            }
            cJSON_Delete(e);
            continue;
        }
        if (strcmp(ev->valuestring, "issue") == 0) {
            /* 失败先落 issue、终态随后到达；这里只保证看板不会漏掉失败。 */
            const cJSON *st = cJSON_GetObjectItem(ent, "state");
            if (!cJSON_IsString(st) || strcmp(st->valuestring, "running") == 0)
                cJSON_ReplaceItemInObject(ent, "state", cJSON_CreateString("failed"));
            cJSON_Delete(e);
            continue;
        }
        if (strcmp(ev->valuestring, "run_result") != 0) {
            cJSON_Delete(e);
            continue;
        }
        const cJSON *st = cJSON_GetObjectItem(content, "state");
        const cJSON *rc = cJSON_GetObjectItem(content, "rc");
        const char *state = cJSON_IsString(st) ? st->valuestring :
                            (cJSON_IsNumber(rc) && rc->valuedouble == 0 ? "completed" : "failed");
        cJSON_ReplaceItemInObject(ent, "state", cJSON_CreateString(state));
        cJSON_ReplaceItemInObject(ent, "progress",
                                  cJSON_CreateNumber(strcmp(state, "running") == 0 ? 0 : 1));
        cJSON_Delete(e);
    }
    gw_hall_evt_free_all(&evts);
}

static char *gw_hall_board(const gateway_business_ctx_t *ctx, const cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    cJSON_AddItemToObject(result, "entries", entries);
    cJSON_AddStringToObject(result, "source", GW_HALL_STATE_REL);

    char state_path[GW_HALL_PATH_MAX];
    gw_hall_state_path(state_path, sizeof(state_path));
    char *raw = NULL;
    if (airy_io_read_file(state_path, &raw, NULL) == 0 && raw) {
        cJSON *state = cJSON_Parse(raw);
        AIRY_FREE(raw);
        if (state) {
            cJSON *board = cJSON_GetObjectItem(state, "board");
            if (cJSON_IsArray(board)) {
                int n = cJSON_GetArraySize(board);
                for (int i = 0; i < n; i++) {
                    cJSON *e = cJSON_GetArrayItem(board, i);
                    cJSON *dup = cJSON_Duplicate(e, 1);
                    if (dup)
                        cJSON_AddItemToArray(entries, dup);
                }
            }
            cJSON_Delete(state);
        }
    }

    /* C-18：看板与 replay 同源。state 文件只记被调度器派发的任务，而
     * 同步 task.submit 不经过调度，因此事件派生必须始终执行并与 state
     * 合并（state 条目优先建条目，事件补齐状态/进度），不能只在空板时
     * 回退——否则一份陈旧 state 会把新任务全部遮住。 */
    bool from_state = cJSON_GetArraySize(entries) > 0;
    gw_hall_board_fill(entries);
    if (!from_state && cJSON_GetArraySize(entries) > 0)
        cJSON_ReplaceItemInObject(result, "source", cJSON_CreateString("hall_events"));
    else if (from_state && cJSON_GetArraySize(entries) > 0)
        cJSON_ReplaceItemInObject(result, "source",
                                  cJSON_CreateString(GW_HALL_STATE_REL "+hall_events"));

    /* Live agent roster from agent_d (best effort; failure -> empty list) */
    cJSON *agents = cJSON_CreateArray();
    cJSON_AddItemToObject(result, "agents", agents);
    if (ctx) {
        /* 架构约束 2026-08-25 "必须走 syscall": agent.list 经 SYS_SVC_CALL 派发 */
        char *resp = NULL;
        airy_err_t rc = airy_sys_svc_call("agent", "list", "{}", GW_TOOL_TIMEOUT_MS, &resp);
        if (rc == AIRY_SUCCESS && resp) {
            cJSON *r = cJSON_Parse(resp);
            AIRY_FREE(resp);
            if (r) {
                cJSON *res = cJSON_GetObjectItem(r, "result");
                if (cJSON_IsObject(res)) {
                    cJSON *ids = cJSON_GetObjectItem(res, "agent_ids");
                    if (cJSON_IsArray(ids)) {
                        int n = cJSON_GetArraySize(ids);
                        for (int i = 0; i < n; i++) {
                            cJSON *it = cJSON_GetArrayItem(ids, i);
                            if (cJSON_IsString(it))
                                cJSON_AddItemToArray(agents, cJSON_Duplicate(it, 1));
                        }
                    }
                    cJSON *total = cJSON_GetObjectItem(res, "total");
                    if (cJSON_IsNumber(total))
                        cJSON_AddNumberToObject(result, "agent_total", total->valuedouble);
                }
                cJSON_Delete(r);
            }
        }
    }

    return gw_hall_respond(id, result);
}

static char *gw_hall_tasks(const cJSON *params, const cJSON *id)
{
    char root[GW_HALL_PATH_MAX];
    gw_hall_root(root, sizeof(root));

    /* task map: task_id -> {tenant, latest_ts, event_count} */
    typedef struct {
        char task[128];
        char tenant[64];
        char latest_ts[HALL_EVT_TS_LEN];
        size_t event_count;
    } gw_task_t;
    gw_task_t *tasks = NULL;
    size_t nt = 0, cap = 0;

    /* walk tenant -> task dirs directly (files are one level deeper) */
    DIR *d = opendir(root);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;
            char tdir[GW_HALL_PATH_MAX];
            snprintf(tdir, sizeof(tdir), "%s/%s", root, ent->d_name);
            DIR *td = opendir(tdir);
            if (!td)
                continue;
            struct dirent *tent;
            while ((tent = readdir(td)) != NULL) {
                if (tent->d_name[0] == '.')
                    continue;
                /* <task>/<category>/<file>.json */
                char catdir[GW_HALL_PATH_MAX];
                snprintf(catdir, sizeof(catdir), "%s/%s", tdir, tent->d_name);
                DIR *cd = opendir(catdir);
                if (!cd)
                    continue;
                struct dirent *c;
                while ((c = readdir(cd)) != NULL) {
                    if (c->d_name[0] == '.')
                        continue;
                    char filedir[GW_HALL_PATH_MAX];
                    snprintf(filedir, sizeof(filedir), "%s/%s", catdir, c->d_name);
                    DIR *fd = opendir(filedir);
                    if (!fd)
                        continue;
                    struct dirent *f;
                    while ((f = readdir(fd)) != NULL) {
                        if (f->d_name[0] == '.')
                            continue;
                        hall_evt_parts_t parts;
                        if (hall_evt_parse(f->d_name, &parts) != 0)
                            continue;
                        /* find or create the task entry */
                        size_t idx = nt;
                        for (size_t i = 0; i < nt; i++) {
                            if (strcmp(tasks[i].task, tent->d_name) == 0) {
                                idx = i;
                                break;
                            }
                        }
                        if (idx == nt) {
                            if (nt == cap) {
                                cap = cap ? cap * 2 : 16;
                                gw_task_t *nta =
                                    (gw_task_t *)AIRY_REALLOC(tasks, cap * sizeof(*tasks));
                                if (!nta) {
                                    closedir(fd);
                                    closedir(cd);
                                    closedir(td);
                                    closedir(d);
                                    goto tasks_done;
                                }
                                tasks = nta;
                            }
                            tasks[nt].latest_ts[0] = '\0';
                            tasks[nt].event_count = 0;
                            AIRY_STRNCPY_TERM(tasks[nt].task, tent->d_name,
                                              sizeof(tasks[nt].task));
                            AIRY_STRNCPY_TERM(tasks[nt].tenant, ent->d_name,
                                              sizeof(tasks[nt].tenant));
                            nt++;
                        }
                        tasks[idx].event_count++;
                        char ts[HALL_EVT_TS_LEN];
                        size_t tlen = parts.ts_utc_len;
                        if (tlen >= sizeof(ts))
                            tlen = sizeof(ts) - 1;
                        AIRY_MEMCPY(ts, parts.ts_utc, tlen);
                        ts[tlen] = '\0';
                        if (strcmp(ts, tasks[idx].latest_ts) > 0)
                            AIRY_STRNCPY_TERM(tasks[idx].latest_ts, ts,
                                              sizeof(tasks[idx].latest_ts));
                    }
                    closedir(fd);
                }
                closedir(cd);
            }
            closedir(td);
        }
        closedir(d);
    }

tasks_done:
    /* newest first */
    for (size_t i = 1; i < nt; i++) {
        gw_task_t key = tasks[i];
        size_t j = i;
        while (j > 0 && strcmp(tasks[j - 1].latest_ts, key.latest_ts) < 0)
            j--;
        if (j != i) {
            for (size_t k = i; k > j; k--)
                tasks[k] = tasks[k - 1];
            tasks[j] = key;
        }
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < nt; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "tenant_id", tasks[i].tenant);
        cJSON_AddStringToObject(t, "task_id", tasks[i].task);
        cJSON_AddStringToObject(t, "latest_ts", tasks[i].latest_ts);
        cJSON_AddNumberToObject(t, "event_count", (double)tasks[i].event_count);
        cJSON_AddItemToArray(arr, t);
    }
    AIRY_FREE(tasks);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tasks", arr);
    cJSON_AddNumberToObject(result, "total", (double)nt);
    cJSON_AddStringToObject(result, "root", root);

    return gw_hall_respond(id, result);
}

static char *gw_hall_replay(const cJSON *params, const cJSON *id)
{
    const char *task_id = NULL;
    const char *category = NULL;
    if (params) {
        cJSON *t = cJSON_GetObjectItem(params, "task_id");
        if (cJSON_IsString(t) && t->valuestring && *t->valuestring)
            task_id = t->valuestring;
        cJSON *c = cJSON_GetObjectItem(params, "category");
        if (cJSON_IsString(c) && c->valuestring && *c->valuestring)
            category = c->valuestring;
    }
    if (!task_id) {
        return jsonrpc_error(-32602, "Invalid params: missing task_id", id);
    }

    char root[GW_HALL_PATH_MAX];
    gw_hall_root(root, sizeof(root));

    gw_hall_evt_list_t evts = {0};
    gw_hall_walk(root, 0, NULL, task_id, category, &evts);
    gw_hall_evt_sort(&evts);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < evts.count; i++) {
        cJSON *e = cJSON_Parse(evts.items[i].json);
        if (e) {
            cJSON_AddItemToArray(arr, e);
        }
    }
    size_t total = evts.count;
    gw_hall_evt_free_all(&evts);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "task_id", task_id);
    cJSON_AddItemToObject(result, "events", arr);
    cJSON_AddNumberToObject(result, "total", (double)total);

    return gw_hall_respond(id, result);
}

static char *gw_hall_stream(const cJSON *params, const cJSON *id)
{
    size_t limit = GW_HALL_DEFAULT_STREAM_LIMIT;
    if (params) {
        cJSON *l = cJSON_GetObjectItem(params, "limit");
        if (cJSON_IsNumber(l) && l->valuedouble > 0) {
            limit = (size_t)l->valuedouble;
            if (limit > GW_HALL_MAX_LIMIT)
                limit = GW_HALL_MAX_LIMIT;
        }
    }

    char root[GW_HALL_PATH_MAX];
    gw_hall_root(root, sizeof(root));

    gw_hall_evt_list_t evts = {0};
    gw_hall_walk(root, 0, NULL, NULL, NULL, &evts);
    gw_hall_evt_sort(&evts);

    size_t take = evts.count < limit ? evts.count : limit;
    size_t start = evts.count - take; /* newest `limit` entries */

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = start; i < evts.count; i++) {
        cJSON *e = cJSON_Parse(evts.items[i].json);
        if (e)
            cJSON_AddItemToArray(arr, e);
    }
    size_t total = evts.count;
    gw_hall_evt_free_all(&evts);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "events", arr);
    cJSON_AddNumberToObject(result, "total", (double)total);

    return gw_hall_respond(id, result);
}

/**
 * @brief hall.* method dispatch (implemented in the gateway itself).
 */
char *handle_hall_call(cJSON *root, gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method) || !method->valuestring)
        return jsonrpc_error(-32600, "Invalid Request", id);

    if (strcmp(method->valuestring, "hall.board") == 0)
        return gw_hall_board(ctx, id);
    if (strcmp(method->valuestring, "hall.tasks") == 0)
        return gw_hall_tasks(params, id);
    if (strcmp(method->valuestring, "hall.replay") == 0)
        return gw_hall_replay(params, id);
    if (strcmp(method->valuestring, "hall.stream") == 0)
        return gw_hall_stream(params, id);

    return jsonrpc_error(-32601, "Method not found", id);
}
