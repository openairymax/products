// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_grad_c12.c
 * @brief GRAD C12 上下文预算域单测（0.1.20 §5.2b.9）
 *
 * 覆盖：est 近似（ceil(utf8_bytes/3)）、resolve 派生公式（off/off-state
 * /典型窗口/小窗口）、k EWMA（噪声不更新、更新收敛、钳位 [1,16]）、
 * char_cap（off 回退 / 上限钳制 / 比例派生）、进程级并发闸（互斥计数、
 * 排队超时 EBUSY、cap 只增不减）。
 */

#include "test_grad_internal.h"

#include "grad_internal.h"

void test_ctxb_est(void)
{
    TEST_CHECK(grad_ctxb_est(NULL) == 0, "NULL est must be 0");
    TEST_CHECK(grad_ctxb_est("") == 0, "empty est must be 0");
    TEST_CHECK(grad_ctxb_est("abc") == 1, "3 bytes est = 1");
    TEST_CHECK(grad_ctxb_est("abcd") == 2, "4 bytes est = 2");
    TEST_CHECK(grad_ctxb_est("abcdef") == 2, "6 bytes est = 2");
    TEST_CHECK(grad_ctxb_est("abcdefg") == 3, "7 bytes est = 3");
}

void test_ctxb_resolve_off(void)
{
    grad_ctx_budget_t b;
    grad_ctxb_resolve(&b, 0, 4096);
    TEST_CHECK(b.n_ctx_slot == 0, "off keeps n_ctx_slot=0");
    TEST_CHECK(b.send_budget == 0, "off keeps send_budget=0");
    TEST_CHECK(grad_ctxb_active(&b) == 0, "off is inactive");
    TEST_CHECK(grad_ctxb_char_cap(&b) == GRAD_ARBITER_PROMPT_MAX_CHARS,
               "off falls back to legacy cap");
}

void test_ctxb_resolve(void)
{
    grad_ctx_budget_t b;
    grad_ctxb_resolve(&b, 8192, 4096);
    TEST_CHECK(grad_ctxb_active(&b) == 1, "8192 slot activates domain");
    TEST_CHECK(b.reserve_completion == 2048, "reserve = n/4 (4096 < 2048? no)");
    TEST_CHECK(b.prompt_budget == 6144, "prompt = 8192 - 2048");
    TEST_CHECK(b.send_budget == 5585, "send = 6144/1.1 floor");
    TEST_CHECK(b.k == 1.0, "initial k = 1.0");

    grad_ctx_budget_t s;
    grad_ctxb_resolve(&s, 100, 4096);
    TEST_CHECK(s.reserve_completion == 25, "reserve capped at n/4");
    TEST_CHECK(s.prompt_budget == 75, "prompt = 100 - 25");
    TEST_CHECK(s.send_budget == 68, "send = 75/1.1 floor");
}

void test_ctxb_k_update(void)
{
    grad_ctx_budget_t b;
    grad_ctxb_resolve(&b, 8192, 4096);

    /* 噪声不更新：dev = 5% ≤ 10% */
    grad_ctxb_k_update(&b, 100, 105);
    TEST_CHECK(b.k == 1.0, "small deviation keeps k");

    /* 更新：ratio=3.0, dev=200% → k = 0.8×1 + 0.2×3 = 1.4 */
    grad_ctxb_k_update(&b, 100, 300);
    TEST_CHECK(b.k > 1.399 && b.k < 1.401, "k EWMA converges to 1.4");
    TEST_CHECK(b.send_budget == 3989, "send recomputed = 6144/(1.4*1.1)");

    /* 零输入不更新（防御） */
    grad_ctx_budget_t c;
    grad_ctxb_resolve(&c, 8192, 4096);
    grad_ctxb_k_update(&c, 0, 300);
    grad_ctxb_k_update(&c, 100, 0);
    TEST_CHECK(c.k == 1.0, "zero est/actual ignored");

    /* 下钳位：ratio=0.5, k=0.8+0.1=0.9，被钳回 1.0 */
    grad_ctxb_k_update(&c, 100, 50);
    TEST_CHECK(c.k == 1.0, "k clamped low at 1.0");

    /* 上钳位：连续大 ratio，k 不得超 16 */
    for (int i = 0; i < 8; i++)
        grad_ctxb_k_update(&c, 10, 10000);
    TEST_CHECK(c.k == 16.0, "k clamped high at 16.0");
}

void test_ctxb_char_cap(void)
{
    grad_ctx_budget_t b;
    grad_ctxb_resolve(&b, 8192, 4096);
    /* send=5585 → cap 派生 16755 > 12000 → 钳 12000 */
    TEST_CHECK(grad_ctxb_char_cap(&b) == GRAD_ARBITER_PROMPT_MAX_CHARS,
               "derived cap clamped at 12000");

    grad_ctx_budget_t s;
    grad_ctxb_resolve(&s, 100, 4096);
    /* send=68 → cap = 204 */
    TEST_CHECK(grad_ctxb_char_cap(&s) == 204, "cap = send*3 in small window");
}

void test_ctxb_sem_gate(void)
{
    grad_ctxb_sem_init();
    grad_ctxb_sem_init(); /* 幂等 */

    TEST_CHECK(grad_ctxb_sem_acquire(1, 0, 50) == AIRY_SUCCESS, "first acquire ok");
    TEST_CHECK(grad_ctxb_sem_acquire(1, 0, 50) == AIRY_EBUSY, "cap=1 queue times out");
    grad_ctxb_sem_release();
    TEST_CHECK(grad_ctxb_sem_acquire(1, 0, 50) == AIRY_SUCCESS, "release reopens gate");
    grad_ctxb_sem_release();
}

void test_ctxb_sem_cap_grows(void)
{
    grad_ctxb_sem_init();

    /* cap 只增不减：1 → 4，同窗口并发占满后第 5 次超时 */
    TEST_CHECK(grad_ctxb_sem_acquire(4, 0, 50) == AIRY_SUCCESS, "slot 1");
    TEST_CHECK(grad_ctxb_sem_acquire(0, 0, 50) == AIRY_SUCCESS, "slot 2 (want=1 <= cap 4)");
    TEST_CHECK(grad_ctxb_sem_acquire(4, 0, 50) == AIRY_SUCCESS, "slot 3");
    TEST_CHECK(grad_ctxb_sem_acquire(4, 0, 50) == AIRY_SUCCESS, "slot 4");
    TEST_CHECK(grad_ctxb_sem_acquire(4, 0, 50) == AIRY_EBUSY, "slot 5 times out at cap 4");

    grad_ctxb_sem_release();
    TEST_CHECK(grad_ctxb_sem_acquire(4, 0, 50) == AIRY_SUCCESS, "released slot reusable");
    grad_ctxb_sem_release();
    grad_ctxb_sem_release();
    grad_ctxb_sem_release();
    grad_ctxb_sem_release();
}
