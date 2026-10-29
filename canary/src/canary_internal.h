/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * canary_internal.h - 金丝雀状态机内部结构（模块私有）。
 */
#ifndef CANARY_INTERNAL_H
#define CANARY_INTERNAL_H

#include "canary.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define CANARY_NAME_MAX 64
#define CANARY_PATH_MAX 512
#define CANARY_STATE_MAX 16
#define CANARY_KS_TTL_MS 1000ull

struct canary_s {
    atomic_int st;          /* canary_state_t */
    atomic_uint stage;      /* C2 阶梯 0..2 */
    atomic_uint permille;   /* 灰度千分比 */
    atomic_int ks_on;       /* kill switch 命中态 */
    atomic_llong ks_until;  /* kill switch 探测缓存到期时刻 ms */
    atomic_ullong n_total;  /* route 总调用数 */
    atomic_ullong n_picked; /* route 演化路径数 */
    uint64_t stage_since;   /* 当前级进入时刻（转移路径串行写） */
    size_t dcl_lines;       /* 爆炸半径申报（propose 拷贝；0=unmeasured） */
    size_t dcl_fn_new;
    size_t dcl_fn_mod;
    char module[CANARY_NAME_MAX];
    char overlay[CANARY_PATH_MAX]; /* <root>/local/evolve/<module> */
    char sentry[CANARY_PATH_MAX];  /* CLI 通道哨兵文件 */
};

/* manifest 路径：内联私有状态文件 */
typedef struct {
    size_t lines;   /* 0 = unmeasured */
    size_t fn_new;
    size_t fn_mod;
} man_decl_t;

int man_write(const struct canary_s *c, const char *state,
              const uint8_t hash[32], uint64_t ms);
int man_read(const struct canary_s *c, char *state, size_t cap,
             uint8_t hash[32]);
int man_read_ms(const struct canary_s *c, char *state, size_t cap,
                uint8_t hash[32], uint64_t *ms_out, man_decl_t *dcl);
int man_shift(const struct canary_s *c);
int man_restore(struct canary_s *c);
int man_mark(struct canary_s *c, const char *state);

/* 路径工具（core 与 manifest 共用） */
int canary_mkdir_p(char *path);

#endif /* CANARY_INTERNAL_H */
