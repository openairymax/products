/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file payload_registry.h
 * @brief TC/MC/Intent/Plan 认知策略载荷注册面——daemon 启动期注入 atoms 机制核的入口。
 *
 * 机制/策略分离（0.1.19 M5-4）：本头是 products/cognition 载荷库对
 * daemons 的唯一契约——返回机制核消费面（tc.h 17 项 / mc.h 10 项 /
 * intent.h 1 项 / airy_plan_ops.h 2 项 ops 表）的静态实例。think_d 在
 * svc_prepare 期调用 are_ops_set_tc()/are_ops_set_mc()/
 * are_ops_set_intent()/are_ops_set_plan() 注入，svc_destroy 期传 NULL
 * 清除。载荷缺席时机制核 fail-fast（AIRY_ENOSYS；plan 槽位为引擎裸
 * 启动 BAN-257），不产生隐式降级。
 */

#ifndef AIRY_COG_PAYLOAD_REGISTRY_H
#define AIRY_COG_PAYLOAD_REGISTRY_H

#include "airy_plan_ops.h"
#include "intent.h"
#include "mc.h"
#include "tc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 取 TC 策略载荷 ops 表（17 项，静态生命周期）。
 * @return 非 NULL 静态表；表内函数指针均指向本库实现
 */
const airy_tc_ops_t *cog_payload_tc(void);

/**
 * @brief 取 MC 策略载荷 ops 表（10 项，静态生命周期）。
 * @return 非 NULL 静态表；表内函数指针均指向本库实现
 */
const airy_mc_ops_t *cog_payload_mc(void);

/**
 * @brief 取意图解析策略载荷 ops 表（1 项，静态生命周期）。
 * @return 非 NULL 静态表；表内函数指针均指向本库实现
 */
const airy_intent_ops_t *cog_payload_intent(void);

/**
 * @brief 取规划策略载荷 ops 表（2 项：reactive/reflective 工厂，静态生命周期）。
 * @return 非 NULL 静态表；表内函数指针均指向本库实现
 */
const airy_plan_ops_t *cog_payload_plan(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_COG_PAYLOAD_REGISTRY_H */
