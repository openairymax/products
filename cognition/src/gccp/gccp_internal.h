// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gccp_internal.h
 * @brief GCCP 策略域内共享内部头（products/cognition，M5-4 迁出）。
 *
 * 声明 gccp.c（协议编排核心域）、gccp_llm_json.c（LLM 调用 + JSON 解析
 * 域）、gccp_heuristic.c（启发式降级域）之间跨文件共享的函数。仅 GCCP
 * 策略域内使用，不对外暴露；公共契约见 gccp_strategy.h / gccp.h。
 */

#ifndef AIRY_RT_GCCP_INTERNAL_H
#define AIRY_RT_GCCP_INTERNAL_H

#include "gccp_strategy.h"

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* LLM 调用域（gccp_llm_json.c）：经补全闭包调用 LLM 并返回响应文本
 * （OWNER，调用方 AIRY_FREE）；闭包缺失或调用失败返回 NULL。响应的
 * 释放在本域内经 ops 注入表完成，调用方无需关心。 */
char *gccp_llm_call(airy_gccp_complete_fn complete, void *complete_ctx, const char *model,
                    const char *system_prompt, const char *user_input);

#ifdef AIRY_HAS_CJSON
/* 提取 cJSON 对象字符串字段（复制，BORROW→OWNER） */
char *gccp_json_field(cJSON *obj, const char *key);
/* 将 LLM 目标 JSON 应用到 goal 结构（字符串字段 OWNER 语义，可覆盖） */
void gccp_apply_json(airy_gccp_goal_t *goal, cJSON *obj);
/* 定位 LLM 输出中的 JSON 子串（剥离 ```json 围栏，OWNER，调用方 AIRY_FREE） */
char *gccp_find_json(const char *text);
#endif /* AIRY_HAS_CJSON */

/* 启发式降级域（gccp_heuristic.c）：无 LLM / 解析失败时的确定性降级路径 */
airy_gccp_goal_t *gccp_prefill(const char *input, size_t input_len);
airy_gccp_probe_t *gccp_heur_probe(const char *input, size_t input_len);
int gccp_is_simple(const char *input, size_t input_len);
airy_gccp_goal_t *gccp_heur_goal(const char *input, size_t input_len);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GCCP_INTERNAL_H */
