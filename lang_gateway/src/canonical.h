/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file canonical.h
 * @brief 推理语言网关内部数据结构与共享工具（非公共 API，不安装）。
 */

#ifndef AIRY_RT_CORELOOPTHREE_LANG_GATEWAY_CANONICAL_H
#define AIRY_RT_CORELOOPTHREE_LANG_GATEWAY_CANONICAL_H

#include "airy_rt.h"
#include "lang_gateway.h"
#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 校准样本：中英文含义对齐（避开专有名词），取双样本平均提高稳定性。 */
#define AIRY_LANG_ZH_SAMPLE1 "深度学习的核心是神经网络"
#define AIRY_LANG_EN_SAMPLE1 "The core of deep learning is neural networks"
#define AIRY_LANG_ZH_SAMPLE2 "人工智能正在改变世界"
#define AIRY_LANG_EN_SAMPLE2 "Artificial intelligence is changing the world"

#define AIRY_LANG_PROFILES_DEFAULT_PATH "agentrt/lang/profiles.json"

/* 模型画像容量单一权威（SSoT）：全部消费方必须引用本宏，禁止本地副本
 * 或字面量（B6-1）。越界一律显式拒绝（fail-fast），不得静默截断。 */
#define AIRY_LANG_PROFILE_MAX 32

/* 路由阈值默认值（SSoT，B6-3）：画像未携带阈值（ratio_high/ratio_low <= 0）
 * 时 lang_router 的回退值，亦是新画像产出时的种子默认。全部消费方必须
 * 引用本宏，路由禁止硬编码阈值字面量。 */
#define AIRY_LANG_RATIO_HIGH_DEFAULT 1.35
#define AIRY_LANG_RATIO_LOW_DEFAULT 0.85

/** 网关实例（内部权威定义）。prof_lock 单锁保护全部可变状态；
 * profiles_path/llm_socket/阈值在 create 后不可变，免锁。ratio_high/
 * ratio_low 为画像缺失阈值时的种子值（B6-3）：运行时路由只读画像内
 * 阈值，改画像即改阈值（V6.3 同源同值）。 */
struct airy_lang_gateway {
    char profiles_path[512];
    char llm_socket[256];
    double ratio_high;
    double ratio_low;
    uint32_t recalibrate_interval;

    airy_mtx_t prof_lock;

    /* 模型画像（model_id → profile） */
    airy_model_profile_t profiles[AIRY_LANG_PROFILE_MAX];
    size_t profile_count;

    /* 指令计数（周期性重校准触发） */
    uint32_t instruction_count;

    /* 校准后台 worker（B6-2）：pending 投递合并为单回合，stop 停机；
     * 线程懒启动（首次投递时创建），端侧零校准负载时不占线程。 */
    airy_cond_t cal_wake;
    int cal_pending;
    int cal_stop;
    airy_thread_t cal_thread;
    int cal_thread_valid;

    /* 可观测性 */
    uint64_t process_count;
    uint64_t calibrate_count;
    uint64_t drift_detected;
};

_Static_assert(sizeof(((struct airy_lang_gateway *)0)->profiles) ==
                   AIRY_LANG_PROFILE_MAX * sizeof(airy_model_profile_t),
               "profiles capacity must match AIRY_LANG_PROFILE_MAX");

/* 内部共享工具 */

/** 规范化文本的 UTF-8 有效长度（含终止符），非法字节替换为 U+FFFD。 */
size_t airy_lang_utf8_sanitize(const char *in, char *out, size_t out_sz);

/** 统计中文字符（U+4E00~U+9FFF）个数。 */
uint32_t airy_lang_chinese_count(const char *text);

/** 统计 ASCII 字符个数。 */
uint32_t airy_lang_ascii_count(const char *text);

/* 画像持久化（calibrator.c 实现，lang_gateway.c create 时加载）。
 * load 返回 AIRY_EOK / AIRY_EINVAL（坏 JSON）/ AIRY_EOVERFLOW（越界整批拒绝）。 */
int profiles_load(struct airy_lang_gateway *gw);
int profiles_save(struct airy_lang_gateway *gw);

/* 校准后台 worker（calibrator.c 实现，B6-2）：post 投递校准回合（懒启动
 * 线程）；stop 置停机并 join。回合上界 = list_models + 每模型 ≤4 次
 * count_tokens RPC（单次 AIRY_LANG_RPC_TIMEOUT_MS），模型间响应停机。 */
void cal_worker_post(struct airy_lang_gateway *gw);
void cal_worker_stop(struct airy_lang_gateway *gw);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_CORELOOPTHREE_LANG_GATEWAY_CANONICAL_H */
