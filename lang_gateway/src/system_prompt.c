/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file system_prompt.c
 * @brief 推理语言网关 Phase 2：语言约束 System Prompt 注入。
 *
 * System Prompt 注入是"推理语言策略"的强制载体：向模型请求首条 system
 * 消息注入 <thinking>/<answer> 双标签契约与语言约束（内部推理语言、最终
 * 输出语言、违反丢弃惩罚），从源头抑制语言漂移。输入不做转换（RPC 契约
 * 字段 transformed_input 恒为输入直通副本）；输入翻译为未来可插拔扩展点，
 * 接入时以独立模块实现，不在此处保留占位实现。
 */

#include "lang_gateway.h"
#include "canonical.h"
#include "airy_memory.h"

#include <stdio.h>
#include <string.h>

/* 双标签契约（<thinking>/<answer>）在 Phase 3 由 output_post_processor
 * 解析，语言约束与标签契约必须成对注入/成对消费。 */
airy_err_t airy_lang_build_system_prompt(airy_lang_t reasoning, airy_lang_t output,
                                         char **out)
{
    if (!out)
        return AIRY_EINVAL;

    const char *r_name = (reasoning == AIRY_LANG_ZH) ? "中文" : "英文";
    const char *o_name = (output == AIRY_LANG_ZH) ? "中文" : "英文";

    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "语言策略（最高优先级）：\n"
        "1. 内部推理：你的思考链、分析和所有内部思考必须严格使用%s。\n"
        "2. 最终回答：你的最终回答必须使用%s。\n"
        "3. 如果推理过程中出现任何违反语言策略的字符，回答将被丢弃。\n"
        "4. 使用<thinking>标签进行推理，使用<answer>标签输出最终回答。\n"
        "5. 用户只会看到<answer>部分的内容，确保最终输出自然流畅。",
        r_name, o_name);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return AIRY_EOVERFLOW;

    *out = AIRY_STRDUP(buf);
    if (!*out)
        return AIRY_ENOMEM;
    return AIRY_EOK;
}
