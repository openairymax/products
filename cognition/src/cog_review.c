// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cog_review.c
 * @brief 认知并行审查（Cognitive Parallel Review, CPR）策略载荷实现。
 *
 * 认知阶段多子 agent 并行审查策略：对用户指令排出多个认知子 agent
 * （认知确认 + 问题/边界/覆盖审查），各自并行独立调用 LLM（真实非流式
 * 生成），互不参考；完成后汇总为认知决策 JSON（意图加权投票 + 风险
 * 合并），供认知主链（规划/蓝图/GRAD）参考。LLM 不可用自动降级。
 *
 * 机制/策略分离（0.1.19 M5-4）：本文件为**策略载荷**，由 daemon（think_d）
 * 在启动期经 are_ops_set_cog_review() 注入；机制核 atoms/coreloopthree
 * 仅保留审查数据类型与 ops 分发面（cognitive_review.h）。子 agent 角色、
 * 提示词与决策消费方式均属本层策略。
 *
 * 并行性：COG_REVIEW_ROLE_COUNT 个 worker 经 corekern 调度器线程 SSoT
 * （airy_thread_create/join，声明见 corekern/include/task.h）并发执行，
 * 每次补全经机制核注入的闭包独立走 llm_d 调用（机制侧适配器统计已
 * 原子化，并发调用无数据竞争）。线程创建失败时对应 worker 降级为
 * 串行执行（意见不丢失）。
 */

#include "cog_review_strategy.h"
#include "cog_review_internal.h"

#include "airy_llm_ops.h"
#include "airy_memory.h"
#include "logging.h"
#include "platform.h"
#include "string_compat.h"
#include "task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

/* ==================== 认知子 agent 角色定义 ==================== */

typedef struct {
    cog_review_role_t role;
    const char *agent_id;
    const char *system_prompt;
} cog_review_role_def_t;

static const cog_review_role_def_t COG_REVIEW_ROLES[COG_REVIEW_ROLE_COUNT] = {
    { COG_REVIEW_ROLE_INTENT_CONFIRM, "intent-confirmer",
      "You are an independent intent-confirmation agent. Analyze the user's "
      "request independently and respond ONLY in JSON (no prose, no code "
      "fence): {\"intent\":\"task|chat|agent\",\"confidence\":0.0-1.0,"
      "\"reason\":\"one sentence\"}. 'task' means an executable multi-step "
      "task; 'chat' means a conversational reply; 'agent' means managing an "
      "agent." },
    { COG_REVIEW_ROLE_PROBLEM, "problem-reviewer",
      "You are an independent problem-review agent. Review the user's request "
      "for ambiguity, risk and missing information. Respond ONLY in JSON (no "
      "prose, no code fence): {\"clarify_needed\":0|1,\"risks\":[\"...\"],"
      "\"missing\":[\"...\"]}. Keep each item short." },
    { COG_REVIEW_ROLE_BOUNDARY, "boundary-checker",
      "You are an independent boundary-check agent. Review the user's request "
      "for implicit constraints, assumptions, resource limits and edge cases "
      "the planner may violate. Respond ONLY in JSON (no prose, no code "
      "fence): {\"clarify_needed\":0|1,\"risks\":[\"...\"],"
      "\"missing\":[\"...\"]}. Keep each item short." },
    { COG_REVIEW_ROLE_COVERAGE, "coverage-checker",
      "You are an independent coverage-check agent. Review the user's request "
      "for whether its success criteria are fully specified, measurable and "
      "verifiable, and whether any goal aspect is uncovered. Respond ONLY in "
      "JSON (no prose, no code fence): {\"clarify_needed\":0|1,"
      "\"risks\":[\"...\"],\"missing\":[\"...\"]}. Keep each item short." },
};

/* ==================== 并行度探测 ==================== */

/* 按宿主机硬件决定认知子代理并行数（2.5.x 按硬件配置定子代理数）：
 *   AIRY_CPR_PARALLEL 环境变量强制覆盖自动探测值；
 *   rich（cpu>=8 且 mem>=8GB）→ 4、mid（cpu>=4 或 mem>=4GB）→ 2、
 *   弱主机 → 1（串行）；探测失败回退默认 2。策略与 CLI 层
 *   cli_review_parallelism 一致，机制/策略分离。 */
static int cog_review_parallelism(void)
{
    const char *env = getenv("AIRY_CPR_PARALLEL");
    if (env && env[0]) {
        int forced = atoi(env);
        if (forced >= 1 && forced <= COG_REVIEW_MAX_PARALLEL)
            return forced;
    }
    airy_sysinfo_t si;
    if (airy_get_sysinfo(&si) != 0)
        return 2;
    const uint64_t gb = 1024ULL * 1024 * 1024;
    if (si.cpu_count >= 8 && si.memory_total >= 8 * gb)
        return COG_REVIEW_MAX_PARALLEL;
    if (si.cpu_count >= 4 || si.memory_total >= 4 * gb)
        return 2;
    return 1;
}

/* ==================== 并行 worker ==================== */

typedef struct {
    cog_review_role_t role;
    const char *input;
    size_t input_len;
    cog_review_complete_fn complete; /**< 机制核补全闭包（借用） */
    void *complete_ctx;              /**< 闭包上下文（不透明，借用） */
    char *opinion_json; /**< 输出：LLM 原始意见文本（OWNER） */
    int llm_ok;         /**< 输出：是否取得 LLM 意见 */
} cog_review_worker_t;

/**
 * @brief worker 线程体：一次独立 LLM 生成（非流式）。
 *
 * 消息数组、请求配置与响应均为本 worker 私有，无共享可变状态；
 * 机制侧适配器的统计计数已原子化，并发调用无数据竞争。
 */
static void *cog_review_worker_fn(void *arg)
{
    cog_review_worker_t *w = (cog_review_worker_t *)arg;
    const cog_review_role_def_t *def = &COG_REVIEW_ROLES[w->role];

    /* LLM 出口经机制核注入的补全闭包（cog_review_complete_fn）——策略不
     * 直连任何机制符号；llm_ops 只用于按统一契约释放响应。闭包缺席时
     * 该 worker 记为无意见，汇总层据此降级。 */
    const airy_llm_ops_t *llm_ops = are_ops_get_llm();

    llm_message_t msgs[2];
    AIRY_MEMSET(msgs, 0, sizeof(msgs));
    msgs[0].role = "system";
    msgs[0].content = def->system_prompt;
    msgs[1].role = "user";
    msgs[1].content = w->input;

    llm_request_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.model = NULL;
    cfg.messages = msgs;
    cfg.message_count = 2;
    cfg.temperature = 0.3f;
    cfg.top_p = 1.0f;
    cfg.max_tokens = 1024;
    cfg.stream = 0;

    llm_response_t *resp = NULL;
    int ret;
    if (w->complete) {
        ret = w->complete(w->complete_ctx, &cfg, &resp);
    } else {
        ret = -1;
    }

    if (ret == 0 && resp && resp->choices && resp->choice_count > 0 &&
        resp->choices[0].content && resp->choices[0].content[0]) {
        w->opinion_json = AIRY_STRDUP(resp->choices[0].content);
        w->llm_ok = (w->opinion_json != NULL);
    } else {
        AIRY_LOG_WARN("CogReview: role %d LLM failed (ret=%d)", (int)w->role, ret);
    }
    if (resp) {
        if (llm_ops && llm_ops->response_free)
            llm_ops->response_free(resp);
        resp = NULL;
    }
    return NULL;
}

/**
 * @brief 执行单个 worker（线程创建成功则在线程内，失败则串行兜底）。
 */
static void cog_review_launch_worker(cog_review_worker_t *w, airy_thread_t *thr, int *thr_ok)
{
    *thr_ok = 0;
    /* embedded：airy_core 在链时经其 PUBLIC compile definition 继承
     * AIRY_USE_SCHEDULER_THREAD_IMPL，platform_process.h 的平台别名被压制，
     * 调用解析到 corekern 调度线程 SSoT（task.h 声明、scheduler.c 实现），
     * 线程纳入任务表以便优先级与记账生效；standalone：无 airy_core，
     * 经平台别名走平台线程。两形态 join 语义一致。 */
    if (airy_thread_create(thr, cog_review_worker_fn, w) == 0) {
        *thr_ok = 1;
    } else {
        /* 线程创建失败：串行执行，避免意见丢失（稳健而非降级） */
        AIRY_LOG_WARN("CogReview: thread create failed, running role %d inline",
                      (int)w->role);
        (void)cog_review_worker_fn(w);
    }
}

/* ==================== LLM 意见解析与汇总 ==================== */

/**
 * @brief 从 LLM 原始输出中提取 JSON 对象（容忍代码围栏/前后缀文字）。
 *
 * 取第一个 '{' 到最后一个 '}' 的子串；找不到则返回 NULL（视为无意见）。
 * 返回串为堆分配，调用者 AIRY_FREE。
 */
static char *cog_extract_json_object(const char *text)
{
    if (!text)
        return NULL;
    const char *start = strchr(text, '{');
    const char *end = start ? strrchr(text, '}') : NULL;
    if (!start || !end || end <= start)
        return NULL;
    size_t len = (size_t)(end - start) + 1;
    char *out = (char *)AIRY_MALLOC(len + 1);
    if (!out)
        return NULL;
    AIRY_MEMCPY(out, start, len);
    out[len] = '\0';
    return out;
}

/**
 * @brief 意图类别 ID 与标签映射（决策 JSON 中意图为字符串标签）。
 */
static const char *cog_intent_label(int idx)
{
    static const char *const labels[] = { "task", "chat", "agent" };
    if (idx < 0 || idx >= 3)
        return "unknown";
    return labels[idx];
}

static int cog_intent_index(const char *label)
{
    if (!label)
        return -1;
    if (strcmp(label, "task") == 0)
        return 0;
    if (strcmp(label, "chat") == 0)
        return 1;
    if (strcmp(label, "agent") == 0)
        return 2;
    return -1;
}

/**
 * @brief 汇总认知决策（纯函数，可单测）。
 *
 * 输入各子 agent 意见，输出决策 JSON：
 *   - 意图：按意见置信度加权投票（类别得分 = 该类别意见置信度之和），
 *     最高得分类别胜出；决策置信度 = 胜出类别得分 / 总得分（无意见时 0）；
 *   - 问题审查：clarify_needed 任一意见为真即真；risk_count 为累计风险数；
 *   - degraded：全部角色均无 LLM 意见时为 1（不虚构、不阻塞）。
 *
 * 非 static：经 cog_review_internal.h 暴露给测试直测。
 */
airy_err_t cog_review_aggregate(cog_review_result_t *res)
{
#ifdef AIRY_HAS_CJSON
    double intent_score[3] = { 0.0, 0.0, 0.0 };
    double total_score = 0.0;
    int clarify_needed = 0;
    int risk_count = 0;
    int any_llm = 0;

    for (size_t i = 0; i < res->opinion_count; i++) {
        cog_review_opinion_t *op = &res->opinions[i];
        if (!op->opinion_json)
            continue;

        char *obj = cog_extract_json_object(op->opinion_json);
        if (!obj)
            continue;
        cJSON *root = cJSON_Parse(obj);
        AIRY_FREE(obj);
        if (!root)
            continue;

        if (op->role == COG_REVIEW_ROLE_INTENT_CONFIRM) {
            cJSON *intent = cJSON_GetObjectItem(root, "intent");
            cJSON *conf = cJSON_GetObjectItem(root, "confidence");
            if (cJSON_IsString(intent)) {
                int idx = cog_intent_index(intent->valuestring);
                double c = (cJSON_IsNumber(conf) && conf->valuedouble > 0.0 &&
                            conf->valuedouble <= 1.0)
                               ? conf->valuedouble
                               : 0.5;
                if (idx >= 0) {
                    intent_score[idx] += c;
                    total_score += c;
                    any_llm = 1;
                }
            }
        } else {
            /* 问题/边界/覆盖审查共用同一提取契约（clarify_needed + risks +
             * missing）：歧义/风险/缺失信息/边界约束/覆盖缺口统一合并。 */
            cJSON *clar = cJSON_GetObjectItem(root, "clarify_needed");
            if (cJSON_IsNumber(clar) && clar->valueint != 0)
                clarify_needed = 1;
            cJSON *risks = cJSON_GetObjectItem(root, "risks");
            if (cJSON_IsArray(risks)) {
                int n = cJSON_GetArraySize(risks);
                risk_count += (n > 0) ? n : 0;
            }
            any_llm = 1;
        }
        cJSON_Delete(root);
    }

    /* 意图决策：加权投票 */
    const char *intent_label = "unknown";
    double confidence = 0.0;
    double best = 0.0;
    for (int i = 0; i < 3; i++) {
        if (intent_score[i] > best) {
            best = intent_score[i];
            intent_label = cog_intent_label(i);
        }
    }
    if (total_score > 0.0)
        confidence = best / total_score;

    res->degraded = any_llm ? 0 : 1;

    /* 决策 JSON */
    cJSON *dec = cJSON_CreateObject();
    if (!dec)
        return AIRY_ENOMEM;
    cJSON_AddStringToObject(dec, "intent", intent_label);
    cJSON_AddNumberToObject(dec, "confidence", confidence);
    cJSON_AddNumberToObject(dec, "clarify_needed", clarify_needed);
    cJSON_AddNumberToObject(dec, "risk_count", risk_count);
    cJSON_AddNumberToObject(dec, "roles", (double)res->opinion_count);
    cJSON_AddNumberToObject(dec, "degraded", res->degraded);
    char *printed = cJSON_PrintUnformatted(dec);
    cJSON_Delete(dec);
    if (!printed)
        return AIRY_ENOMEM;
    AIRY_FREE(res->decision_json);
    res->decision_json = printed;
    return AIRY_SUCCESS;
#else
    /* 无 cJSON（理论不可达：cJSON 为项目硬依赖）：留空意见决策 */
    res->degraded = 1;
    (void)res;
    return AIRY_SUCCESS;
#endif
}

/* ==================== 公共 API ==================== */

void cog_review_result_init(cog_review_result_t *res)
{
    if (res)
        AIRY_MEMSET(res, 0, sizeof(*res));
}

void cog_review_result_free(cog_review_result_t *res)
{
    if (!res)
        return;
    for (size_t i = 0; i < res->opinion_count; i++) {
        AIRY_FREE(res->opinions[i].opinion_json);
        res->opinions[i].opinion_json = NULL;
    }
    res->opinion_count = 0;
    AIRY_FREE(res->decision_json);
    res->decision_json = NULL;
    res->degraded = 0;
}

airy_err_t cog_review_run(cog_review_complete_fn complete, void *complete_ctx, const char *input,
                          size_t input_len, int max_parallel, cog_review_result_t *out_result)
{
    if (!input || input_len == 0 || !out_result)
        return AIRY_EINVAL;

    cog_review_result_init(out_result);

    /* 无 LLM 可用：不虚构意见，降级决策（不阻塞主链） */
    if (!complete)
        return cog_review_aggregate(out_result);

    /* 并行度：显式指定（裁剪到 [1, MAX_PARALLEL]）或自动硬件探测 */
    int n = COG_REVIEW_MAX_PARALLEL;
    if (max_parallel > 0 && max_parallel < COG_REVIEW_MAX_PARALLEL)
        n = max_parallel;
    else if (max_parallel <= 0)
        n = cog_review_parallelism();
    if (n < 1)
        n = 1;

    cog_review_worker_t workers[COG_REVIEW_ROLE_COUNT];
    airy_thread_t thr[COG_REVIEW_ROLE_COUNT];
    int thr_ok[COG_REVIEW_ROLE_COUNT];
    AIRY_MEMSET(workers, 0, sizeof(workers));
    AIRY_MEMSET(thr, 0, sizeof(thr));
    AIRY_MEMSET(thr_ok, 0, sizeof(thr_ok));

    for (size_t i = 0; i < (size_t)n; i++) {
        workers[i].role = (cog_review_role_t)i;
        workers[i].input = input;
        workers[i].input_len = input_len;
        workers[i].complete = complete;
        workers[i].complete_ctx = complete_ctx;
    }

    /* 并行启动全部 worker（创建失败者串行兜底） */
    for (size_t i = 0; i < (size_t)n; i++)
        cog_review_launch_worker(&workers[i], &thr[i], &thr_ok[i]);

    /* 等待全部完成并收集意见 */
    for (size_t i = 0; i < (size_t)n; i++) {
        if (thr_ok[i])
            (void)airy_platform_thread_join(thr[i], NULL);
        if (workers[i].opinion_json) {
            size_t idx = out_result->opinion_count;
            if (idx < COG_REVIEW_ROLE_COUNT) {
                out_result->opinions[idx].role = workers[i].role;
                AIRY_STRNCPY_TERM(out_result->opinions[idx].agent_id,
                                  COG_REVIEW_ROLES[workers[i].role].agent_id,
                                  sizeof(out_result->opinions[idx].agent_id));
                out_result->opinions[idx].opinion_json = workers[i].opinion_json;
                out_result->opinion_count++;
            } else {
                AIRY_FREE(workers[i].opinion_json);
            }
            workers[i].opinion_json = NULL;
        }
    }

    return cog_review_aggregate(out_result);
}
