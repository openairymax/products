// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * mech_memory_absent.c - standalone 形态 memory 机制接缝的缺席态闭合单元
 * （0.1.19 M5-4 §271）
 *
 * 工程语义：products/cognition standalone 形态（未纳入 agentrt 超级构建）
 * 无机制核（airy_coreloopthree 缺席），而 MC/TC 载荷表（payload_registry）
 * 的 memory 协作槽位（src/foundation/tc/tc_memory.c 的
 * airy_tc_step_write_to_memory 等）引用 memory 机制接口符号——链接期须
 * 闭合，运行期不得伪实现。
 *
 * 本单元只实现「机制缺席」这一真实部署形态的防御语义，逐符号镜像
 * atoms coreloopthree src/memory/engine.c 对空引擎句柄的判空早退分支：
 *   - engine 句柄为 NULL（standalone 下 chain->memory 恒 NULL）→
 *     AIRY_EINVAL（参数防御，与 engine.c 同判）；
 *   - 非空句柄（standalone 不可达，防御保留）→ AIRY_ENOSYS（机制实现
 *     未装配，契约级缺席语义，与 reflective plan() 的 tc/mc 缺席同款）。
 * 不提供任何伪造的存储行为；嵌入机制核后本单元整体退出闭包（embedded
 * 由 airy_coreloopthree 的 engine.c 提供真实现，本单元不参与编译）。
 */

#include "memory.h"

airy_err_t airy_memory_write(airy_memory_engine_t *engine,
                             const airy_memory_record_t *record, char **out_record_id)
{
    if (!engine || !record || !out_record_id)
        return AIRY_EINVAL;
    return AIRY_ENOSYS;
}

airy_err_t airy_memory_query(airy_memory_engine_t *engine,
                             const airy_memory_query_t *query,
                             airy_memory_result_ext_t **out_result)
{
    if (!engine || !query || !out_result)
        return AIRY_EINVAL;
    return AIRY_ENOSYS;
}

airy_err_t airy_memory_mount(airy_memory_engine_t *engine, const char *record_id,
                             const char *context)
{
    if (!engine || !record_id || !context)
        return AIRY_EINVAL;
    return AIRY_ENOSYS;
}

void airy_memory_result_free(airy_memory_result_ext_t *result)
{
    (void)result;
}
