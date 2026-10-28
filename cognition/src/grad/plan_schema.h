// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#ifndef AIRY_PRODUCTS_COGNITION_GRAD_PLAN_SCHEMA_H
#define AIRY_PRODUCTS_COGNITION_GRAD_PLAN_SCHEMA_H

/**
 * @file plan_schema.h
 * @brief GRAD 计划 schema v1 校验契约（0.1.20 C1）。
 *
 * 模型 A 产出的计划 JSON 在进入结构化解析前先过本 schema：JSON 层
 * （字段类型/取值域/长度界）+ 结构层（depends 存在性与无自环、outputs
 * 计划内唯一、inputs 的 producer 必须落在 depends 传递闭包内）。
 * 违例整份拒绝（EINVAL），不静默修复——0.1.19 审查确认宽松解析是
 * 空壳计划流入批判门的根因。
 *
 * 校验层次分工（机制/策略分离）：
 *   - C1（本头）：文档 well-formedness 与节点引用拓扑完整性；
 *   - C4（plan_metadata）：补全缺省元数据（outputs/inputs/cost，
 *     标 inferred），显式值绝不覆盖；
 *   - C5（grad_verifier）：语义判定（E-01 因果 / E-02 死锁 / E-03
 *     资源 / E-04 护栏 / E-05 充分性）。
 * 因此 outputs/inputs/cost 为可缺省字段（C4 补全），给出则必须合规。
 *
 * 本头是 schema 的编译期权威源；同目录 plan.schema.json 为发布视图
 * （供生态侧工具查阅），字段以本头为准。GRAD_PLAN_SCHEMA_VERSION
 * 即批次回滚点标识。
 */

#include "airy_rt.h"

#include <stddef.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* schema 版本（批次回滚点标识） */
#define GRAD_PLAN_SCHEMA_VERSION "1"

/* 结构界 */
#define GRAD_MAX_NODES 32
#define GRAD_PLAN_ID_MAX 64
/* plan_id 缺省值：调用方未提供计划标识时的统一归一名（schema 域内合法）。
 * 解析、校验与落盘必须共用该常量，避免"校验用 NULL / 构造用默认值"
 * 的语义分叉。 */
#define GRAD_PLAN_ID_DEFAULT "grad_plan"
#define GRAD_NODE_ID_MAX 48
#define GRAD_GOAL_MAX 512
#define GRAD_ARTIFACT_MAX 64
#define GRAD_INPUT_MAX 160
#define GRAD_DEPENDS_MAX GRAD_MAX_NODES
#define GRAD_INPUTS_MAX 16
#define GRAD_OUTPUTS_MAX 8

/* 取值域 */
#define GRAD_COST_MAX 1000000.0
#define GRAD_TIMEOUT_MAX_MS 600000
#define GRAD_TIMEOUT_DEFAULT_MS 30000
#define GRAD_PRIORITY_DEFAULT 150
#define GRAD_INFERRED_MAX_RATIO 0.5

/* 节点角色合法域（schema v1；序数供 C4 基线表索引） */
typedef enum grad_plan_role {
    GRAD_PLAN_ROLE_PLAN = 0,
    GRAD_PLAN_ROLE_RETRIEVE,
    GRAD_PLAN_ROLE_REASON,
    GRAD_PLAN_ROLE_ACT,
    GRAD_PLAN_ROLE_VERIFY,
    GRAD_PLAN_ROLE_COUNT
} grad_plan_role_e;

/* 计划来源（系统字段，缺省视为 generated） */
typedef enum grad_plan_origin {
    GRAD_PLAN_ORIGIN_GENERATED = 0,
    GRAD_PLAN_ORIGIN_SEED,
    GRAD_PLAN_ORIGIN_FALLBACK
} grad_plan_origin_e;

/* 首个 schema 违例定位（WARN 日志与测试断言用） */
typedef struct grad_schema_err {
    int index;       /* 违例节点下标；文档级违例为 -1 */
    char field[24];  /* 违例字段名 */
    char reason[32]; /* missing/pattern/range/dup/enum/format/... */
} grad_schema_err_t;

#ifdef AIRY_HAS_CJSON

/**
 * @brief 校验计划 JSON 文档是否满足 schema v1。
 *
 * @param root [in] 已通过 cJSON_Parse 的计划文档（non-NULL, BORROW）
 * @param plan_id [in] 计划标识（调用方持有，模式 [A-Za-z0-9_.-]{1,64}）
 * @param err [out] 违例定位（可为 NULL；不关心详情时置 NULL）
 * @return AIRY_SUCCESS 合规；AIRY_EINVAL 违例（详情回填 *err）
 *
 * @ownership err: NONE
 * @threadsafe yes（纯函数）
 */
airy_err_t grad_plan_schema_validate(const cJSON *root, const char *plan_id,
                                     grad_schema_err_t *err);

#endif /* AIRY_HAS_CJSON */

/**
 * @brief 角色字符串到枚举序（非法角色返回 GRAD_PLAN_ROLE_COUNT）。
 *
 * @ownership return: NONE
 */
grad_plan_role_e grad_role_of(const char *role);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_PRODUCTS_COGNITION_GRAD_PLAN_SCHEMA_H */
