/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file lang_gateway.h
 * @brief 推理语言全生命周期管理网关（Inference Language Gateway）。
 *
 * 用户输入的自然语言不直接进入模型，先经网关标准化为内部标准数据格式
 * （Canonical Data Format），再路由给具体模型：
 *
 *   Phase 1（决策前）：信号提取（语言/任务/上下文/代码含量）+ 多因子路由决策
 *                      （硬规则 → 模型原生能力 → 输入语言对齐 → 性价比阈值）
 *   Phase 2（推理中）：System Prompt 语言约束注入 + 输入转换（占位符保护/历史重写）
 *   Phase 3（输出后）：语言漂移检测 + 术语一致性 + 意译润色
 *
 * 模型画像（ModelProfile）由 Tokenizer 特征校准模块产出：校准经后台
 * worker 异步执行（首轮不阻塞请求路径），每 N 次指令（默认 100）自动
 * 重校准 lang_ratio（中文 token / 英文 token）。校准经 llm_d 的
 * count_tokens RPC 完成，llm_d 不可用时网关降级为启发式决策（不影响
 * 主流程）。
 *
 * 模块化设计：各阶段为独立源文件（signal_extractor / lang_router /
 * system_prompt / output_post_processor / calibrator），策略参数可经
 * 配置调整，后续策略升级不改变公共 API。
 *
 * M5-2 机制/策略切分：本库为生态层策略载荷（products/lang_gateway），
 * 数据类型契约与 ops 注入表归机制核（airy_lang_gw_ops.h），本头仅
 * 声明库直接 API；机制核消费者经 are_ops_get_lang_gw() 分发，不直接
 * 依赖本头。
 */

#ifndef AIRY_PRODUCTS_LANG_GATEWAY_H
#define AIRY_PRODUCTS_LANG_GATEWAY_H

#include "airy_lang_gw_ops.h"
#include "airy_rt.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 七类型（airy_lang_t / airy_lang_task_t / airy_model_profile_t /
 * airy_lang_signals_t / airy_lang_decision_t / airy_canonical_request_t /
 * airy_lang_gateway_config_t）与 opaque 句柄由契约头 airy_lang_gw_ops.h
 * 单点承接（M5-2 ABI 契约入核），本头不重复定义。 */

/**
 * @brief 创建推理语言网关。
 * @param cfg 配置（NULL 用默认值）（BORROW）
 * @param out 输出句柄（OWNER，airy_lang_gateway_destroy 释放）
 * @return 0 成功；非零错误码
 */
AIRY_API airy_err_t airy_lang_gateway_create(const airy_lang_gateway_config_t *cfg,
                                             airy_lang_gateway_t **out);

/**
 * @brief 销毁网关。
 * @param gw 句柄（TRANSFER）
 */
AIRY_API void airy_lang_gateway_destroy(airy_lang_gateway_t *gw);

/**
 * @brief 网关主入口：Phase 1+2 —— 标准化用户输入并产出 Canonical Request。
 *
 * 执行：信号提取 → 路由决策（需模型画像；缺失时启发式降级）→ System Prompt
 * 注入 → 输入转换（占位符保护/历史重写）。本函数为纯本地计算，不产生 LLM
 * 调用；语言约束 System Prompt 供上层注入实际模型请求的首条 system 消息。
 *
 * @param gw 网关
 * @param raw_input 用户原始自然语言输入（BORROW）
 * @param model_id 目标模型 ID（BORROW；NULL/空 = 默认模型）
 * @param history_tokens 对话历史估算 token 数（0 = 未知）
 * @param out 输出的 Canonical Request（OWNER，airy_lang_gateway_free_canonical 释放）
 * @return 0 成功；非零错误码
 */
AIRY_API airy_err_t airy_lang_gateway_process(airy_lang_gateway_t *gw, const char *raw_input,
                                              const char *model_id, uint32_t history_tokens,
                                              airy_canonical_request_t **out);

/**
 * @brief 释放 Canonical Request。
 * @param req 请求（TRANSFER）
 */
AIRY_API void airy_lang_gateway_free_canonical(airy_canonical_request_t *req);

/**
 * @brief Phase 3：输出后处理（语言漂移检测 + 术语一致性 + 意译润色）。
 *
 * @param gw 网关
 * @param text 模型原始输出（BORROW）
 * @param expected_lang 期望输出语言（来自路由决策 output_lang）
 * @param out 处理后的输出（OWNER，AIRY_FREE 释放）
 * @return 0 成功；非零错误码。语言漂移不视为错误：漂移文本经术语表/润色
 *         归一化后原样返回，上层可据返回值决定是否重试。
 */
AIRY_API airy_err_t airy_lang_gateway_post_process(airy_lang_gateway_t *gw, const char *text,
                                                   airy_lang_t expected_lang, char **out);

/**
 * @brief 查询模型画像。
 * @param gw 网关
 * @param model_id 模型 ID（BORROW）
 * @param out 输出画像（BORROW）
 * @return 0 找到；AIRY_ERR_NOT_FOUND 未校准/未知模型；AIRY_EINVAL 参数错误
 */
AIRY_API airy_err_t airy_lang_gateway_get_profile(airy_lang_gateway_t *gw, const char *model_id,
                                                  airy_model_profile_t *out);

/**
 * @brief 写入（或更新）模型画像并持久化。
 * @param gw 网关
 * @param profile 画像（BORROW）
 * @return 0 成功；非零错误码
 */
AIRY_API airy_err_t airy_lang_gateway_set_profile(airy_lang_gateway_t *gw,
                                                  const airy_model_profile_t *profile);

/**
 * @brief 对单个模型执行 Tokenizer 特征校准。
 *
 * 经 llm_d count_tokens RPC 对中英文对齐样本计数，计算
 * lang_ratio = zh_tokens / en_tokens，并推断原生语言，写入画像并持久化。
 * llm_d 不可用时返回错误（上层仅降级，不阻断）。
 *
 * @param gw 网关
 * @param model_id 模型 ID（BORROW）
 * @return 0 成功；非零错误码（llm_d 不可用/模型未配置）
 */
AIRY_API airy_err_t airy_lang_gateway_calibrate(airy_lang_gateway_t *gw, const char *model_id);

/**
 * @brief 对所有已配置模型执行 Tokenizer 特征校准。
 *
 * 模型列表经 llm_d list_models RPC 获取。个别模型失败不阻断其余模型。
 *
 * @param gw 网关
 * @param out_count 成功校准的模型数（可为 NULL）
 * @return 0 至少一个模型校准成功；非零全部失败（llm_d 不可用）
 */
AIRY_API airy_err_t airy_lang_gateway_calibrate_all(airy_lang_gateway_t *gw, uint32_t *out_count);

/**
 * @brief 指令计数器步进；达到重校准阈值时触发全量重校准并清零。
 *
 * 由上层在每次用户指令进入网关后调用。返回 1 表示本轮已触发重校准
 * （调用方可在决策链中呈现），0 表示未触发。
 *
 * @param gw 网关
 * @return 1 = 触发重校准；0 = 未触发
 */
AIRY_API int airy_lang_gateway_tick(airy_lang_gateway_t *gw);

/**
 * @brief 网关统计（可观测性）。
 *
 * 输出含网关计数与逐模型校准画像：profiles[] 每项携带 lang_ratio /
 * lang_confidence / native_lang / calibrated_at（Unix 秒，0 = 尚未校准），
 * 据此可判定校准是否生效及生效时间；画像数量与持久化内容一致，不截断。
 *
 * @param gw 网关
 * @param out_json 输出 JSON（OWNER，AIRY_FREE 释放）
 * @return 0 成功；非零错误码
 */
AIRY_API airy_err_t airy_lang_gateway_stats(airy_lang_gateway_t *gw, char **out_json);

/* ======================== 独立策略组件（可单独使用/测试） ======================== */

/**
 * @brief 语言检测（纯本地启发式：代码特征 → 非 ASCII 比例 → 中文字符占比）。
 * @param text 输入文本（BORROW）
 * @param out_lang 输出语言（可为 NULL）
 * @param out_conf 输出置信度 0.0~1.0（可为 NULL）
 * @return 0 成功；AIRY_EINVAL 空输入
 */
AIRY_API airy_err_t airy_lang_detect(const char *text, airy_lang_t *out_lang, double *out_conf);

/**
 * @brief 任务分类（关键词表：代码/数学/文化/政策/QA）。
 * @param text 输入文本（BORROW）
 * @param out_task 输出任务类型（可为 NULL）
 * @return 0 成功；AIRY_EINVAL 空输入
 */
AIRY_API airy_err_t airy_lang_classify_task(const char *text, airy_lang_task_t *out_task);

/**
 * @brief 多因子路由决策（硬规则 → 模型能力 → 输入对齐 → 性价比）。
 * @param signals 信号（BORROW）
 * @param profile 模型画像（BORROW；NULL = 仅硬规则+输入对齐，跳过画像因子）
 * @param out 输出决策（OWNER，调用方 free reason/chain）
 * @return 0 成功；AIRY_EINVAL 参数错误
 */
AIRY_API airy_err_t airy_lang_route(const airy_lang_signals_t *signals,
                                    const airy_model_profile_t *profile,
                                    airy_lang_decision_t *out);

/**
 * @brief 生成语言约束 System Prompt（Phase 2 注入物）。
 *
 * 约束模型内部推理语言与最终输出语言，抑制语言漂移；空串（无约束）在
 * reasoning==output 且均非 UNKNOWN 时也会生成简版约束。
 *
 * @param reasoning 推理语言
 * @param output 输出语言
 * @param out 输出 prompt（OWNER，AIRY_FREE 释放）
 * @return 0 成功；AIRY_EINVAL 参数错误
 */
AIRY_API airy_err_t airy_lang_build_system_prompt(airy_lang_t reasoning, airy_lang_t output,
                                                  char **out);

/**
 * @brief 文本 token 数估算（中文 1.8 token/字 + 英文 1.2 token/词，启发式）。
 * @param text 文本（BORROW）
 * @param out_tokens 输出估算值
 * @return 0 成功；AIRY_EINVAL 空输入
 */
AIRY_API airy_err_t airy_lang_estimate_tokens(const char *text, uint32_t *out_tokens);

/**
 * @brief 语言显示名（"中文"/"英文"/"未知"）。
 * @param lang 语言
 * @return 静态字符串（非线程本地，仅调试/渲染用）
 */
AIRY_API const char *airy_lang_name(airy_lang_t lang);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_CORELOOPTHREE_LANG_GATEWAY_H */
