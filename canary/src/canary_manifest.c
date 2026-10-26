/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * canary_manifest.c - overlay manifest 持久化（§7.0.2.3 隔离规则）。
 *
 * manifest 落位 <root>/local/evolve/<module>/canary.manifest，行式文本：
 *   module=<name> / state=<态> / base_sha256=<64hex> / created_ms=<ts>
 * 写入经临时文件 + rename 原子生效；采纳前现存 manifest 保留为
 * .prev（C4 回滚恢复链）；base 哈希失配 → state=replay_pending，
 * 禁止静默合并。
 */
#include "canary_internal.h"
#include "canary_sha256.h"

#include <errno.h>
#include <stdio.h>
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

int man_write(const struct canary_s *c, const char *state,
              const uint8_t hash[32], uint64_t ms)
{
    char path[CANARY_PATH_MAX];
    char tmp[CANARY_PATH_MAX];
    char hex[65];
    char dir[CANARY_PATH_MAX];
    FILE *f;

    if (man_path(c, path, sizeof(path), MAN_NAME) != 0)
        return -1;
    if (man_path(c, tmp, sizeof(tmp), MAN_NAME ".tmp") != 0)
        return -1;
    memcpy(dir, c->overlay, strlen(c->overlay) + 1);
    if (canary_mkdir_p(dir) != 0)
        return -1;
    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    sha256_hex(hash, hex);
    fprintf(f, "module=%s\nstate=%s\nbase_sha256=%s\ncreated_ms=%llu\n",
            c->module, state, hex, (unsigned long long)ms);
    if (fclose(f) != 0)
        return -1;
    if (remove(path) != 0 && errno != ENOENT)
        return -1;
    if (rename(tmp, path) != 0)
        return -1;
    return 0;
}

int man_read(const struct canary_s *c, char *state, size_t cap,
             uint8_t hash[32])
{
    return man_read_ms(c, state, cap, hash, NULL);
}

int man_read_ms(const struct canary_s *c, char *state, size_t cap,
                uint8_t hash[32], uint64_t *ms_out)
{
    char path[CANARY_PATH_MAX];
    char line[192];
    char hex[72];
    char mod[CANARY_NAME_MAX];
    unsigned long long ms = 0ull;
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
        if (sscanf(line, "module=%63s", mod) == 1)
            continue;
        if (sscanf(line, "state=%15s", state) == 1)
            found |= 1;
        else if (sscanf(line, "base_sha256=%71s", hex) == 1)
            found |= 2;
        else if (sscanf(line, "created_ms=%llu", &ms) == 1)
            found |= 4;
    }
    fclose(f);
    if ((found & 3) != 3 || hex_dec(hex, hash) != 0)
        return -1;
    if (ms_out)
        *ms_out = ms;
    return 0;
}

int man_shift(const struct canary_s *c)
{
    char cur[CANARY_PATH_MAX];
    char prev[CANARY_PATH_MAX];

    if (man_path(c, cur, sizeof(cur), MAN_NAME) != 0 ||
        man_path(c, prev, sizeof(prev), MAN_PREV) != 0)
        return -1;
    if (rename(cur, prev) != 0 && errno != ENOENT)
        return -1;
    return 0;
}

int man_restore(const struct canary_s *c)
{
    char cur[CANARY_PATH_MAX];
    char prev[CANARY_PATH_MAX];
    uint8_t hash[32];
    char state[CANARY_STATE_MAX];

    if (man_path(c, cur, sizeof(cur), MAN_NAME) != 0 ||
        man_path(c, prev, sizeof(prev), MAN_PREV) != 0)
        return -1;
    if (rename(prev, cur) == 0)
        return 0;
    if (errno != ENOENT)
        return -1;
    if (man_read(c, state, sizeof(state), hash) != 0)
        return 0;
    return man_mark(c, "rolled_back");
}

int man_mark(const struct canary_s *c, const char *state)
{
    uint8_t hash[32];
    char old[CANARY_STATE_MAX];
    uint64_t ms = 0ull;

    if (man_read_ms(c, old, sizeof(old), hash, &ms) != 0)
        return -1;
    return man_write(c, state, hash, ms);
}
