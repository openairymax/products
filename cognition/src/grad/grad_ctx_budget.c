// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_ctx_budget.c
 * @brief GRAD C12 上下文预算域（0.1.20 §5.2b.9）
 *
 * 由服务端 context window（launch.n_ctx_slot）派生单次发送预算：
 *   reserve_completion = min(req_max_tokens, n_ctx_slot/4)
 *   prompt_budget      = n_ctx_slot - reserve_completion
 *   send_budget        = prompt_budget / (k × 1.1)
 * est 以 ceil(utf8_bytes/3) 近似 prompt token 数；k 为 est→真实
 * prompt token 的膨胀系数（EWMA α=0.2，钳位 [1,16]，偏差 ≤10% 不
 * 更新以免噪声抖动）。n_ctx_slot=0 时整个域关闭：预算与裁剪回退
 * GRAD_ARBITER_PROMPT_MAX_CHARS 旧行为，并发闸不设限——D2-9 接线前
 * 全字段为 0，零行为变化（回滚开关）。
 *
 * 进程级并发闸（inflight）按 min(max_concurrent, server_parallel)
 * 收口 GRAD 的 LLM 出口并发：排队超出 queue_ms 返回 AIRY_EBUSY
 * （fail-closed，与超预算同语义，上游采纳模型 C 确定性判定）。
 * 单例初始化幂等：daemon prepare 期先行（单线程），grad_run 装配
 * 点防御性重入。
 */

#include "grad_internal.h"

#include <string.h>

/* ── token 估算与预算派生 ─────────────────────────────────────────── */

size_t grad_ctxb_est(const char *s)
{
    if (!s || !s[0])
        return 0;
    return (strlen(s) + 2) / 3;
}

int grad_ctxb_active(const grad_ctx_budget_t *b)
{
    return b && b->n_ctx_slot > 0;
}

static void ctxb_refresh(grad_ctx_budget_t *b)
{
    double sb = (double)b->prompt_budget / (b->k * 1.1);
    b->send_budget = (sb < 1.0) ? 1u : (uint32_t)sb;
}

void grad_ctxb_resolve(grad_ctx_budget_t *b, uint32_t n_ctx_slot, uint32_t req_max_tokens)
{
    if (!b)
        return;
    __builtin_memset(b, 0, sizeof(*b));
    if (n_ctx_slot == 0)
        return;
    b->n_ctx_slot = n_ctx_slot;
    b->k = 1.0;
    b->reserve_completion =
        (req_max_tokens < n_ctx_slot / 4) ? req_max_tokens : n_ctx_slot / 4;
    b->prompt_budget = n_ctx_slot - b->reserve_completion;
    ctxb_refresh(b);
}

void grad_ctxb_k_update(grad_ctx_budget_t *b, size_t est, uint32_t actual)
{
    if (!grad_ctxb_active(b) || est == 0 || actual == 0)
        return;
    /* ratio 与 k 同量纲（est→真实 token 的倍率），偏差以 k 为基准。 */
    double ratio = (double)actual / (double)est;
    double dev = (ratio > b->k) ? (ratio - b->k) / b->k : (b->k - ratio) / b->k;
    if (dev <= 0.10)
        return;
    b->k = b->k * 0.8 + ratio * 0.2;
    if (b->k < 1.0)
        b->k = 1.0;
    if (b->k > 16.0)
        b->k = 16.0;
    ctxb_refresh(b);
}

size_t grad_ctxb_char_cap(const grad_ctx_budget_t *b)
{
    if (!grad_ctxb_active(b))
        return GRAD_ARBITER_PROMPT_MAX_CHARS;
    double cap = (double)b->send_budget * 3.0;
    if (cap >= (double)GRAD_ARBITER_PROMPT_MAX_CHARS)
        return GRAD_ARBITER_PROMPT_MAX_CHARS;
    return (cap < 1.0) ? 1u : (size_t)cap;
}

/* ── 进程级 LLM 出口并发闸 ────────────────────────────────────────── */

typedef struct grad_inflight {
    airy_mtx_t mtx;
    int ready;    /**< mtx 已初始化（daemon prepare 单线程置位） */
    int32_t cap;  /**< 并发上限（只增不减：多 run 装配幂等） */
    int32_t held; /**< 当前持闸数 */
} grad_inflight_t;

static grad_inflight_t g_inflight;

void grad_ctxb_sem_init(void)
{
    if (g_inflight.ready)
        return;
    if (airy_mtx_init(&g_inflight.mtx) == 0)
        g_inflight.ready = 1;
}

airy_err_t grad_ctxb_sem_acquire(uint32_t max_concurrent, uint32_t server_parallel,
                                 uint32_t queue_ms)
{
    if (!g_inflight.ready)
        return AIRY_EFAIL;
    uint32_t want = max_concurrent ? max_concurrent : 1;
    if (server_parallel && server_parallel < want)
        want = server_parallel;

    if (airy_mtx_lock(&g_inflight.mtx) != 0)
        return AIRY_EFAIL;
    if (want > (uint32_t)g_inflight.cap)
        g_inflight.cap = (int32_t)want;
    airy_mtx_unlock(&g_inflight.mtx);

    uint64_t deadline = airy_time_monotonic_ns() + (uint64_t)queue_ms * 1000000ull;
    for (;;) {
        int ok = 0;
        if (airy_mtx_lock(&g_inflight.mtx) != 0)
            return AIRY_EFAIL;
        if (g_inflight.held < g_inflight.cap) {
            g_inflight.held++;
            ok = 1;
        }
        airy_mtx_unlock(&g_inflight.mtx);
        if (ok)
            return AIRY_SUCCESS;
        if (airy_time_monotonic_ns() >= deadline)
            return AIRY_EBUSY;
        airy_sleep_ms(10);
    }
}

void grad_ctxb_sem_release(void)
{
    if (!g_inflight.ready)
        return;
    if (airy_mtx_lock(&g_inflight.mtx) != 0)
        return;
    if (g_inflight.held > 0)
        g_inflight.held--;
    airy_mtx_unlock(&g_inflight.mtx);
}
