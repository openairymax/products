/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file calibrator.c
 * @brief 推理语言网关：模型 Tokenizer 特征校准（ModelProfile 产出方）。
 *
 * 校准方式：经 llm_d 的 count_tokens RPC 对中英文对齐样本计数，计算
 * lang_ratio = zh_tokens / en_tokens（>1 表示该模型中文更费 token）。
 * 校准触发（B6-2 起全部走后台 worker，不阻塞请求路径）：
 *   - 系统启动时投递一轮回合（auto_calibrate_on_create）；
 *   - 每 recalibrate_interval（默认 100）次指令投递一轮回合（tick）。
 * 回合上界 = list_models + 每模型 ≤4 次 count_tokens RPC；模型间响应停机。
 *
 * 画像持久化于 $AIRY_DATA_DIR/agentrt/lang/profiles.json，重启后直接
 * 加载，避免重复校准。llm_d 不可用时校准返回错误，网关降级为启发式
 * 决策（不阻断主流程）。
 */

#include "lang_gateway.h"
#include "canonical.h"
#include "airy_memory.h"
#include "platform.h"
#include "airy_ipc_ops.h"
#include "error.h"

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AIRY_LANG_RPC_TIMEOUT_MS 5000

/* worker 空闲等待超时（仅防 signal 丢失的保险；正常路径由 signal/broadcast
 * 立即唤醒）。停机经 broadcast 通知，不依赖本超时。 */
#define AIRY_LANG_WORKER_IDLE_MS 60000

/* 原生语言推断：Qwen 系中文；DeepSeek-V3 中文；其余默认英文（多数模型
 * 英文训练数据占比更高）。family/provider 取自 llm_d list_models。 */
static airy_lang_t infer_native_lang(const char *family, const char *provider,
                                     double ratio)
{
    if (family && (strstr(family, "Qwen") || strstr(family, "QWEN")))
        return AIRY_LANG_ZH;
    if (family && strstr(family, "DeepSeek") && strstr(family, "V3"))
        return AIRY_LANG_ZH;
    if (provider && strstr(provider, "deepseek"))
        return ratio < 1.3 ? AIRY_LANG_ZH : AIRY_LANG_EN;
    if (provider && strstr(provider, "qwen"))
        return AIRY_LANG_ZH;
    return AIRY_LANG_EN;
}

static void family_from_model(const char *model, const char *provider,
                              char *out, size_t out_sz)
{
    if (model && (strstr(model, "gpt") || strstr(model, "GPT"))) {
        snprintf(out, out_sz, "GPT");
    } else if (model && (strstr(model, "claude") || strstr(model, "Claude"))) {
        snprintf(out, out_sz, "Claude");
    } else if (model && (strstr(model, "deepseek") || strstr(model, "DeepSeek"))) {
        snprintf(out, out_sz, "DeepSeek");
    } else if (model && (strstr(model, "qwen") || strstr(model, "Qwen"))) {
        snprintf(out, out_sz, "Qwen");
    } else if (model && (strstr(model, "llama") || strstr(model, "Llama"))) {
        snprintf(out, out_sz, "Llama");
    } else if (provider && provider[0]) {
        snprintf(out, out_sz, "%s", provider);
    } else {
        snprintf(out, out_sz, "Other");
    }
}

/* 分词族映射（产品侧策略）：机制核只识别 generic|bpe|spm|uniform，
 * 模型名到算法族的归属属于策略，随产品发布。 */
static const char *token_family_of(const char *model)
{
    if (!model)
        return "generic";
    if (strstr(model, "gpt-4") || strstr(model, "gpt-4o") ||
        strstr(model, "gpt-35") || strstr(model, "gpt-3.5"))
        return "bpe";
    if (strstr(model, "claude") || strstr(model, "Claude"))
        return "uniform";
    if (strstr(model, "llama") || strstr(model, "Llama") ||
        strstr(model, "vicuna") || strstr(model, "alpaca"))
        return "spm";
    return "generic";
}

double cal_ratio_pooled(double sum_zh, double sum_en)
{
    if (!(sum_en > 0.0))
        return 0.0;
    return sum_zh / sum_en;
}

double cal_confidence(uint32_t samples, double dev)
{
    if (samples < AIRY_LANG_MIN_SAMPLES)
        return 0.0;
    if (dev < 0.0)
        dev = 0.0;
    if (dev > 0.25)
        dev = 0.25;
    return 1.0 - 2.0 * dev;
}

/* 单模型测量：主样本必成，备用样本失败时按单样本计（pairs=1），
 * 离散度以封顶值表达——样本量交由 cal_confidence 显式判定充足性。 */
static int rpc_count_tokens(const char *socket, const char *model, const char *text,
                            int64_t *out_tokens);

typedef struct {
    int64_t sum_zh;
    int64_t sum_en;
    uint32_t pairs;
    double dev;
} cal_meas_t;

static int cal_measure(const char *socket, const char *model, cal_meas_t *m)
{
    int64_t zh1 = 0, en1 = 0, zh2 = 0, en2 = 0;

    if (rpc_count_tokens(socket, model, AIRY_LANG_ZH_SAMPLE1, &zh1) != 0 ||
        rpc_count_tokens(socket, model, AIRY_LANG_EN_SAMPLE1, &en1) != 0 ||
        en1 <= 0)
        return -1;

    AIRY_MEMSET(m, 0, sizeof(*m));
    m->sum_zh = zh1;
    m->sum_en = en1;
    m->pairs = 1;
    m->dev = 0.25;

    if (rpc_count_tokens(socket, model, AIRY_LANG_ZH_SAMPLE2, &zh2) != 0 ||
        rpc_count_tokens(socket, model, AIRY_LANG_EN_SAMPLE2, &en2) != 0 ||
        en2 <= 0)
        return 0;

    m->sum_zh += zh2;
    m->sum_en += en2;
    m->pairs = 2;

    double r1 = (double)zh1 / (double)en1;
    double r2 = (double)zh2 / (double)en2;
    double hi = r1 > r2 ? r1 : r2;
    double lo = r1 < r2 ? r1 : r2;
    m->dev = hi > 0.0 ? (hi - lo) / hi : 0.25;
    return 0;
}

/* 经 llm_d count_tokens RPC 统计文本 token 数（按声明的分词族计数）。 */
static int rpc_count_tokens(const char *socket, const char *model, const char *text,
                            int64_t *out_tokens)
{
    char params[1024];
    int n = snprintf(params, sizeof(params),
                     "{\"family\":\"%s\",\"model\":\"%s\",\"text\":\"%s\"}",
                     token_family_of(model), model ? model : "", text);
    if (n < 0 || (size_t)n >= sizeof(params))
        return -1;

    /* The RPC client comes from the injected IPC ops table
     * (svc_common); degrade gracefully when absent. */
    const airy_ipc_ops_t *ipc_ops = are_ops_get_ipc();
    if (!ipc_ops || !ipc_ops->rpc_call)
        return -1;

    char *result = NULL;
    int rc = ipc_ops->rpc_call(socket, "count_tokens", params, &result,
                               AIRY_LANG_RPC_TIMEOUT_MS);
    if (rc != 0 || !result)
        return -1;

#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_Parse(result);
    AIRY_FREE(result);
    if (!root)
        return -1;
    cJSON *tok = cJSON_GetObjectItemCaseSensitive(root, "tokens");
    int64_t tokens = (tok && cJSON_IsNumber(tok)) ? (int64_t)tok->valuedouble : -1;
    cJSON_Delete(root);
#else
    /* 无 cJSON：手写扫描 {"tokens":123} */
    int64_t tokens = -1;
    const char *key = strstr(result, "\"tokens\"");
    if (key) {
        const char *colon = strchr(key, ':');
        if (colon)
            tokens = strtoll(colon + 1, NULL, 10);
    }
    AIRY_FREE(result);
#endif

    if (tokens < 0)
        return -1;
    *out_tokens = tokens;
    return 0;
}

/* 经 llm_d list_models RPC 获取模型名与其真实 provider（D-3：provider
 * 必须回填上游声明，不再留空或继承旧画像的过期值）。 */
typedef struct {
    char *name;
    char *provider;
} cal_model_t;

static void cal_models_free(cal_model_t *ms, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        AIRY_FREE(ms[i].name);
        AIRY_FREE(ms[i].provider);
    }
    AIRY_FREE(ms);
}

static int rpc_list_models(const char *socket, cal_model_t **out_models, size_t *out_count)
{
    /* The RPC client comes from the injected IPC ops table
     * (svc_common); degrade gracefully when absent. */
    const airy_ipc_ops_t *ipc_ops = are_ops_get_ipc();
    if (!ipc_ops || !ipc_ops->rpc_call)
        return -1;

    char *result = NULL;
    int rc = ipc_ops->rpc_call(socket, "list_models", "{}", &result,
                               AIRY_LANG_RPC_TIMEOUT_MS);
    if (rc != 0 || !result)
        return -1;

    cal_model_t *models = NULL;
    size_t count = 0;

#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_Parse(result);
    AIRY_FREE(result);
    if (!root)
        return -1;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (cJSON_IsArray(arr)) {
        cJSON *item = NULL;
        size_t cap = 0;
        cJSON_ArrayForEach(item, arr) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *prov = cJSON_GetObjectItemCaseSensitive(item, "provider");
            if (!cJSON_IsString(name) || !name->valuestring)
                continue;
            if (count >= AIRY_LANG_PROFILE_MAX) {
                /* 越界 fail-fast（B6-1）：模型数超出画像容量，整批拒绝 */
                cal_models_free(models, count);
                models = NULL;
                count = 0;
                cJSON_Delete(root);
                return -1;
            }
            if (count == cap) {
                cap = cap == 0 ? 8 : cap * 2;
                cal_model_t *tmp = AIRY_REALLOC(models, cap * sizeof(cal_model_t));
                if (!tmp)
                    break;
                models = tmp;
            }
            models[count].name = AIRY_STRDUP(name->valuestring);
            models[count].provider =
                (cJSON_IsString(prov) && prov->valuestring)
                    ? AIRY_STRDUP(prov->valuestring)
                    : AIRY_STRDUP("");
            if (!models[count].name || !models[count].provider) {
                AIRY_FREE(models[count].name);
                AIRY_FREE(models[count].provider);
                break;
            }
            count++;
        }
    }
    cJSON_Delete(root);
#else
    (void)models;
    (void)count;
#endif

    *out_models = models;
    *out_count = count;
    return (count > 0) ? 0 : -1;
}

static const char *lang_gw_profiles_path(const struct airy_lang_gateway *gw)
{
    return gw->profiles_path[0] ? gw->profiles_path : NULL;
}

/* 从 JSON 加载画像到网关实例。all-or-nothing：坏 JSON 或条目数超出
 * AIRY_LANG_PROFILE_MAX 时整批拒绝并返回错误，不部分接受（B6-1）——
 * 防止越界静默截断后随 save 重写文件造成画像永久丢失。 */
int profiles_load(struct airy_lang_gateway *gw)
{
    const char *path = lang_gw_profiles_path(gw);
    if (!path)
        return AIRY_EOK;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return AIRY_EOK; /* 首次启动无持久化文件属正常 */
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

#ifndef AIRY_HAS_CJSON
    (void)buf;
    return AIRY_EOK;
#else
    cJSON *root = cJSON_Parse(buf);
    if (!root)
        return AIRY_EINVAL;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return AIRY_EINVAL;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (gw->profile_count >= AIRY_LANG_PROFILE_MAX) {
            /* 越界 fail-fast（V6.2）：整批拒绝，保留原文件待修复 */
            gw->profile_count = 0;
            cJSON_Delete(root);
            return AIRY_EOVERFLOW;
        }
        cJSON *mid = cJSON_GetObjectItemCaseSensitive(item, "model_id");
        cJSON *ratio = cJSON_GetObjectItemCaseSensitive(item, "lang_ratio");
        cJSON *conf = cJSON_GetObjectItemCaseSensitive(item, "lang_confidence");
        cJSON *rhi = cJSON_GetObjectItemCaseSensitive(item, "ratio_high");
        cJSON *rlo = cJSON_GetObjectItemCaseSensitive(item, "ratio_low");
        cJSON *nl = cJSON_GetObjectItemCaseSensitive(item, "native_lang");
        cJSON *prov = cJSON_GetObjectItemCaseSensitive(item, "provider");
        cJSON *fam = cJSON_GetObjectItemCaseSensitive(item, "family");
        cJSON *cal = cJSON_GetObjectItemCaseSensitive(item, "calibrated_at");
        cJSON *samp = cJSON_GetObjectItemCaseSensitive(item, "lang_samples");
        if (!cJSON_IsString(mid) || !mid->valuestring)
            continue;
        size_t idx = gw->profile_count++;
        airy_model_profile_t *p = &gw->profiles[idx];
        /* 旧版持久化无本字段（真实样本量未知）→ 记 0，统计侧按样本不足
         * 呈现，不用「假定充足」掩盖未知。 */
        gw->prof_samples[idx] =
            cJSON_IsNumber(samp) && samp->valuedouble > 0
                ? (uint32_t)samp->valuedouble
                : 0;
        snprintf(p->model_id, sizeof(p->model_id), "%s", mid->valuestring);
        snprintf(p->provider, sizeof(p->provider), "%s",
                 cJSON_IsString(prov) && prov->valuestring ? prov->valuestring : "");
        snprintf(p->family, sizeof(p->family), "%s",
                 cJSON_IsString(fam) && fam->valuestring ? fam->valuestring : "");
        p->native_lang = (cJSON_IsString(nl) && nl->valuestring &&
                          strcmp(nl->valuestring, "zh") == 0)
                             ? AIRY_LANG_ZH
                             : AIRY_LANG_EN;
        p->lang_ratio = cJSON_IsNumber(ratio) ? ratio->valuedouble : 1.0;
        p->lang_confidence = cJSON_IsNumber(conf) ? conf->valuedouble : 0.9;
        /* 阈值同源（V6.3）：旧版持久化无阈值字段时以网关种子填充 */
        p->ratio_high =
            cJSON_IsNumber(rhi) && rhi->valuedouble > 0.0
                ? rhi->valuedouble
                : gw->ratio_high;
        p->ratio_low =
            cJSON_IsNumber(rlo) && rlo->valuedouble > 0.0 ? rlo->valuedouble
                                                          : gw->ratio_low;
        p->calibrated_at = cJSON_IsNumber(cal) ? (uint64_t)cal->valuedouble : 0;
    }
    cJSON_Delete(root);
    return AIRY_EOK;
#endif
}

/* 持久化画像到 JSON 文件。锁内仅做纯内存遍历（快照语义），文件 I/O
 * 留在锁外（B6-2：防止锁内磁盘等待拖住请求路径）。 */
int profiles_save(struct airy_lang_gateway *gw)
{
    const char *path = lang_gw_profiles_path(gw);
    if (!path)
        return -1;

#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return -1;
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON *arr = cJSON_AddArrayToObject(root, "profiles");
    airy_mtx_lock(&gw->prof_lock);
    for (size_t i = 0; i < gw->profile_count; i++) {
        const airy_model_profile_t *p = &gw->profiles[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "model_id", p->model_id);
        cJSON_AddStringToObject(item, "provider", p->provider);
        cJSON_AddStringToObject(item, "family", p->family);
        cJSON_AddStringToObject(item, "native_lang",
                                p->native_lang == AIRY_LANG_ZH ? "zh" : "en");
        cJSON_AddNumberToObject(item, "lang_ratio", p->lang_ratio);
        cJSON_AddNumberToObject(item, "lang_confidence", p->lang_confidence);
        cJSON_AddNumberToObject(item, "ratio_high", p->ratio_high);
        cJSON_AddNumberToObject(item, "ratio_low", p->ratio_low);
        cJSON_AddNumberToObject(item, "calibrated_at", (double)p->calibrated_at);
        cJSON_AddNumberToObject(item, "lang_samples", (double)gw->prof_samples[i]);
        cJSON_AddItemToArray(arr, item);
    }
    airy_mtx_unlock(&gw->prof_lock);
    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json)
        return -1;

    /* 确保目录存在（agentrt/lang/） */
    const char *slash = strrchr(path, '/');
    if (slash && slash != path) {
        char dir[512];
        size_t dlen = (size_t)(slash - path);
        if (dlen < sizeof(dir)) {
            AIRY_MEMCPY(dir, path, dlen);
            dir[dlen] = '\0';
            airy_mkdir_p(dir);
        }
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        AIRY_FREE(json);
        return -1;
    }
    fputs(json, fp);
    fclose(fp);
    AIRY_FREE(json);
    return 0;
#else
    (void)path;
    return -1;
#endif
}

airy_err_t lang_prof_store(struct airy_lang_gateway *gw,
                          const airy_model_profile_t *profile, uint32_t samples)
{
    if (!gw || !profile)
        return AIRY_EINVAL;

    airy_mtx_lock(&gw->prof_lock);
    airy_model_profile_t *dst = NULL;
    size_t idx = gw->profile_count;
    for (size_t i = 0; i < gw->profile_count; i++) {
        if (strcmp(gw->profiles[i].model_id, profile->model_id) == 0) {
            dst = &gw->profiles[i];
            idx = i;
            break;
        }
    }
    if (!dst && gw->profile_count >= AIRY_LANG_PROFILE_MAX) {
        airy_mtx_unlock(&gw->prof_lock);
        return AIRY_EOVERFLOW;
    }
    if (!dst) {
        dst = &gw->profiles[gw->profile_count++];
        idx = gw->profile_count - 1;
    }
    *dst = *profile;
    gw->prof_samples[idx] = samples;
    airy_mtx_unlock(&gw->prof_lock);

    /* 持久化在锁外执行（save 内部自行加锁快照），避免锁内文件 I/O */
    profiles_save(gw);
    return AIRY_EOK;
}

airy_err_t airy_lang_gateway_set_profile(airy_lang_gateway_t *gw,
                                         const airy_model_profile_t *profile)
{
    /* 公开写入无样本上下文：样本量记 0（未知），统计侧按样本不足呈现，
     * 不伪造「已充足」。校准器内部走 lang_prof_store 显式登记实测量。 */
    return lang_prof_store(gw, profile, 0);
}

airy_err_t airy_lang_gateway_get_profile(airy_lang_gateway_t *gw, const char *model_id,
                                         airy_model_profile_t *out)
{
    if (!gw || !model_id || !out)
        return AIRY_EINVAL;

    airy_mtx_lock(&gw->prof_lock);
    for (size_t i = 0; i < gw->profile_count; i++) {
        if (strcmp(gw->profiles[i].model_id, model_id) == 0) {
            *out = gw->profiles[i];
            airy_mtx_unlock(&gw->prof_lock);
            return AIRY_EOK;
        }
    }
    airy_mtx_unlock(&gw->prof_lock);
    return AIRY_ERR_NOT_FOUND;
}

/* 停机请求查询（calibrate_all 模型间检查点）。 */
static int cal_should_stop(struct airy_lang_gateway *gw)
{
    airy_mtx_lock(&gw->prof_lock);
    int stop = gw->cal_stop;
    airy_mtx_unlock(&gw->prof_lock);
    return stop;
}

/* worker 主循环：等待投递信号 → 解锁执行校准回合 → 循环。pending 合并
 * 为单回合（多次投递只跑一次）；回合内 RPC 有超时上界，模型间响应停机。
 * 出口清理线程局部错误状态（Windows TLS 无析构钩子；POSIX 由 pthread_key
 * 析构器兜底，此处为无操作）。 */
static void *cal_worker_main(void *arg)
{
    struct airy_lang_gateway *gw = arg;
    airy_mtx_lock(&gw->prof_lock);
    while (!gw->cal_stop) {
        if (!gw->cal_pending) {
            (void)airy_cond_timedwait(&gw->cal_wake, &gw->prof_lock,
                                      AIRY_LANG_WORKER_IDLE_MS);
            continue;
        }
        gw->cal_pending = 0;
        airy_mtx_unlock(&gw->prof_lock);

        (void)airy_lang_gateway_calibrate_all(gw, NULL);

        airy_mtx_lock(&gw->prof_lock);
    }
    airy_mtx_unlock(&gw->prof_lock);
    airy_err_thread_cleanup();
    return NULL;
}

void cal_worker_post(struct airy_lang_gateway *gw)
{
    if (!gw)
        return;
    airy_mtx_lock(&gw->prof_lock);
    gw->cal_pending = 1;
    if (!gw->cal_thread_valid) {
        /* 懒启动：首次投递才创建线程，零校准负载的端侧不占线程 */
        if (airy_platform_thread_create(&gw->cal_thread, cal_worker_main, gw) == 0)
            gw->cal_thread_valid = 1;
    }
    airy_cond_signal(&gw->cal_wake);
    airy_mtx_unlock(&gw->prof_lock);
}

void cal_worker_stop(struct airy_lang_gateway *gw)
{
    if (!gw)
        return;
    airy_mtx_lock(&gw->prof_lock);
    gw->cal_stop = 1;
    if (!gw->cal_thread_valid) {
        airy_mtx_unlock(&gw->prof_lock);
        return;
    }
    airy_cond_broadcast(&gw->cal_wake);
    airy_thread_t t = gw->cal_thread;
    airy_mtx_unlock(&gw->prof_lock);

    void *ret = NULL;
    (void)airy_platform_thread_join(t, &ret);
    airy_mtx_lock(&gw->prof_lock);
    gw->cal_thread_valid = 0;
    airy_mtx_unlock(&gw->prof_lock);
}

airy_err_t airy_lang_gateway_calibrate(airy_lang_gateway_t *gw, const char *model_id)
{
    if (!gw || !model_id || !model_id[0])
        return AIRY_EINVAL;

    if (!gw->llm_socket[0])
        return AIRY_ERR_NOT_FOUND;

    cal_meas_t m;
    if (cal_measure(gw->llm_socket, model_id, &m) != 0)
        return AIRY_ERR_NOT_FOUND;

    double ratio = cal_ratio_pooled((double)m.sum_zh, (double)m.sum_en);
    if (ratio <= 0.0)
        return AIRY_ERR_OVERFLOW;
    /* 置信度只由样本量与主备离散度导出；样本不足（备用样本失败）时
     * 返回 0 并在统计侧显式标注，不再以 0.5/0.6 冒充可信。 */
    double confidence = cal_confidence(m.pairs, m.dev);

    /* 推断 family/provider（复用既有画像字段，缺失时启发式）；路由阈值
     * 同源继承（V6.3）：已有画像保留其阈值，新画像以网关种子为初值 */
    char provider[64] = "";
    char family[64] = "Other";
    double ratio_high = gw->ratio_high;
    double ratio_low = gw->ratio_low;
    airy_model_profile_t old;
    if (airy_lang_gateway_get_profile(gw, model_id, &old) == AIRY_EOK) {
        snprintf(provider, sizeof(provider), "%s", old.provider);
        snprintf(family, sizeof(family), "%s", old.family);
        if (old.ratio_high > 0.0)
            ratio_high = old.ratio_high;
        if (old.ratio_low > 0.0)
            ratio_low = old.ratio_low;
    }
    family_from_model(model_id, provider[0] ? provider : NULL, family, sizeof(family));

    airy_model_profile_t p;
    AIRY_MEMSET(&p, 0, sizeof(p));
    snprintf(p.model_id, sizeof(p.model_id), "%s", model_id);
    snprintf(p.provider, sizeof(p.provider), "%s", provider);
    snprintf(p.family, sizeof(p.family), "%s", family);
    p.native_lang = infer_native_lang(family, provider, ratio);
    p.lang_ratio = ratio;
    p.lang_confidence = confidence;
    p.ratio_high = ratio_high;
    p.ratio_low = ratio_low;
    p.calibrated_at = (uint64_t)time(NULL);

    airy_mtx_lock(&gw->prof_lock);
    gw->calibrate_count++;
    airy_mtx_unlock(&gw->prof_lock);
    return lang_prof_store(gw, &p, m.pairs);
}

/* per-family 校准（D-3）：先把同族模型的样本池化，再以族合并比率与
 * 族样本量回写每个成员画像；provider 取 list_models 的真实声明。 */
typedef struct {
    char family[64];
    char provider[64];
    double sum_zh;
    double sum_en;
    double min_r;
    double max_r;
    uint32_t samples;
} cal_fam_t;

typedef struct {
    char *name;
    char *provider;
    char family[64];
    cal_meas_t m;
    int ok;
} cal_row_t;

static cal_fam_t *fam_find(cal_fam_t *fams, size_t nfam, const char *family)
{
    for (size_t i = 0; i < nfam; i++) {
        if (strcmp(fams[i].family, family) == 0)
            return &fams[i];
    }
    return NULL;
}

static void fam_add(cal_fam_t *fam, const cal_row_t *row)
{
    double r = cal_ratio_pooled((double)row->m.sum_zh, (double)row->m.sum_en);

    if (fam->samples == 0) {
        snprintf(fam->provider, sizeof(fam->provider), "%s", row->provider);
        fam->min_r = r;
        fam->max_r = r;
    } else {
        if (r < fam->min_r)
            fam->min_r = r;
        if (r > fam->max_r)
            fam->max_r = r;
    }
    fam->sum_zh += (double)row->m.sum_zh;
    fam->sum_en += (double)row->m.sum_en;
    fam->samples += row->m.pairs;
}

airy_err_t airy_lang_gateway_calibrate_all(airy_lang_gateway_t *gw, uint32_t *out_count)
{
    if (!gw)
        return AIRY_EINVAL;

    if (!gw->llm_socket[0]) {
        if (out_count)
            *out_count = 0;
        return AIRY_ERR_NOT_FOUND;
    }

    cal_model_t *models = NULL;
    size_t count = 0;
    if (rpc_list_models(gw->llm_socket, &models, &count) != 0) {
        if (out_count)
            *out_count = 0;
        return AIRY_ERR_NOT_FOUND;
    }

    cal_row_t *rows = AIRY_CALLOC(count, sizeof(cal_row_t));
    cal_fam_t *fams = AIRY_CALLOC(count, sizeof(cal_fam_t));
    size_t nfam = 0;
    uint32_t ok = 0;

    if (!rows || !fams) {
        AIRY_FREE(rows);
        AIRY_FREE(fams);
        cal_models_free(models, count);
        if (out_count)
            *out_count = 0;
        return AIRY_ERR_OVERFLOW;
    }

    /* 回合一：逐模型采样，按族池化 */
    for (size_t i = 0; i < count; i++) {
        if (cal_should_stop(gw))
            break; /* 停机请求：放弃剩余模型，回合上界受控 */
        rows[i].name = models[i].name;
        rows[i].provider = models[i].provider;
        snprintf(rows[i].family, sizeof(rows[i].family), "%s", "Other");
        family_from_model(rows[i].name,
                          rows[i].provider[0] ? rows[i].provider : NULL,
                          rows[i].family, sizeof(rows[i].family));
        if (cal_measure(gw->llm_socket, rows[i].name, &rows[i].m) != 0) {
            rows[i].ok = 0;
            continue;
        }
        rows[i].ok = 1;
        cal_fam_t *fam = fam_find(fams, nfam, rows[i].family);
        if (!fam && nfam < count) {
            fam = &fams[nfam++];
            snprintf(fam->family, sizeof(fam->family), "%s", rows[i].family);
        }
        if (fam)
            fam_add(fam, &rows[i]);
    }

    /* 回合二：以族合并事实回写成员画像 */
    for (size_t i = 0; i < count; i++) {
        if (!rows[i].ok || cal_should_stop(gw))
            continue;
        cal_fam_t *fam = fam_find(fams, nfam, rows[i].family);
        if (!fam || fam->samples == 0)
            continue;

        double ratio = cal_ratio_pooled(fam->sum_zh, fam->sum_en);
        if (ratio <= 0.0)
            continue;
        double dev = fam->max_r > 0.0 ? (fam->max_r - fam->min_r) / fam->max_r : 0.25;

        char provider[64] = "";
        double ratio_high = gw->ratio_high;
        double ratio_low = gw->ratio_low;
        airy_model_profile_t old;
        if (airy_lang_gateway_get_profile(gw, rows[i].name, &old) == AIRY_EOK) {
            snprintf(provider, sizeof(provider), "%s", old.provider);
            if (old.ratio_high > 0.0)
                ratio_high = old.ratio_high;
            if (old.ratio_low > 0.0)
                ratio_low = old.ratio_low;
        }
        if (rows[i].provider[0])
            snprintf(provider, sizeof(provider), "%s", rows[i].provider);

        airy_model_profile_t p;
        AIRY_MEMSET(&p, 0, sizeof(p));
        snprintf(p.model_id, sizeof(p.model_id), "%s", rows[i].name);
        snprintf(p.provider, sizeof(p.provider), "%s", provider);
        snprintf(p.family, sizeof(p.family), "%s", fam->family);
        p.native_lang = infer_native_lang(fam->family, provider, ratio);
        p.lang_ratio = ratio;
        p.lang_confidence = cal_confidence(fam->samples, dev);
        p.ratio_high = ratio_high;
        p.ratio_low = ratio_low;
        p.calibrated_at = (uint64_t)time(NULL);
        if (lang_prof_store(gw, &p, fam->samples) == AIRY_EOK)
            ok++;
    }

    if (ok > 0) {
        airy_mtx_lock(&gw->prof_lock);
        gw->calibrate_count += ok;
        airy_mtx_unlock(&gw->prof_lock);
    }

    AIRY_FREE(rows);
    AIRY_FREE(fams);
    cal_models_free(models, count);

    if (out_count)
        *out_count = ok;
    return ok > 0 ? AIRY_EOK : AIRY_ERR_NOT_FOUND;
}
