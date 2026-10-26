/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * canary.h - 金丝雀自演化状态机公共 API（0.1.20 §7.0.2）。
 *
 * 五态机：C0 提案 → C1 影子 → C2 本地灰度（10%→50%→100%，每级 ≥2h）
 * → C3 采纳（写 local overlay manifest，记 base SHA-256）→ C4 回滚
 * （恢复上一 manifest，只回 overlay 不碰 base）。爆炸半径硬约束：
 * 一次演化 ≤1 模块 / ≤3 文件 / ≤200 行；G30 准入：新增函数 ≤150 行、
 * 修改后 ≤200 行。kill switch 三通道（环境变量 / 文件哨兵 / CLI），
 * 命中即停止一切自演化写入。base 变化 → manifest 标记待重放，
 * 禁止静默合并。
 *
 * 机制核定位：零第三方依赖（C11 + libc），纯机制不含策略；宿主
 * （如 dpolicy 引擎）经挂载点函数指针接线，挂载点缺席即旁路。
 * route() 为热路径多线程安全（C11 原子），状态转移函数须由宿主
 * 串行调用（commit 路径）。canary_ks_env()/canary_ks_file() 为
 * 进程级配置，须在并发使用前设置。
 */
#ifndef CANARY_H
#define CANARY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 爆炸半径硬约束（§7.0.2.2） */
#define CANARY_MAX_FILES 3
#define CANARY_MAX_LINES 200
/* G30 准入（演化补丁函数行数） */
#define CANARY_FN_NEW_MAX 150
#define CANARY_FN_MOD_MAX 200
/* C2 灰度阶梯与驻留（每级 ≥2h） */
#define CANARY_STAGES 3u
#define CANARY_DWELL_MS 7200000ull
/* 门禁阈值：K1 正确性 ≥99.9%；K2 性能保持 ≥95%（劣化 ≤5%）；
 * K4 回滚 ≤5min */
#define CANARY_OK_FLOOR 9990u
#define CANARY_PERF_FLOOR 9500u
#define CANARY_RB_MAX_SECS 300u

/* C0-C4 状态 */
typedef enum {
    CANARY_ST_NONE = 0,
    CANARY_ST_PROPOSED,
    CANARY_ST_SHADOW,
    CANARY_ST_CANARY,
    CANARY_ST_ADOPTED,
    CANARY_ST_ROLLED
} canary_state_t;

typedef struct canary_s canary_t;

/* 爆炸半径申报（C0 提案） */
typedef struct {
    const char *module;
    const char *const *files;
    size_t nfiles;
    size_t lines;
} canary_blast_t;

/* 门禁指标观测（K1-K6 定量面） */
typedef struct {
    uint32_t ok_pm;   /* K1 正确率 ‱ */
    uint32_t perf_ppm; /* K2 性能保持率 ‱（基线 10000） */
    uint32_t crashes; /* K3 观察窗崩溃数 */
    uint32_t rb_secs; /* K4 回滚实测秒 */
    uint32_t flags;   /* K5/K6 位 */
} canary_gates_t;

#define CANARY_G_EXPLAIN 1u
#define CANARY_G_BOUNDARY 2u

/* 创建实例：root 为 overlay 根目录，module 为目标模块名 */
int canary_new(canary_t **out, const char *root, const char *module);
void canary_free(canary_t *c);

/* C0 提案：校验爆炸半径硬约束（≤1 模块由实例构造保证） */
int canary_propose(canary_t *c, const canary_blast_t *b);

/* G30 准入：演化补丁函数行数静态校验（W6 canary 准入前执行） */
int canary_admit(size_t new_fn, size_t mod_fn);

/* 门禁判定：K1-K6 全绿返回 0，否则返回负错误码 */
int canary_gates(const canary_gates_t *g);

/* 状态推进（宿主串行调用） */
int canary_to_shadow(canary_t *c);
int canary_to_canary(canary_t *c);
/* C2 阶梯推进：≥2h 驻留后 10%→50%→100%；已满级返回 1 */
int canary_step(canary_t *c, uint64_t now_ms);

/* C3 采纳：.prev 保留上一 manifest，写 adopted + base SHA-256 */
int canary_adopt(canary_t *c, const uint8_t hash[32], uint64_t now_ms);
/* C4 回滚：恢复上一 manifest（无 .prev 则标记 rolled_back） */
int canary_rollback(canary_t *c);
/* base 一致性校验：与 manifest 记录的 base SHA-256 对比；
 * 失配返回 -2 并将 manifest 标记 replay_pending */
int canary_verify(canary_t *c, const uint8_t hash[32]);

/* 热路径路由：按当前态与灰度千分比采样（salt % 1000），
 * 返回 1 走演化路径；kill 命中或非灰度态恒 0 */
int canary_route(canary_t *c, uint32_t salt);

/* kill switch 三通道配置（进程级，须并发前设置；空串=禁用该通道） */
void canary_ks_env(const char *name);
void canary_ks_file(const char *path);
/* 三通道探测（TTL 1s 缓存）：宿主低频轮询并据此停止自演化写入 */
int canary_ks_hit(canary_t *c, uint64_t now_ms);
/* CLI 通道：立即熔断并持久化哨兵文件（重启后仍生效），单向 */
int canary_ks_cli(canary_t *c);

/* 观测面 */
canary_state_t canary_state(const canary_t *c);
uint32_t canary_permille(const canary_t *c);
void canary_counts(const canary_t *c, uint64_t *total, uint64_t *picked);

#ifdef __cplusplus
}
#endif

#endif /* CANARY_H */
