// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_lang_gateway.c - 推理语言网关单元测试
 *
 * 覆盖：语言检测、任务分类、多因子路由决策（含 V6.3 阈值画像化）、
 * System Prompt 注入、网关 process（Canonical Data Format）、输出后处理
 * （标签解析/漂移/术语）、画像 set/get、指令计数 tick、token 估算、
 * 真实 RPC 校准链路（置信度导出/阈值继承）、校准后台 worker 异步化。
 */

#include "lang_gateway.h"
#include "airy_ipc_ops.h"

#include "airy_memory.h"
#include "platform.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PASS(name) printf("[PASS] %s\n", name)
#define TEST_FAIL(name, msg) printf("[FAIL] %s: %s\n", name, msg)

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name, msg)    \
    do {                          \
        if (cond) {               \
            TEST_PASS(name);      \
            g_pass++;             \
        } else {                  \
            TEST_FAIL(name, msg); \
            g_fail++;             \
        }                         \
    } while (0)

static void test_detect_language(void)
{
    airy_lang_t lang;
    double conf = 0.0;

    CHECK(airy_lang_detect("请解释梯度下降算法", &lang, &conf) == AIRY_EOK &&
              lang == AIRY_LANG_ZH,
          "detect zh input", "中文输入未识别为中文");
    CHECK(airy_lang_detect("Explain gradient descent algorithm", &lang, &conf) == AIRY_EOK &&
              lang == AIRY_LANG_EN,
          "detect en input", "英文输入未识别为英文");
    CHECK(airy_lang_detect("def foo(x): return x + 1", &lang, &conf) == AIRY_EOK &&
              lang == AIRY_LANG_EN,
          "detect code as en", "代码应视为英文");
    CHECK(airy_lang_detect("abc", &lang, &conf) == AIRY_EOK &&
              lang == AIRY_LANG_EN,
          "detect short ascii", "短 ASCII 文本应视为英文");
    CHECK(airy_lang_detect("ab", &lang, &conf) == AIRY_EOK &&
              lang == AIRY_LANG_UNKNOWN,
          "detect too short unknown", "<3 字符应返回 unknown");
}

static void test_classify_task(void)
{
    airy_lang_task_t task = AIRY_LANG_TASK_GENERAL;

    CHECK(airy_lang_classify_task("帮我写一个排序算法", &task) == AIRY_EOK &&
              task == AIRY_LANG_TASK_CODE,
          "classify code", "代码任务未识别");
    CHECK(airy_lang_classify_task("解释一下红楼梦的典故", &task) == AIRY_EOK &&
              task == AIRY_LANG_TASK_CULTURE,
          "classify culture", "文化任务未识别");
    CHECK(airy_lang_classify_task("新政策对中小企业有什么影响", &task) == AIRY_EOK &&
              task == AIRY_LANG_TASK_POLICY,
          "classify policy", "政策任务未识别");
    CHECK(airy_lang_classify_task("计算微积分的导数", &task) == AIRY_EOK &&
              task == AIRY_LANG_TASK_MATH,
          "classify math", "数学任务未识别");
    CHECK(airy_lang_classify_task("今天天气怎么样", &task) == AIRY_EOK &&
              task == AIRY_LANG_TASK_GENERAL,
          "classify general", "普通问答未识别为 general");
}

static void test_route_hard_rule(void)
{
    airy_lang_signals_t s;
    memset(&s, 0, sizeof(s));
    s.detected_lang = AIRY_LANG_EN;
    s.task_type = AIRY_LANG_TASK_CULTURE;

    airy_lang_decision_t d;
    memset(&d, 0, sizeof(d));
    airy_model_profile_t p;
    memset(&p, 0, sizeof(p));
    p.native_lang = AIRY_LANG_EN;
    p.lang_ratio = 1.8;

    CHECK(airy_lang_route(&s, &p, &d) == AIRY_EOK &&
              d.reasoning_lang == AIRY_LANG_ZH && d.output_lang == AIRY_LANG_ZH,
          "route hard rule forces zh", "文化任务未强制中文");
    CHECK(d.decision_chain != NULL && strstr(d.decision_chain, "hard_rule") != NULL,
          "route chain recorded", "决策链缺失");
    AIRY_FREE(d.decision_reason);
    AIRY_FREE(d.decision_chain);
}

static void test_route_ratio_high(void)
{
    airy_lang_signals_t s;
    memset(&s, 0, sizeof(s));
    s.detected_lang = AIRY_LANG_ZH;
    s.task_type = AIRY_LANG_TASK_GENERAL;

    airy_model_profile_t p;
    memset(&p, 0, sizeof(p));
    p.native_lang = AIRY_LANG_EN;
    p.lang_ratio = 1.82; /* 中文极费 token → 英文推理，输出仍跟随用户 */

    airy_lang_decision_t d;
    memset(&d, 0, sizeof(d));
    CHECK(airy_lang_route(&s, &p, &d) == AIRY_EOK &&
              d.reasoning_lang == AIRY_LANG_EN && d.output_lang == AIRY_LANG_ZH,
          "route ratio high switches reasoning to en",
          "lang_ratio>1.35 未切换英文推理或输出语言未跟随用户");
    AIRY_FREE(d.decision_reason);
    AIRY_FREE(d.decision_chain);
}

static void test_route_native_fallback(void)
{
    airy_lang_signals_t s;
    memset(&s, 0, sizeof(s));
    s.detected_lang = AIRY_LANG_ZH;
    s.task_type = AIRY_LANG_TASK_GENERAL;

    airy_model_profile_t p;
    memset(&p, 0, sizeof(p));
    p.native_lang = AIRY_LANG_ZH;
    p.lang_ratio = 0.92; /* 中庸区间 → 跟随原生中文 */

    airy_lang_decision_t d;
    memset(&d, 0, sizeof(d));
    CHECK(airy_lang_route(&s, &p, &d) == AIRY_EOK &&
              d.reasoning_lang == AIRY_LANG_ZH,
          "route native fallback zh", "中庸区间未跟随原生语言");
    AIRY_FREE(d.decision_reason);
    AIRY_FREE(d.decision_chain);
}

static void test_route_no_profile(void)
{
    airy_lang_signals_t s;
    memset(&s, 0, sizeof(s));
    s.detected_lang = AIRY_LANG_EN;
    s.task_type = AIRY_LANG_TASK_GENERAL;

    airy_lang_decision_t d;
    memset(&d, 0, sizeof(d));
    CHECK(airy_lang_route(&s, NULL, &d) == AIRY_EOK &&
              d.reasoning_lang == AIRY_LANG_EN && d.output_lang == AIRY_LANG_EN,
          "route no profile input aligned", "无画像时应按输入语言对齐");
    AIRY_FREE(d.decision_reason);
    AIRY_FREE(d.decision_chain);
}

/* B6-3 V6.3：路由阈值从画像读取（同源同值）——改画像阈值即改路由。 */
static void test_route_threshold_from_profile(void)
{
    airy_lang_signals_t s;
    airy_model_profile_t p;
    airy_lang_decision_t d;

    /* 放宽高阈值至 2.0：EN 输入 + ratio=1.5 不再进对齐层（旧硬编码 1.2
     * 会触发 en 全对齐），落到中庸区间跟随原生 */
    memset(&s, 0, sizeof(s));
    s.detected_lang = AIRY_LANG_EN;
    s.task_type = AIRY_LANG_TASK_GENERAL;
    memset(&p, 0, sizeof(p));
    p.native_lang = AIRY_LANG_EN;
    p.lang_ratio = 1.5;
    p.ratio_high = 2.0;
    p.ratio_low = 0.4;
    memset(&d, 0, sizeof(d));
    CHECK(airy_lang_route(&s, &p, &d) == AIRY_EOK &&
              d.reasoning_lang == AIRY_LANG_EN && d.decision_reason &&
              strcmp(d.decision_reason, "native_fallback_en") == 0,
          "route threshold from profile high",
          "改画像 ratio_high 未生效（V6.3 同源同值）");
    AIRY_FREE(d.decision_reason);
    AIRY_FREE(d.decision_chain);

    /* 收紧低阈值至 0.95：ZH 输入 + ratio=0.92 进对齐层全中文（旧默认
     * 0.85 会落中庸区间跟随原生） */
    memset(&s, 0, sizeof(s));
    s.detected_lang = AIRY_LANG_ZH;
    s.task_type = AIRY_LANG_TASK_GENERAL;
    memset(&p, 0, sizeof(p));
    p.native_lang = AIRY_LANG_EN;
    p.lang_ratio = 0.92;
    p.ratio_high = 1.35;
    p.ratio_low = 0.95;
    memset(&d, 0, sizeof(d));
    CHECK(airy_lang_route(&s, &p, &d) == AIRY_EOK &&
              d.reasoning_lang == AIRY_LANG_ZH && d.output_lang == AIRY_LANG_ZH &&
              d.decision_reason &&
              strcmp(d.decision_reason, "input_zh_efficient") == 0,
          "route threshold from profile low",
          "改画像 ratio_low 未生效（V6.3 同源同值）");
    AIRY_FREE(d.decision_reason);
    AIRY_FREE(d.decision_chain);
}

static void test_system_prompt(void)
{
    char *prompt = NULL;
    CHECK(airy_lang_build_system_prompt(AIRY_LANG_ZH, AIRY_LANG_ZH, &prompt) ==
                  AIRY_EOK &&
              prompt && strstr(prompt, "<thinking>") && strstr(prompt, "<answer>"),
          "build system prompt", "语言约束 prompt 生成失败");
    CHECK(prompt && strstr(prompt, "中文"), "prompt contains zh",
          "中文约束缺失");
    AIRY_FREE(prompt);
}

static void test_gateway_process(void)
{
    airy_lang_gateway_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 0;
    cfg.recalibrate_interval = 3;

    airy_lang_gateway_t *gw = NULL;
    CHECK(airy_lang_gateway_create(&cfg, &gw) == AIRY_EOK && gw != NULL,
          "gateway create", "网关创建失败");
    if (!gw)
        return;

    airy_model_profile_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.model_id, sizeof(p.model_id), "gpt-4o-test");
    snprintf(p.provider, sizeof(p.provider), "openai");
    snprintf(p.family, sizeof(p.family), "GPT");
    p.native_lang = AIRY_LANG_EN;
    p.lang_ratio = 1.82;
    p.lang_confidence = 0.87;
    p.calibrated_at = 1700000000ULL;
    CHECK(airy_lang_gateway_set_profile(gw, &p) == AIRY_EOK,
          "gateway set profile", "画像写入失败");

    airy_canonical_request_t *req = NULL;
    CHECK(airy_lang_gateway_process(gw, "请解释梯度下降算法", "gpt-4o-test", 0,
                                    &req) == AIRY_EOK &&
              req != NULL,
          "gateway process", "网关处理失败");
    if (req) {
        CHECK(req->request_id && req->request_id[0], "request id",
              "request_id 缺失");
        CHECK(req->signals.detected_lang == AIRY_LANG_ZH, "signals zh",
              "信号语言错误");
        CHECK(req->routing.decision_chain && req->routing.decision_chain[0],
              "decision chain", "决策链缺失");
        CHECK(req->system_prompt && req->system_prompt[0], "system prompt",
              "语言约束 prompt 缺失");
        CHECK(req->telemetry_json && strstr(req->telemetry_json, "reasoning_lang"),
              "telemetry", "遥测缺失");
        airy_lang_gateway_free_canonical(req);
    }

    /* 画像查询 */
    airy_model_profile_t got;
    memset(&got, 0, sizeof(got));
    CHECK(airy_lang_gateway_get_profile(gw, "gpt-4o-test", &got) == AIRY_EOK &&
              got.lang_ratio == 1.82,
          "gateway get profile", "画像读取失败");

    /* 指令计数触发重校准（llm_d 不可用 → 降级，但计数归零 + 返回 1） */
    CHECK(airy_lang_gateway_tick(gw) == 0, "tick no trigger", "前两次不应触发");
    CHECK(airy_lang_gateway_tick(gw) == 0, "tick no trigger 2", "第三次前不应触发");
    int triggered = airy_lang_gateway_tick(gw);
    CHECK(triggered == 1, "tick triggers recalibrate",
          "达到阈值应触发重校准（llm_d 不可用时降级）");

    char *stats = NULL;
    CHECK(airy_lang_gateway_stats(gw, &stats) == AIRY_EOK && stats &&
              strstr(stats, "\"process_count\":1") &&
              strstr(stats, "\"profile_count\":1") &&
              strstr(stats, "\"model_id\":\"gpt-4o-test\"") &&
              strstr(stats, "\"provider\":\"openai\"") &&
              strstr(stats, "\"lang_confidence\":0.870") &&
              strstr(stats, "\"calibrated_at\":1700000000"),
          "gateway stats", "校准状态不可判读");
    AIRY_FREE(stats);

    airy_lang_gateway_destroy(gw);
}

static void test_post_process(void)
{
    airy_lang_gateway_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 0;
    airy_lang_gateway_t *gw = NULL;
    airy_lang_gateway_create(&cfg, &gw);
    if (!gw)
        return;

    /* 标签解析：剥离 thinking，取 answer */
    char *out = NULL;
    const char *raw = "<thinking>Let me analyze.</thinking>\n<answer>根据分析，答案是42。</answer>";
    CHECK(airy_lang_gateway_post_process(gw, raw, AIRY_LANG_ZH, &out) == AIRY_EOK &&
              out && strstr(out, "42") && !strstr(out, "<thinking>"),
          "post process tags", "标签解析失败");
    AIRY_FREE(out);

    /* 术语替换：overfitting → 过拟合 */
    const char *eng = "The model suffers from overfitting.";
    CHECK(airy_lang_gateway_post_process(gw, eng, AIRY_LANG_ZH, &out) == AIRY_EOK &&
              out && strstr(out, "过拟合"),
          "post process glossary", "术语表未生效");
    AIRY_FREE(out);

    /* 原死配置表项（中文长于英文曾被运行时守卫静默跳过）：X-macro 化后
     * 编译期保证替换不增长，替换应真实生效 */
    CHECK(airy_lang_gateway_post_process(gw, "The embedding layer.", AIRY_LANG_ZH, &out) ==
                  AIRY_EOK &&
              out && strstr(out, "嵌入") && !strstr(out, "embedding"),
          "post process glossary embedding", "embedding→嵌入 应生效（原死配置）");
    AIRY_FREE(out);

    /* 无标签原始文本原样通过 */
    CHECK(airy_lang_gateway_post_process(gw, "  你好   世界  ", AIRY_LANG_ZH, &out) ==
                  AIRY_EOK &&
              out && strstr(out, "你好") && strstr(out, "世界") &&
              !strstr(out, "    "),
          "post process normalize", "空白归一失败");
    AIRY_FREE(out);

    airy_lang_gateway_destroy(gw);
}

static void test_estimate_tokens(void)
{
    uint32_t t = 0;
    CHECK(airy_lang_estimate_tokens("深度学习", &t) == AIRY_EOK && t > 0,
          "estimate tokens zh", "中文估算失败");
    CHECK(airy_lang_estimate_tokens("hello world", &t) == AIRY_EOK && t > 0,
          "estimate tokens en", "英文估算失败");
}

/* B6-1 V6.2：画像容量越界 fail-fast。容量上界与 canonical.h
 * AIRY_LANG_PROFILE_MAX（32）一致，容量变更时此处应同步失败提醒。 */
static void test_profile_capacity(void)
{
    const char *path = "/tmp/airy_t_lg_cap.json";
    remove(path);

    airy_lang_gateway_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 0;
    cfg.profiles_path = path;

    airy_lang_gateway_t *gw = NULL;
    CHECK(airy_lang_gateway_create(&cfg, &gw) == AIRY_EOK && gw != NULL,
          "capacity create", "容量测试网关创建失败");
    if (!gw)
        return;

    int full = 1;
    for (int i = 0; i < 32; i++) {
        airy_model_profile_t p;
        memset(&p, 0, sizeof(p));
        snprintf(p.model_id, sizeof(p.model_id), "m%d", i);
        p.lang_ratio = 1.0;
        if (airy_lang_gateway_set_profile(gw, &p) != AIRY_EOK) {
            full = 0;
            break;
        }
    }
    CHECK(full, "capacity fill 32", "写入 32 条画像失败");

    airy_model_profile_t extra;
    memset(&extra, 0, sizeof(extra));
    snprintf(extra.model_id, sizeof(extra.model_id), "overflow-model");
    CHECK(airy_lang_gateway_set_profile(gw, &extra) == AIRY_EOVERFLOW,
          "capacity overflow fail-fast", "第 33 条画像应显式拒绝而非静默截断");

    airy_lang_gateway_destroy(gw);
    remove(path);
}

/* B6-1 V6.2：越界持久化文件整批拒绝（all-or-nothing，不部分接受）；
 * 正常文件读回 lang_confidence（save/load 同源）；坏 JSON 显式拒绝。 */
static void test_profiles_load(void)
{
    /* 33 条越界文件 → 整批拒绝，网关空画像启动 */
    const char *path = "/tmp/airy_t_lg_ovf.json";
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL, "overflow file open", "临时文件创建失败");
    if (fp) {
        fputs("{\"version\":1,\"profiles\":[", fp);
        for (int i = 0; i < 33; i++)
            fprintf(fp, "%s{\"model_id\":\"m%d\",\"lang_ratio\":1.5}", i ? "," : "", i);
        fputs("]}", fp);
        fclose(fp);
    }

    airy_lang_gateway_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 0;
    cfg.profiles_path = path;
    airy_lang_gateway_t *gw = NULL;
    CHECK(airy_lang_gateway_create(&cfg, &gw) == AIRY_EOK && gw != NULL,
          "overflow create survives", "越界文件不应阻断网关创建");
    if (gw) {
        airy_model_profile_t got;
        memset(&got, 0, sizeof(got));
        CHECK(airy_lang_gateway_get_profile(gw, "m0", &got) == AIRY_ERR_NOT_FOUND,
              "overflow batch rejected", "越界画像应整批拒绝而非部分加载");
        airy_lang_gateway_destroy(gw);
    }
    remove(path);

    /* 单条合法文件 → 加载成功且 lang_confidence 读回持久化值 */
    path = "/tmp/airy_t_lg_ok.json";
    fp = fopen(path, "wb");
    CHECK(fp != NULL, "valid file open", "临时文件创建失败");
    if (fp) {
        fprintf(fp,
                "{\"version\":1,\"profiles\":[{\"model_id\":\"cm\","
                "\"lang_ratio\":1.4,\"lang_confidence\":0.66,"
                "\"native_lang\":\"en\",\"calibrated_at\":1700000001}]}");
        fclose(fp);
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 0;
    cfg.profiles_path = path;
    gw = NULL;
    CHECK(airy_lang_gateway_create(&cfg, &gw) == AIRY_EOK && gw != NULL,
          "valid file load", "合法画像文件加载失败");
    if (gw) {
        airy_model_profile_t got;
        memset(&got, 0, sizeof(got));
        CHECK(airy_lang_gateway_get_profile(gw, "cm", &got) == AIRY_EOK &&
                  got.lang_confidence == 0.66 && got.lang_ratio == 1.4 &&
                  got.ratio_high == 1.35 && got.ratio_low == 0.85,
              "confidence roundtrip",
              "lang_confidence 应读回持久化值；旧版文件无阈值应以种子回填（V6.3）");
        airy_lang_gateway_destroy(gw);
    }
    remove(path);

    /* 坏 JSON → 拒绝使用，网关空画像启动 */
    path = "/tmp/airy_t_lg_bad.json";
    fp = fopen(path, "wb");
    CHECK(fp != NULL, "bad file open", "临时文件创建失败");
    if (fp) {
        fputs("not-json", fp);
        fclose(fp);
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 0;
    cfg.profiles_path = path;
    gw = NULL;
    CHECK(airy_lang_gateway_create(&cfg, &gw) == AIRY_EOK && gw != NULL,
          "bad json survives", "坏文件不应阻断网关创建");
    if (gw) {
        airy_model_profile_t got;
        memset(&got, 0, sizeof(got));
        CHECK(airy_lang_gateway_get_profile(gw, "cm", &got) == AIRY_ERR_NOT_FOUND,
              "bad json rejected", "坏 JSON 应整批拒绝");
        airy_lang_gateway_destroy(gw);
    }
    remove(path);
}

/* B6-2 V6.1：校准后台 worker 异步化。注入慢 RPC（300ms）验证：
 * create 首轮与 tick 投递均不阻塞请求路径；destroy 正确 join 不悬挂。 */
static int slow_rpc_call(const char *socket_path, const char *method,
                         const char *params_json, char **out_result_json,
                         uint32_t timeout_ms)
{
    (void)socket_path;
    (void)method;
    (void)params_json;
    (void)timeout_ms;
    airy_sleep_ms(300); /* 模拟慢 llm_d：远大于请求路径预算 */
    *out_result_json = NULL;
    return -1;
}

/* 真 count_tokens mock：按样本文本返回确定性 token 数。
 * zh1=12/en1=10（ratio 1.2），zh2=6/en2=5（ratio 1.2）——主备一致。 */
static int count_rpc_call(const char *socket_path, const char *method,
                          const char *params_json, char **out_result_json,
                          uint32_t timeout_ms)
{
    (void)socket_path;
    (void)timeout_ms;
    if (!method || strcmp(method, "count_tokens") != 0 || !params_json)
        return -1;
    long tokens;
    if (strstr(params_json, "深度学习的核心"))
        tokens = 12;
    else if (strstr(params_json, "The core of deep learning"))
        tokens = 10;
    else if (strstr(params_json, "人工智能正在改变世界"))
        tokens = 6;
    else if (strstr(params_json, "Artificial intelligence is changing"))
        tokens = 5;
    else
        return -1;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"tokens\":%ld}", tokens);
    *out_result_json = AIRY_STRDUP(buf);
    return *out_result_json ? 0 : -1;
}

/* 附加修复③：lang_confidence 由主/备样本测量一致性导出（非拍值）。
 * 确定性 mock 驱动真实 RPC 校准链路，断言 lang_ratio/置信度/种子阈值
 * 初值/重校准阈值继承（V6.3 同源同值）。 */
static void test_calibrate_real_chain(void)
{
    airy_ipc_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.rpc_call = count_rpc_call;
    are_ops_set_ipc(&ops);

    const char *path = "/tmp/airy_t_lg_cal.json";
    remove(path);

    airy_lang_gateway_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 0;
    cfg.llm_socket = "/tmp/airy_t_lg_ct.sock";
    cfg.profiles_path = path;

    airy_lang_gateway_t *gw = NULL;
    CHECK(airy_lang_gateway_create(&cfg, &gw) == AIRY_EOK && gw != NULL,
          "calibrate chain create", "网关创建失败");
    if (!gw) {
        are_ops_set_ipc(NULL);
        return;
    }

    CHECK(airy_lang_gateway_calibrate(gw, "m1") == AIRY_EOK,
          "calibrate via rpc", "真实 RPC 链路校准失败");

    airy_model_profile_t p;
    memset(&p, 0, sizeof(p));
    CHECK(airy_lang_gateway_get_profile(gw, "m1", &p) == AIRY_EOK &&
              p.lang_ratio > 1.19 && p.lang_ratio < 1.21,
          "calibrate ratio measured", "lang_ratio 应为主备样本均值比 1.2");
    CHECK(p.lang_confidence == 1.0, "calibrate confidence converged",
          "主备样本一致（ratio 同为 1.2）时置信度应为上限 1.0");
    CHECK(p.ratio_high == 1.35 && p.ratio_low == 0.85,
          "calibrate seeds thresholds", "新画像阈值应以网关种子为初值（V6.3）");
    CHECK(p.native_lang == AIRY_LANG_EN && p.calibrated_at > 0,
          "calibrate native and time", "family=Other 应推断英文原生且时间戳有效");

    /* V6.3 继承：重校准保留既有画像阈值，不被种子覆盖 */
    airy_model_profile_t tuned;
    memset(&tuned, 0, sizeof(tuned));
    snprintf(tuned.model_id, sizeof(tuned.model_id), "%s", "m1");
    tuned.ratio_high = 2.0;
    tuned.ratio_low = 0.5;
    CHECK(airy_lang_gateway_set_profile(gw, &tuned) == AIRY_EOK,
          "calibrate set tuned", "注入调优阈值失败");
    CHECK(airy_lang_gateway_calibrate(gw, "m1") == AIRY_EOK,
          "calibrate again", "二次校准失败");
    airy_model_profile_t after;
    memset(&after, 0, sizeof(after));
    CHECK(airy_lang_gateway_get_profile(gw, "m1", &after) == AIRY_EOK &&
              after.ratio_high == 2.0 && after.ratio_low == 0.5,
          "calibrate inherits thresholds", "重校准应继承既有画像阈值（V6.3 防重置）");

    airy_lang_gateway_destroy(gw);
    are_ops_set_ipc(NULL);
    remove(path);
}

static void test_async_calibrate(void)
{
    airy_ipc_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.rpc_call = slow_rpc_call;
    are_ops_set_ipc(&ops);

    airy_lang_gateway_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 1;
    cfg.recalibrate_interval = 1;
    cfg.llm_socket = "/tmp/airy_t_lg_slow.sock";
    cfg.profiles_path = "/tmp/airy_t_lg_async.json";

    uint64_t t0 = airy_time_wall_ms();
    airy_lang_gateway_t *gw = NULL;
    int crc = airy_lang_gateway_create(&cfg, &gw);
    uint64_t create_ms = airy_time_wall_ms() - t0;
    CHECK(crc == AIRY_EOK && gw != NULL, "async create ok", "网关创建失败");
    if (!gw) {
        are_ops_set_ipc(NULL);
        return;
    }
    CHECK(create_ms < 150, "async create nonblocking",
          "create 被首轮校准阻塞（应后台化执行）");

    /* 等 worker 完成首轮（慢 RPC 300ms），回到空闲等待 */
    airy_sleep_ms(400);

    t0 = airy_time_wall_ms();
    int trig = airy_lang_gateway_tick(gw);
    uint64_t tick_ms = airy_time_wall_ms() - t0;
    CHECK(trig == 1, "async tick triggers", "到期 tick 应返回 1");
    CHECK(tick_ms < 150, "async tick nonblocking",
          "tick 被同步校准阻塞（应投递后台）");

    /* 等 worker 完成第二回合，destroy join 应即时返回 */
    airy_sleep_ms(400);
    t0 = airy_time_wall_ms();
    airy_lang_gateway_destroy(gw);
    uint64_t destroy_ms = airy_time_wall_ms() - t0;
    CHECK(destroy_ms < 500, "async destroy joins",
          "destroy 应 join worker 而非长时间阻塞或悬挂");

    are_ops_set_ipc(NULL);
    remove("/tmp/airy_t_lg_async.json");
}

int main(void)
{
    printf("=== lang_gateway unit tests ===\n");

    test_detect_language();
    test_classify_task();
    test_route_hard_rule();
    test_route_ratio_high();
    test_route_native_fallback();
    test_route_no_profile();
    test_route_threshold_from_profile();
    test_system_prompt();
    test_gateway_process();
    test_post_process();
    test_estimate_tokens();
    test_profile_capacity();
    test_profiles_load();
    test_calibrate_real_chain();
    test_async_calibrate();

    printf("=== lang_gateway: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
