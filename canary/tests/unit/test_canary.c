/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * test_canary.c - 金丝雀状态机单元测试（真实文件 IO，无桩）。
 *
 * 覆盖：SHA-256 FIPS 180-4 已知向量；C0 提案爆炸半径边界；
 * G30 准入；K1-K6 门禁；五态机全序推进与 C2 灰度阶梯（≥2h 驻留）；
 * C3 采纳与 .prev 保留链；C4 回滚恢复；base 哈希失配 → 待重放；
 * kill switch 三通道（环境变量 / 文件哨兵 / CLI 持久化）；路由计数。
 */
#include "canary.h"
#include "canary_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define UT_GETPID _getpid
#define UT_UNLINK remove
#define UT_RMDIR _rmdir
#define UT_MKDIR(p) _mkdir(p)
#define UT_SETENV(k, v) _putenv_s(k, v)
#define UT_UNSETENV(k) _putenv_s(k, "")
#else
#include <sys/stat.h>
#include <unistd.h>
#define UT_GETPID getpid
#define UT_UNLINK remove
#define UT_RMDIR rmdir
#define UT_MKDIR(p) mkdir(p, 0755)
#define UT_SETENV(k, v) setenv(k, v, 1)
#define UT_UNSETENV(k) unsetenv(k)
#endif

static int g_fail = 0;

static void ut_fail(const char *file, long line, const char *cond)
{
    char msg[512];
    int n = snprintf(msg, sizeof(msg), "FAIL %s:%ld: %s\n",
                     file, line, cond);

    if (n > 0)
        fwrite(msg, 1, (size_t)n, stderr);
    g_fail++;
}

#define CHECK(cond)                                   \
    do {                                              \
        if (!(cond))                                  \
            ut_fail(__FILE__, __LINE__, #cond);       \
    } while (0)

static const char * const UT_FILES[2] = { "a.c", "b.c" };

static void tmp_root(char *buf, size_t cap)
{
    snprintf(buf, cap, "canary_ut_%ld", (long)UT_GETPID());
    UT_MKDIR(buf);
}

static void ut_cleanup(const char *root)
{
    char p[512];

    snprintf(p, sizeof(p), "%s/local/evolve/ut/canary.manifest", root);
    UT_UNLINK(p);
    snprintf(p, sizeof(p), "%s/local/evolve/ut/canary.manifest.prev", root);
    UT_UNLINK(p);
    snprintf(p, sizeof(p), "%s/local/evolve/ut/canary.manifest.tmp", root);
    UT_UNLINK(p);
    snprintf(p, sizeof(p), "%s/local/evolve/ut/kill", root);
    UT_UNLINK(p);
    snprintf(p, sizeof(p), "%s/local/evolve/ut", root);
    UT_RMDIR(p);
    snprintf(p, sizeof(p), "%s/local/evolve", root);
    UT_RMDIR(p);
    snprintf(p, sizeof(p), "%s/local", root);
    UT_RMDIR(p);
    UT_RMDIR(root);
}

static void test_sha256(void)
{
    uint8_t d[32];
    char hex[65];

    sha256_buf("", 0, d);
    sha256_hex(d, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb924"
                     "27ae41e4649b934ca495991b7852b855") == 0);
    sha256_buf("abc", 3, d);
    sha256_hex(d, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223"
                      "b00361a396177a9cb410ff61f20015ad") == 0);
    /* 分块输入与单块一致（update/final 路径） */
    {
        sha256_ctx ctx;
        uint8_t d2[32];

        sha256_init(&ctx);
        sha256_update(&ctx, "ab", 2);
        sha256_update(&ctx, "c", 1);
        sha256_final(&ctx, d2);
        CHECK(memcmp(d, d2, 32) == 0);
    }
}

static void test_propose_admit_gates(void)
{
    canary_t *c = NULL;
    canary_blast_t b;
    canary_gates_t g;

    CHECK(canary_new(&c, "/tmp/ut_x", "ut") == 0);
    b = (canary_blast_t){0};
    CHECK(canary_propose(c, NULL) == -1);
    b.module = "other";
    b.files = UT_FILES;
    b.nfiles = 2;
    b.lines = 100;
    CHECK(canary_propose(c, &b) == -1); /* 模块名不匹配 */
    b.module = "ut";
    CHECK(canary_propose(c, &b) == 0);
    CHECK(canary_state(c) == CANARY_ST_PROPOSED);
    b.nfiles = 4;
    CHECK(canary_propose(c, &b) == -1); /* >3 文件 */
    b.nfiles = 2;
    b.lines = 201;
    CHECK(canary_propose(c, &b) == -1); /* >200 行 */
    canary_free(c);

    CHECK(canary_admit(150, 200) == 0);
    CHECK(canary_admit(151, 200) == -1);
    CHECK(canary_admit(150, 201) == -1);
    CHECK(canary_admit(0, 200) == -1);

    g = (canary_gates_t){0};
    g.ok_pm = 9990;
    g.perf_ppm = 9500;
    g.rb_secs = 300;
    g.flags = CANARY_G_EXPLAIN | CANARY_G_BOUNDARY;
    CHECK(canary_gates(&g) == 0);
    g.ok_pm = 9989;
    CHECK(canary_gates(&g) == -2);
    g.ok_pm = 9990;
    g.perf_ppm = 9499;
    CHECK(canary_gates(&g) == -3);
    g.perf_ppm = 9500;
    g.crashes = 1;
    CHECK(canary_gates(&g) == -4);
    g.crashes = 0;
    g.rb_secs = 301;
    CHECK(canary_gates(&g) == -5);
    g.rb_secs = 300;
    g.flags = CANARY_G_BOUNDARY;
    CHECK(canary_gates(&g) == -6);
    g.flags = CANARY_G_EXPLAIN;
    CHECK(canary_gates(&g) == -7);
    CHECK(canary_gates(NULL) == -1);
}

static void test_flow(void)
{
    char root[128];
    canary_t *c = NULL;
    canary_blast_t b;
    canary_gates_t g;
    uint8_t h1[32], h2[32];
    uint64_t total = 0, picked = 0;

    tmp_root(root, sizeof(root));
    ut_cleanup(root);
    CHECK(canary_new(&c, root, "ut") == 0);
    b = (canary_blast_t){0};
    b.module = "ut";
    b.files = UT_FILES;
    b.nfiles = 2;
    b.lines = 100;
    CHECK(canary_propose(c, &b) == 0);
    CHECK(canary_adopt(c, h1, 0) == -1); /* 非 C2 态不可采纳 */
    CHECK(canary_to_shadow(c) == 0);
    CHECK(canary_to_canary(c, 0) == 0);
    CHECK(canary_permille(c) == 100);
    CHECK(canary_step(c, 100) == -2); /* 驻留不足 2h */
    CHECK(canary_step(c, CANARY_DWELL_MS + 1) == 0);
    CHECK(canary_permille(c) == 500);
    CHECK(canary_step(c, CANARY_DWELL_MS + 2) == -2);
    CHECK(canary_step(c, 2 * CANARY_DWELL_MS + 2) == 0);
    CHECK(canary_permille(c) == 1000);
    CHECK(canary_step(c, 9 * CANARY_DWELL_MS) == 1); /* 满级 */
    CHECK(canary_route(c, 999) == 1);

    g = (canary_gates_t){0};
    g.ok_pm = 10000;
    g.perf_ppm = 9800;
    g.rb_secs = 60;
    g.flags = CANARY_G_EXPLAIN | CANARY_G_BOUNDARY;
    CHECK(canary_gates(&g) == 0);
    sha256_buf("base-v1", 7, h1);
    CHECK(canary_adopt(c, h1, 42) == 0);
    CHECK(canary_state(c) == CANARY_ST_ADOPTED);
    CHECK(canary_route(c, 0) == 1); /* 采纳后全量 */
    CHECK(canary_verify(c, h1) == 0);
    sha256_buf("base-v2", 7, h2);
    CHECK(canary_verify(c, h2) == -2); /* 失配 → 待重放 */
    CHECK(canary_verify(c, h2) == -2);
    canary_counts(c, &total, &picked);
    CHECK(total >= 2 && picked >= 2);

    canary_free(c);
    ut_cleanup(root);
}

static void test_rollback(void)
{
    char root[128];
    canary_t *c = NULL;
    canary_blast_t b;
    uint8_t h1[32], h2[32];

    tmp_root(root, sizeof(root));
    ut_cleanup(root);
    CHECK(canary_new(&c, root, "ut") == 0);
    CHECK(canary_rollback(c) == -1); /* NONE 态无演化可回退，转移表正确拒绝 */
    CHECK(canary_state(c) == CANARY_ST_NONE);
    canary_free(c);

    CHECK(canary_new(&c, root, "ut") == 0);
    b = (canary_blast_t){0};
    b.module = "ut";
    b.files = UT_FILES;
    b.nfiles = 1;
    b.lines = 50;
    CHECK(canary_propose(c, &b) == 0);
    CHECK(canary_to_shadow(c) == 0);
    CHECK(canary_rollback(c) == 0);
    CHECK(canary_state(c) == CANARY_ST_ROLLED);
    canary_free(c);

    /* C3 采纳后回滚：.prev 链恢复（两次采纳 → 回滚回第一次） */
    CHECK(canary_new(&c, root, "ut") == 0);
    b.nfiles = 2;
    CHECK(canary_propose(c, &b) == 0);
    CHECK(canary_to_shadow(c) == 0 && canary_to_canary(c, 0) == 0);
    sha256_buf("v1", 2, h1);
    CHECK(canary_adopt(c, h1, 1) == 0);
    canary_free(c);

    CHECK(canary_new(&c, root, "ut") == 0);
    CHECK(canary_state(c) == CANARY_ST_NONE); /* 新实例态重置 */
    CHECK(canary_propose(c, &b) == 0);
    CHECK(canary_to_shadow(c) == 0 && canary_to_canary(c, 0) == 0);
    sha256_buf("v2", 2, h2);
    CHECK(canary_adopt(c, h2, 2) == 0);
    CHECK(canary_verify(c, h2) == 0);
    CHECK(canary_rollback(c) == 0);
    CHECK(canary_state(c) == CANARY_ST_ROLLED);
    /* 回滚恢复上一 manifest（.prev = v1 采纳记录，经 verify 证明） */
    CHECK(canary_verify(c, h1) == 0);
    canary_free(c);
    ut_cleanup(root);
}

static void test_killswitch(const char *root)
{
    canary_t *c = NULL;
    canary_t *d = NULL;
    char path[512];

    canary_ks_env("");
    canary_ks_file("");

    CHECK(canary_new(&c, root, "ut") == 0);
    CHECK(canary_ks_hit(c, 1000) == 0);
    CHECK(canary_route(c, 0) == 0); /* NONE 态不引流 */

    /* 通道一：环境变量 */
    UT_SETENV("CANARY_UT_KILL", "1");
    canary_ks_env("CANARY_UT_KILL");
    CHECK(canary_ks_hit(c, 5000) == 1);
    CHECK(canary_route(c, 0) == 0); /* kill 命中即断流 */
    UT_UNSETENV("CANARY_UT_KILL");
    CHECK(canary_ks_hit(c, 9000) == 0); /* TTL 过后解除 */

    /* 通道二：全局哨兵文件 */
    snprintf(path, sizeof(path), "%s/global-sentry", root);
    canary_ks_file(path);
    CHECK(canary_ks_hit(c, 20000) == 0);
    {
        FILE *f = fopen(path, "wb");

        CHECK(f != NULL);
        fputs("x", f);
        fclose(f);
    }
    CHECK(canary_ks_hit(c, 30000) == 1);
    UT_UNLINK(path);
    canary_ks_file("");

    /* 通道三：CLI 持久化（新实例重启后仍命中） */
    CHECK(canary_ks_cli(c) == 0);
    CHECK(canary_ks_hit(c, 40000) == 1);
    CHECK(canary_new(&d, root, "ut") == 0);
    CHECK(canary_ks_hit(d, 40000) == 1); /* 哨兵跨实例生效 */
    canary_free(d);
    canary_free(c);
    canary_ks_env("");
}

/* 真实时钟锚定回归：to_canary 以宿主传入时钟为驻留零点，
 * 防止锚点清零导致首级立即放行的缺陷复发 */
static void test_dwell_anchor(void)
{
    canary_t *c = NULL;
    canary_blast_t b;

    CHECK(canary_new(&c, "/tmp/ut_dwell", "ut") == 0);
    b = (canary_blast_t){0};
    b.module = "ut";
    b.files = UT_FILES;
    b.nfiles = 1;
    b.lines = 10;
    CHECK(canary_propose(c, &b) == 0);
    CHECK(canary_to_shadow(c) == 0);
    CHECK(canary_to_canary(c, 1000) == 0);
    CHECK(canary_step(c, 1000) == -2);
    CHECK(canary_step(c, 1000 + CANARY_DWELL_MS - 1) == -2);
    CHECK(canary_step(c, 1000 + CANARY_DWELL_MS) == 0);
    CHECK(canary_permille(c) == 500);
    CHECK(canary_step(c, 1000 + CANARY_DWELL_MS) == -2);
    CHECK(canary_step(c, 1000 + 2 * CANARY_DWELL_MS) == 0);
    CHECK(canary_step(c, 1000 + 9 * CANARY_DWELL_MS) == 1);
    canary_free(c);
}

static void test_route_sampling(void)
{
    canary_t *c = NULL;
    canary_blast_t b;
    uint32_t salt;
    uint64_t total = 0, picked = 0;

    CHECK(canary_new(&c, "/tmp/ut_route", "ut") == 0);
    b = (canary_blast_t){0};
    b.module = "ut";
    b.files = UT_FILES;
    b.nfiles = 1;
    b.lines = 10;
    CHECK(canary_propose(c, &b) == 0);
    CHECK(canary_to_shadow(c) == 0);
    CHECK(canary_to_canary(c, 0) == 0);
    for (salt = 0; salt < 1000u; salt++)
        CHECK(canary_route(c, salt) == (salt < 100u));
    canary_counts(c, &total, &picked);
    CHECK(total == 1000 && picked == 100);
    canary_free(c);
}

int main(void)
{
    char root[128];

    test_sha256();
    test_propose_admit_gates();
    test_flow();
    test_rollback();
    tmp_root(root, sizeof(root));
    test_killswitch(root);
    ut_cleanup(root);
    test_dwell_anchor();
    test_route_sampling();

    if (g_fail != 0) {
        char msg[64];
        int n = snprintf(msg, sizeof(msg),
                         "canary unit: %d failure(s)\n", g_fail);

        if (n > 0)
            fwrite(msg, 1, (size_t)n, stderr);
        return 1;
    }
    fputs("canary unit: all tests passed\n", stdout);
    return 0;
}
