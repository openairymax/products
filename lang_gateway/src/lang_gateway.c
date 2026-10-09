/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file lang_gateway.c
 * @brief 推理语言网关主控制器：三阶段编排 + 生命周期 + 统计。
 *
 *   Phase 1（决策前）：signal_extractor 信号提取 → lang_router 多因子路由
 *   Phase 2（推理中）：system_prompt 注入语言约束 System Prompt
 *   Phase 3（输出后）：output_post_processor 漂移检测/术语/润色（post_process）
 *
 * 用户输入经 process() 标准化为 Canonical Data Format 后再送入模型通道；
 * 决策链与三阶段遥测随 canonical.telemetry_json 保留（可观测性/审计）。
 */

#include "lang_gateway.h"
#include "canonical.h"
#include "airy_memory.h"
#include "platform.h"
#include <logging.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void gateway_defaults(struct airy_lang_gateway *gw)
{
    const char *data_dir = airy_data_dir();
    if (data_dir && data_dir[0]) {
        snprintf(gw->profiles_path, sizeof(gw->profiles_path), "%s/%s",
                 data_dir, AIRY_LANG_PROFILES_DEFAULT_PATH);
    } else {
        snprintf(gw->profiles_path, sizeof(gw->profiles_path), "%s",
                 AIRY_LANG_PROFILES_DEFAULT_PATH);
    }

    const char *sock = airy_runtime_dir_socket("llm.sock");
    snprintf(gw->llm_socket, sizeof(gw->llm_socket), "%s", sock ? sock : "");

    gw->ratio_high = AIRY_LANG_RATIO_HIGH_DEFAULT;
    gw->ratio_low = AIRY_LANG_RATIO_LOW_DEFAULT;
    gw->recalibrate_interval = 100;
    gw->profile_count = 0;
    gw->instruction_count = 0;
    gw->process_count = 0;
    gw->calibrate_count = 0;
    gw->drift_detected = 0;
}

airy_err_t airy_lang_gateway_create(const airy_lang_gateway_config_t *cfg,
                                    airy_lang_gateway_t **out)
{
    if (!out)
        return AIRY_EINVAL;

    struct airy_lang_gateway *gw = AIRY_CALLOC(1, sizeof(*gw));
    if (!gw)
        return AIRY_ENOMEM;

    if (airy_mtx_init(&gw->prof_lock) != 0 ||
        airy_cond_init(&gw->cal_wake) != 0) {
        airy_cond_destroy(&gw->cal_wake);
        airy_mtx_destroy(&gw->prof_lock);
        AIRY_FREE(gw);
        return AIRY_ENOMEM;
    }

    gateway_defaults(gw);

    if (cfg) {
        if (cfg->profiles_path && cfg->profiles_path[0])
            snprintf(gw->profiles_path, sizeof(gw->profiles_path), "%s",
                     cfg->profiles_path);
        if (cfg->llm_socket && cfg->llm_socket[0])
            snprintf(gw->llm_socket, sizeof(gw->llm_socket), "%s",
                     cfg->llm_socket);
        if (cfg->ratio_high > 0.0)
            gw->ratio_high = cfg->ratio_high;
        if (cfg->ratio_low > 0.0)
            gw->ratio_low = cfg->ratio_low;
        if (cfg->recalibrate_interval > 0)
            gw->recalibrate_interval = cfg->recalibrate_interval;
    }

    /* 加载已校准画像（持久化），避免重复校准；坏数据整批拒绝（fail-fast），
     * 网关以空画像启动等待校准重建（不阻断主流程，但显式告警不静默）。 */
    int lerr = profiles_load(gw);
    if (lerr == AIRY_EOVERFLOW)
        AIRY_LOG_WARN("lang_gateway: profiles overflow (max=%d), "
                      "persisted profiles rejected",
                      AIRY_LANG_PROFILE_MAX);
    else if (lerr != AIRY_EOK)
        AIRY_LOG_WARN("lang_gateway: invalid profiles file, starting empty");

    /* 启动时投递一轮回合到后台 worker 异步校准（llm_d 不可用时静默
     * 降级）；首轮不阻塞 create（B6-2）。 */
    int auto_cal = cfg ? cfg->auto_calibrate_on_create : 1;
    if (auto_cal)
        cal_worker_post(gw);

    *out = gw;
    return AIRY_EOK;
}

void airy_lang_gateway_destroy(airy_lang_gateway_t *gw)
{
    if (!gw)
        return;
    cal_worker_stop(gw);
    airy_cond_destroy(&gw->cal_wake);
    airy_mtx_destroy(&gw->prof_lock);
    AIRY_FREE(gw);
}

void airy_lang_gateway_free_canonical(airy_canonical_request_t *req)
{
    if (!req)
        return;
    AIRY_FREE(req->request_id);
    AIRY_FREE(req->model_id);
    AIRY_FREE(req->raw_input);
    AIRY_FREE(req->routing.decision_reason);
    AIRY_FREE(req->routing.decision_chain);
    AIRY_FREE(req->system_prompt);
    AIRY_FREE(req->transformed_input);
    AIRY_FREE(req->final_output);
    AIRY_FREE(req->telemetry_json);
    AIRY_FREE(req);
}

airy_err_t airy_lang_gateway_process(airy_lang_gateway_t *gw, const char *raw_input,
                                     const char *model_id, uint32_t history_tokens,
                                     airy_canonical_request_t **out)
{
    if (!gw || !raw_input || !out)
        return AIRY_EINVAL;

    uint64_t t0 = (uint64_t)time(NULL);

    /* ---- Phase 1: 信号提取（纯本地，零 LLM） ---- */
    airy_lang_signals_t signals;
    AIRY_MEMSET(&signals, 0, sizeof(signals));
    (void)airy_lang_detect(raw_input, &signals.detected_lang, &signals.lang_confidence);
    (void)airy_lang_classify_task(raw_input, &signals.task_type);
    signals.history_tokens = history_tokens;
    signals.contains_code = (signals.task_type == AIRY_LANG_TASK_CODE);
    signals.is_critical = (signals.task_type == AIRY_LANG_TASK_CULTURE ||
                           signals.task_type == AIRY_LANG_TASK_POLICY);

    /* ---- Phase 1: 路由决策（模型画像缺失时启发式降级） ---- */
    airy_model_profile_t profile;
    const airy_model_profile_t *pp = NULL;
    if (model_id && model_id[0] &&
        airy_lang_gateway_get_profile(gw, model_id, &profile) == AIRY_EOK)
        pp = &profile;

    airy_lang_decision_t routing;
    AIRY_MEMSET(&routing, 0, sizeof(routing));
    airy_err_t rerr = airy_lang_route(&signals, pp, &routing);
    if (rerr != AIRY_EOK) {
        /* 路由失败（参数错误）：降级为中文推理/中文输出 */
        routing.reasoning_lang = AIRY_LANG_ZH;
        routing.output_lang = AIRY_LANG_ZH;
        routing.decision_reason = AIRY_STRDUP("route_failed_fallback");
        routing.decision_chain = AIRY_STRDUP("[\"route failed → zh fallback\"]");
    }

    /* ---- Phase 2: System Prompt 注入（输入直通，无占位假实现） ---- */
    char *sys_prompt = NULL;
    (void)airy_lang_build_system_prompt(routing.reasoning_lang, routing.output_lang,
                                        &sys_prompt);

    /* ---- 组装 Canonical Request ---- */
    airy_canonical_request_t *req = AIRY_CALLOC(1, sizeof(*req));
    if (!req) {
        AIRY_FREE(sys_prompt);
        AIRY_FREE(routing.decision_reason);
        AIRY_FREE(routing.decision_chain);
        return AIRY_ENOMEM;
    }

    char rid[64];
    snprintf(rid, sizeof(rid), "lgw_%llu", (unsigned long long)t0);
    req->request_id = AIRY_STRDUP(rid);
    req->model_id = AIRY_STRDUP(model_id && model_id[0] ? model_id : "default");
    req->raw_input = AIRY_STRDUP(raw_input);
    req->signals = signals;
    req->routing = routing;
    req->system_prompt = sys_prompt;
    /* RPC 契约字段：翻译扩展点未接入，恒为输入直通副本（B6-4 名实一致） */
    req->transformed_input = AIRY_STRDUP(raw_input);

    /* ---- 遥测（决策链/语言/统计） ---- */
    const char *dl = signals.detected_lang == AIRY_LANG_ZH ? "zh"
                    : signals.detected_lang == AIRY_LANG_EN ? "en" : "unknown";
    const char *rl = routing.reasoning_lang == AIRY_LANG_ZH ? "zh" : "en";
    const char *ol = routing.output_lang == AIRY_LANG_ZH ? "zh" : "en";
    char tele[512];
    snprintf(tele, sizeof(tele),
             "{\"detected_lang\":\"%s\",\"conf\":%.2f,\"task\":\"%s\","
             "\"reasoning_lang\":\"%s\",\"output_lang\":\"%s\","
             "\"reason\":\"%s\",\"chain\":%s}",
             dl, signals.lang_confidence,
             signals.task_type == AIRY_LANG_TASK_CULTURE ? "culture" :
             signals.task_type == AIRY_LANG_TASK_POLICY ? "policy" :
             signals.task_type == AIRY_LANG_TASK_CODE ? "code" :
             signals.task_type == AIRY_LANG_TASK_MATH ? "math" : "general",
             rl, ol,
             routing.decision_reason ? routing.decision_reason : "",
             routing.decision_chain ? routing.decision_chain : "[]");
    req->telemetry_json = AIRY_STRDUP(tele);

    airy_mtx_lock(&gw->prof_lock);
    gw->process_count++;
    airy_mtx_unlock(&gw->prof_lock);
    *out = req;
    return AIRY_EOK;
}

int airy_lang_gateway_tick(airy_lang_gateway_t *gw)
{
    if (!gw)
        return 0;

    airy_mtx_lock(&gw->prof_lock);
    gw->instruction_count++;
    int due = gw->recalibrate_interval > 0 &&
              gw->instruction_count >= gw->recalibrate_interval;
    if (due)
        gw->instruction_count = 0;
    airy_mtx_unlock(&gw->prof_lock);

    if (due) {
        /* 到期即投递后台回合并返回 1（决策点同步语义，不依赖后台成败） */
        cal_worker_post(gw);
        return 1;
    }
    return 0;
}

#define LANG_STATS_HEAD_FMT                                     \
    "{\"process_count\":%llu,\"calibrate_count\":%llu,"         \
    "\"drift_detected\":%llu,\"profile_count\":%zu,"            \
    "\"instruction_count\":%u,\"recalibrate_interval\":%u,\"profiles\":["

/* 单条画像 JSON；buf 为 NULL 时仅返回所需长度（两遍法第一遍）。
 * ratio_high/ratio_low 为路由阈值（V6.3 同源画像，可观测验证）。 */
static int profile_to_json(char *buf, size_t cap, const airy_model_profile_t *p)
{
    return snprintf(buf, cap,
                    "{\"model_id\":\"%s\",\"provider\":\"%s\",\"family\":\"%s\","
                    "\"lang_ratio\":%.3f,\"lang_confidence\":%.3f,"
                    "\"ratio_high\":%.2f,\"ratio_low\":%.2f,"
                    "\"native_lang\":\"%s\",\"calibrated_at\":%llu}",
                    p->model_id, p->provider, p->family, p->lang_ratio,
                    p->lang_confidence, p->ratio_high, p->ratio_low,
                    p->native_lang == AIRY_LANG_ZH ? "zh" : "en",
                    (unsigned long long)p->calibrated_at);
}

airy_err_t airy_lang_gateway_stats(airy_lang_gateway_t *gw, char **out_json)
{
    if (!gw || !out_json)
        return AIRY_EINVAL;
    *out_json = NULL;

    /* 两遍法在单锁内完成：need 计算与写入之间数据不得变化，否则溢出 */
    airy_mtx_lock(&gw->prof_lock);

    size_t need = (size_t)snprintf(NULL, 0, LANG_STATS_HEAD_FMT,
                                   (unsigned long long)gw->process_count,
                                   (unsigned long long)gw->calibrate_count,
                                   (unsigned long long)gw->drift_detected,
                                   gw->profile_count, gw->instruction_count,
                                   gw->recalibrate_interval) + 3 /* ]} + NUL */;

    for (size_t i = 0; i < gw->profile_count; i++) {
        int n = profile_to_json(NULL, 0, &gw->profiles[i]);
        if (n < 0) {
            airy_mtx_unlock(&gw->prof_lock);
            return AIRY_EINVAL;
        }
        need += (size_t)n + 1 /* 分隔逗号 */;
    }

    char *buf = AIRY_MALLOC(need);
    if (!buf) {
        airy_mtx_unlock(&gw->prof_lock);
        return AIRY_ENOMEM;
    }

    size_t off = (size_t)snprintf(buf, need, LANG_STATS_HEAD_FMT,
                                  (unsigned long long)gw->process_count,
                                  (unsigned long long)gw->calibrate_count,
                                  (unsigned long long)gw->drift_detected,
                                  gw->profile_count, gw->instruction_count,
                                  gw->recalibrate_interval);
    for (size_t i = 0; i < gw->profile_count; i++) {
        if (i > 0)
            buf[off++] = ',';
        off += (size_t)profile_to_json(buf + off, need - off, &gw->profiles[i]);
    }
    AIRY_MEMCPY(buf + off, "]}", 3); /* 含终止符：校准状态必须完整可查，不得截断 */

    airy_mtx_unlock(&gw->prof_lock);

    *out_json = buf;
    return AIRY_EOK;
}
