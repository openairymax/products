/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * canary_core.c - 金丝雀五态机、G30 准入、灰度阶梯与 kill switch。
 *
 * 状态转移经合法性表驱动（§7.0.2：C0→C1→C2→C3/C4，任意活动态可回滚）；
 * 热路径 route() 只消费原子态零 IO；kill switch 探测带 1s TTL 缓存，
 * 宿主低频轮询 canary_ks_hit() 并在命中时停止一切自演化写入。
 */
#include "canary_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 转移合法性表：[from][to]，行=当前态 列=目标态 */
static const uint8_t TRANS[6][6] = {
    /* N  P  S  C  A  R */
    { 0, 1, 0, 0, 0, 0 }, /* NONE → PROPOSED */
    { 0, 0, 1, 0, 0, 1 }, /* PROPOSED → SHADOW | ROLLED */
    { 0, 0, 0, 1, 0, 1 }, /* SHADOW → CANARY | ROLLED */
    { 0, 0, 0, 0, 1, 1 }, /* CANARY → ADOPTED | ROLLED */
    { 0, 0, 0, 0, 0, 1 }, /* ADOPTED → ROLLED */
    { 0, 0, 0, 0, 0, 0 }  /* ROLLED 终态 */
};

static const uint32_t STAGE_PM[CANARY_STAGES] = { 100u, 500u, 1000u };

static int move(canary_t *c, canary_state_t to)
{
    int from = atomic_load(&c->st);

    if (TRANS[from][to] == 0)
        return -1;
    atomic_store(&c->st, (int)to);
    return 0;
}

int canary_new(canary_t **out, const char *root, const char *module)
{
    canary_t *c;
    size_t rl;
    int n;

    if (!out || !root || !module || !*module)
        return -1;
    rl = strlen(root);
    if (strlen(module) >= CANARY_NAME_MAX || rl == 0)
        return -1;
    while (rl > 1 && root[rl - 1] == '/')
        rl--;
    c = calloc(1, sizeof(*c));
    if (!c)
        return -1;
    n = snprintf(c->overlay, sizeof(c->overlay), "%.*s/local/evolve/%s",
                 (int)rl, root, module);
    if (n < 0 || (size_t)n >= sizeof(c->overlay))
        goto fail;
    n = snprintf(c->sentry, sizeof(c->sentry), "%s/kill", c->overlay);
    if (n < 0 || (size_t)n >= sizeof(c->sentry))
        goto fail;
    snprintf(c->module, sizeof(c->module), "%s", module);
    atomic_init(&c->st, CANARY_ST_NONE);
    atomic_init(&c->stage, 0u);
    atomic_init(&c->permille, 0u);
    atomic_init(&c->ks_on, 0);
    atomic_init(&c->ks_until, 0ll);
    atomic_init(&c->n_total, 0ull);
    atomic_init(&c->n_picked, 0ull);
    *out = c;
    return 0;
fail:
    free(c);
    return -1;
}

void canary_free(canary_t *c)
{
    free(c);
}

int canary_propose(canary_t *c, const canary_blast_t *b)
{
    if (!c || !b || !b->module || !b->files || b->nfiles == 0)
        return -1;
    if (b->nfiles > CANARY_MAX_FILES || b->lines == 0 ||
        b->lines > CANARY_MAX_LINES)
        return -1;
    if (strcmp(b->module, c->module) != 0)
        return -1;
    return move(c, CANARY_ST_PROPOSED);
}

int canary_admit(size_t new_fn, size_t mod_fn)
{
    if (new_fn == 0 || new_fn > CANARY_FN_NEW_MAX)
        return -1;
    if (mod_fn == 0 || mod_fn > CANARY_FN_MOD_MAX)
        return -1;
    return 0;
}

int canary_gates(const canary_gates_t *g)
{
    if (!g)
        return -1;
    if (g->ok_pm < CANARY_OK_FLOOR)
        return -2; /* K1 */
    if (g->perf_ppm < CANARY_PERF_FLOOR)
        return -3; /* K2 */
    if (g->crashes != 0u)
        return -4; /* K3 */
    if (g->rb_secs > CANARY_RB_MAX_SECS)
        return -5; /* K4 */
    if ((g->flags & CANARY_G_EXPLAIN) == 0u)
        return -6; /* K5 */
    if ((g->flags & CANARY_G_BOUNDARY) == 0u)
        return -7; /* K6 */
    return 0;
}

int canary_to_shadow(canary_t *c)
{
    return c ? move(c, CANARY_ST_SHADOW) : -1;
}

int canary_to_canary(canary_t *c, uint64_t now_ms)
{
    if (!c)
        return -1;
    if (move(c, CANARY_ST_CANARY) != 0)
        return -1;
    c->stage_since = now_ms;
    atomic_store(&c->stage, 0u);
    atomic_store(&c->permille, STAGE_PM[0]);
    return 0;
}

int canary_step(canary_t *c, uint64_t now_ms)
{
    uint32_t s;

    if (!c || atomic_load(&c->st) != CANARY_ST_CANARY)
        return -1;
    s = atomic_load(&c->stage);
    if (s + 1u >= CANARY_STAGES)
        return 1;
    if (now_ms < c->stage_since + CANARY_DWELL_MS)
        return -2;
    c->stage_since = now_ms;
    s++;
    atomic_store(&c->stage, s);
    atomic_store(&c->permille, STAGE_PM[s]);
    return 0;
}

int canary_adopt(canary_t *c, const uint8_t hash[32], uint64_t now_ms)
{
    if (!c || !hash)
        return -1;
    if (atomic_load(&c->st) != CANARY_ST_CANARY)
        return -1;
    if (man_shift(c) != 0)
        return -1;
    if (man_write(c, "adopted", hash, now_ms) != 0)
        return -1;
    return move(c, CANARY_ST_ADOPTED);
}

int canary_rollback(canary_t *c)
{
    int rc;

    if (!c)
        return -1;
    rc = move(c, CANARY_ST_ROLLED);
    if (rc != 0)
        return rc;
    return man_restore(c);
}

int canary_verify(canary_t *c, const uint8_t hash[32])
{
    uint8_t cur[32];
    char state[CANARY_STATE_MAX];

    if (!c || !hash)
        return -1;
    if (man_read(c, state, sizeof(state), cur) != 0)
        return 0;
    if (memcmp(cur, hash, 32) == 0)
        return 0;
    if (man_mark(c, "replay_pending") != 0)
        return -1;
    return -2;
}

int canary_route(canary_t *c, uint32_t salt)
{
    atomic_fetch_add(&c->n_total, 1ull);
    if (atomic_load(&c->ks_on))
        return 0;
    int st = atomic_load(&c->st);
    if (st == CANARY_ST_ADOPTED) {
        atomic_fetch_add(&c->n_picked, 1ull);
        return 1;
    }
    if (st != CANARY_ST_CANARY)
        return 0;
    if (salt % 1000u >= atomic_load(&c->permille))
        return 0;
    atomic_fetch_add(&c->n_picked, 1ull);
    return 1;
}

canary_state_t canary_state(const canary_t *c)
{
    if (!c)
        return CANARY_ST_NONE;
    return (canary_state_t)atomic_load(&c->st);
}

uint32_t canary_permille(const canary_t *c)
{
    if (!c)
        return 0u;
    return atomic_load(&c->permille);
}

void canary_counts(const canary_t *c, uint64_t *total, uint64_t *picked)
{
    if (total)
        *total = c ? atomic_load(&c->n_total) : 0ull;
    if (picked)
        *picked = c ? atomic_load(&c->n_picked) : 0ull;
}

/* ---- kill switch 三通道 ---- */

static char g_ks_env[CANARY_NAME_MAX];
static char g_ks_file[CANARY_PATH_MAX];

void canary_ks_env(const char *name)
{
    if (!name || strlen(name) >= sizeof(g_ks_env))
        return;
    snprintf(g_ks_env, sizeof(g_ks_env), "%s", name);
}

void canary_ks_file(const char *path)
{
    if (!path || strlen(path) >= sizeof(g_ks_file))
        return;
    snprintf(g_ks_file, sizeof(g_ks_file), "%s", path);
}

static int ks_probe_file(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int ks_probe(const canary_t *c)
{
    if (g_ks_env[0] && getenv(g_ks_env))
        return 1;
    if (g_ks_file[0] && ks_probe_file(g_ks_file))
        return 1;
    return ks_probe_file(c->sentry);
}

int canary_ks_hit(canary_t *c, uint64_t now_ms)
{
    int hit;

    if (!c)
        return 0;
    if ((uint64_t)atomic_load(&c->ks_until) > now_ms)
        return atomic_load(&c->ks_on);
    hit = ks_probe(c);
    atomic_store(&c->ks_on, hit);
    atomic_store(&c->ks_until, (long long)now_ms + (long long)CANARY_KS_TTL_MS);
    return hit;
}

int canary_ks_cli(canary_t *c)
{
    FILE *f;

    if (!c)
        return -1;
    atomic_store(&c->ks_on, 1);
    canary_mkdir_p(c->overlay);
    f = fopen(c->sentry, "wb");
    if (!f)
        return -1;
    fputs("kill\n", f);
    fclose(f);
    return 0;
}
