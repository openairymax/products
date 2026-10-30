/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * canary_manifest.c - overlay manifest 持久化（§7.0.2.3 隔离规则）。
 *
 * manifest 落位 <root>/local/evolve/<module>/canary.manifest，行式文本：
 *   module=<name> / state=<态> / base_sha256=<64hex> / created_ms=<ts>
 *   / lines=<n|unmeasured> / fn_new=<n|unmeasured> / fn_mod=<n|unmeasured>
 * 写入经临时文件 + man_repl 覆盖式改名生效；采纳前现存 manifest 保留为
 * .prev（C4 回滚恢复链）；base 哈希失配 → state=replay_pending，
 * 禁止静默合并（标记路径透传原申报，账目不丢失）。
 */
#include "canary_internal.h"
#include "canary_sha256.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define CAN_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define CAN_MKDIR(p) mkdir(p, 0755)
#endif

#define MAN_NAME "canary.manifest"
#define MAN_PREV "canary.manifest.prev"

/*
 * 覆盖式改名原子件：src 存在时把 dst 替换为 src，缺位报 ENOENT。
 * POSIX rename 自带原子替换语义；Windows CRT rename 拒绝已存在的
 * dst（run 40975362676 canary_unit 回滚判负实证），须先 remove。
 * 两端 errno 语义一致：src 缺位为 ENOENT，dst 不可写为 EACCES。
 */
static int man_repl(const char *src, const char *dst)
{
#ifdef _WIN32
    remove(dst);
#endif
    return rename(src, dst);
}

static int man_path(const struct canary_s *c, char *buf, size_t cap,
                    const char *name)
{
    int n = snprintf(buf, cap, "%s/%s", c->overlay, name);

    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

int canary_mkdir_p(char *path)
{
    char *p;

    for (p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        CAN_MKDIR(path);
        *p = '/';
    }
    if (CAN_MKDIR(path) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

static int hex_dec(const char *in, uint8_t hash[32])
{
    size_t i;
    int hi, lo;

    if (strlen(in) != 64)
        return -1;
    for (i = 0; i < 32; i++) {
        hi = hex_val(in[i * 2]);
        lo = hex_val(in[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        hash[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* 0 = 未实测（unmeasured），非 0 写实测申报值 */
static const char *dcl_str(char *buf, size_t cap, size_t v)
{
    if (v == 0)
        return "unmeasured";
    snprintf(buf, cap, "%zu", v);
    return buf;
}

/* 声明字段解码：非数字（unmeasured）安全归 0 */
static size_t dcl_dec(const char *s)
{
    size_t v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (size_t)(*s++ - '0');
    return v;
}

int man_write(const struct canary_s *c, const char *state,
              const uint8_t hash[32], uint64_t ms)
{
    char path[CANARY_PATH_MAX];
    char tmp[CANARY_PATH_MAX];
    char hex[65];
    char dir[CANARY_PATH_MAX];
    char body[384];
    char ln[24], fn[24], fm[24]; /* size_t 十进制最长 20 位 */
    int n;
    FILE *f;

    if (man_path(c, path, sizeof(path), MAN_NAME) != 0)
        return -1;
    if (man_path(c, tmp, sizeof(tmp), MAN_NAME ".tmp") != 0)
        return -1;
    snprintf(dir, sizeof(dir), "%s", c->overlay);
    if (canary_mkdir_p(dir) != 0)
        return -1;
    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    sha256_hex(hash, hex);
    n = snprintf(body, sizeof(body),
                 "module=%s\nstate=%s\nbase_sha256=%s\ncreated_ms=%llu\n"
                 "lines=%s\nfn_new=%s\nfn_mod=%s\n",
                 c->module, state, hex, (unsigned long long)ms,
                 dcl_str(ln, sizeof(ln), c->dcl_lines),
                 dcl_str(fn, sizeof(fn), c->dcl_fn_new),
                 dcl_str(fm, sizeof(fm), c->dcl_fn_mod));
    if (n < 0 || (size_t)n >= sizeof(body) || fputs(body, f) == EOF) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    if (man_repl(tmp, path) != 0)
        return -1;
    return 0;
}

int man_read(const struct canary_s *c, char *state, size_t cap,
             uint8_t hash[32])
{
    return man_read_ms(c, state, cap, hash, NULL, NULL);
}

/* 取 "key=value" 行的 value 段：key 命中返回 1 并拷出 value，否则 0 */
static int man_field(const char *line, const char *key,
                     char *val, size_t cap)
{
    size_t n = strlen(key);
    size_t i = 0;

    if (strncmp(line, key, n) != 0 || line[n] != '=')
        return 0;
    line += n + 1;
    while (line[i] != '\0' && line[i] != '\n' && i + 1 < cap) {
        val[i] = line[i];
        i++;
    }
    val[i] = '\0';
    return 1;
}

int man_read_ms(const struct canary_s *c, char *state, size_t cap,
                uint8_t hash[32], uint64_t *ms_out, man_decl_t *dcl)
{
    char path[CANARY_PATH_MAX];
    char line[192];
    char hex[72];
    char mod[CANARY_NAME_MAX];
    char num[24];
    unsigned long long ms = 0ull;
    size_t dl = 0, dn = 0, dm = 0;
    int found = 0;
    FILE *f;

    if (!state || cap < CANARY_STATE_MAX)
        return -1;
    if (man_path(c, path, sizeof(path), MAN_NAME) != 0)
        return -1;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (man_field(line, "module", mod, sizeof(mod)))
            continue;
        if (man_field(line, "state", state, cap))
            found |= 1;
        else if (man_field(line, "base_sha256", hex, sizeof(hex)))
            found |= 2;
        else if (man_field(line, "created_ms", num, sizeof(num))) {
            ms = strtoull(num, NULL, 10);
            found |= 4;
        } else if (man_field(line, "lines", num, sizeof(num)))
            dl = dcl_dec(num);
        else if (man_field(line, "fn_new", num, sizeof(num)))
            dn = dcl_dec(num);
        else if (man_field(line, "fn_mod", num, sizeof(num)))
            dm = dcl_dec(num);
    }
    fclose(f);
    if ((found & 3) != 3 || hex_dec(hex, hash) != 0)
        return -1;
    if (ms_out)
        *ms_out = ms;
    if (dcl) {
        dcl->lines = dl;
        dcl->fn_new = dn;
        dcl->fn_mod = dm;
    }
    return 0;
}

int man_shift(const struct canary_s *c)
{
    char cur[CANARY_PATH_MAX];
    char prev[CANARY_PATH_MAX];

    if (man_path(c, cur, sizeof(cur), MAN_NAME) != 0 ||
        man_path(c, prev, sizeof(prev), MAN_PREV) != 0)
        return -1;
    if (man_repl(cur, prev) != 0 && errno != ENOENT)
        return -1;
    return 0;
}

int man_restore(struct canary_s *c)
{
    char cur[CANARY_PATH_MAX];
    char prev[CANARY_PATH_MAX];
    uint8_t hash[32];
    char state[CANARY_STATE_MAX];

    if (man_path(c, cur, sizeof(cur), MAN_NAME) != 0 ||
        man_path(c, prev, sizeof(prev), MAN_PREV) != 0)
        return -1;
    if (man_repl(prev, cur) == 0)
        return 0;
    if (errno != ENOENT)
        return -1;
    if (man_read(c, state, sizeof(state), hash) != 0)
        return 0;
    return man_mark(c, "rolled_back");
}

int man_mark(struct canary_s *c, const char *state)
{
    uint8_t hash[32];
    char old[CANARY_STATE_MAX];
    man_decl_t dcl;
    uint64_t ms = 0ull;

    if (man_read_ms(c, old, sizeof(old), hash, &ms, &dcl) != 0)
        return -1;
    /* 标记路径透传原申报，防重写丢失爆炸半径账目 */
    c->dcl_lines = dcl.lines;
    c->dcl_fn_new = dcl.fn_new;
    c->dcl_fn_mod = dcl.fn_mod;
    return man_write(c, state, hash, ms);
}
