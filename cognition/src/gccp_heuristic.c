// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gccp_heuristic.c
 * @brief GCCP 策略启发式降级域（products/cognition，M5-4 迁出）。
 *
 * 无 LLM / LLM 调用失败 / JSON 解析失败时的确定性降级路径：
 * gccp_prefill()（Q1=全文填充）、gccp_heur_probe()（固定 4+1 问题集）、
 * gccp_is_simple()（简单对话前置门）、gccp_heur_goal()（全文作为最终
 * 目标）。共享声明见 gccp_internal.h。
 *
 * 策略侧不调用机制核释放符号（airy_gccp_probe_free 等）：降级构建的
 * 失败路径一律内联 AIRY_FREE 回滚，保证策略库与机制核零符号耦合。
 */

#include "gccp.h"
#include "gccp_internal.h"
#include "logging.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Heuristic degradation: build an initial goal from the full input (Q1=full input).
 */
airy_gccp_goal_t *gccp_prefill(const char *input, size_t input_len)
{
    airy_gccp_goal_t *goal = (airy_gccp_goal_t *)AIRY_CALLOC(1, sizeof(airy_gccp_goal_t));
    if (!goal)
        return NULL;
    size_t cap = input_len + 1;
    goal->goal_endpoint = (char *)AIRY_MALLOC(cap);
    if (!goal->goal_endpoint) {
        AIRY_FREE(goal);
        return NULL;
    }
    AIRY_MEMCPY(goal->goal_endpoint, input, input_len);
    goal->goal_endpoint[input_len] = '\0';
    goal->confidence = 0.5f;
    goal->status = AIRY_GCCP_STATUS_DEGRADED;
    goal->raw_prompt = AIRY_STRDUP(input);
    return goal;
}

/**
 * @brief Heuristic degradation: fixed five questions (no LLM available).
 *
 * 4 completeness questions (endpoint/start/bottleneck/audience) + 1 result
 * question (verify: verifiable completion criteria), converging the user's
 * true intent per the GCCP "4+1" design.
 *
 * 分配顺序（questions→probe→prefill）：先分配叶子资源再组装容器，任何
 * 一步失败按已持有资源逆序内联回滚，不依赖机制核 probe_free。
 */
airy_gccp_probe_t *gccp_heur_probe(const char *input, size_t input_len)
{
    airy_gccp_question_t *questions =
        (airy_gccp_question_t *)AIRY_CALLOC(5, sizeof(airy_gccp_question_t));
    if (!questions)
        return NULL;

    airy_gccp_probe_t *probe = (airy_gccp_probe_t *)AIRY_CALLOC(1, sizeof(airy_gccp_probe_t));
    if (!probe) {
        AIRY_FREE(questions);
        return NULL;
    }

    probe->prefill = gccp_prefill(input, input_len);
    if (!probe->prefill) {
        AIRY_FREE(questions);
        AIRY_FREE(probe);
        return NULL;
    }

    probe->questions = questions;
    probe->need_interaction = 1;
    probe->question_count = 5;

    const char *ids[] = {"endpoint", "start", "bottleneck", "audience", "verify"};
    const char *qs[] = {"这个任务的最终目标（终点）是什么？完成后应呈现怎样的状态？",
                        "当前起点是什么？哪些条件已经具备，哪些还缺失？",
                        "实现路径上可能有哪些障碍或约束（资源、时间、权限、依赖）？",
                        "谁是最终受益者/验收者？对结果有什么偏好或验收要求？",
                        "任务完成后，你如何确认结果是正确/成功的？验收标准是什么？"};
    const char *hs[] = {"描述目标终态，尽量可验证", "列出现有基础与缺失项", "列出约束、风险与依赖",
                        "说明受众及其验收标准", "给出可验证的完成/验收标准"};

    for (size_t i = 0; i < 5; i++) {
        snprintf(probe->questions[i].id, sizeof(probe->questions[i].id), "%s", ids[i]);
        snprintf(probe->questions[i].question, sizeof(probe->questions[i].question), "%s", qs[i]);
        snprintf(probe->questions[i].hint, sizeof(probe->questions[i].hint), "%s", hs[i]);
        probe->questions[i].required = 1;
    }
    return probe;
}

/**
 * @brief 简单对话/闲聊启发式前置门。
 *
 * GCCP 目标澄清面向复杂多步任务；问候、自我介绍、闲聊、单步短请求若也
 * 触发 4+1 问题轮，用户感知为"对话不可用"。对明显简单输入直接判定
 * 无需交互（need_interaction=0），既不调 LLM（省 token）也不产生问题集。
 *
 * 判定规则（保守：宁可少交互，不误伤真实任务）：
 *   1. 含明确任务动词（删除/创建/安装/实现/部署…）+ 宾语 → 真实任务，
 *      即使短也走完整 GCCP（先于长度门判断，避免"删除 a.txt"等短真实
 *      任务被误判为简单对话）；
 *   2. 命中问候/闲聊/自我介绍模式 → 简单；
 *   3. 极短输入（≤16 字节 ≈ 5 个汉字）且未命中上述 → 简单。
 *
 * @return 1=简单对话（跳过交互）；0=需要完整 GCCP probe
 */
int gccp_is_simple(const char *input, size_t input_len)
{
    if (!input || input_len == 0)
        return 1;

    static const char *const task_verbs[] = {
        "删除", "创建", "修改", "更新", "安装", "卸载", "运行", "启动", "停止",
        "查找", "搜索", "打开", "编写", "开发", "部署", "迁移", "重构", "集成",
        "测试", "实现", "修复", "构建", "编译", "发布", "配置", "排查", "优化",
        "设计", "分析", "统计", "汇总", "生成", "整理", "转换", "打包", "下载",
        "上传", "备份", "恢复", "清理",
        "delete", "create", "install", "uninstall", "run", "start", "stop",
        "find", "search", "open", "write", "develop", "deploy", "migrate",
        "refactor", "integrate", "test", "implement", "fix", "build",
        "compile", "release", "config", "debug", "optimize", "design",
        "analyze", "generate", "convert", "download", "upload", "backup",
        NULL};
    for (int i = 0; task_verbs[i]; i++) {
        if (strstr(input, task_verbs[i]))
            return 0; /* 真实任务动词 → 不跳过（走完整 GCCP） */
    }

    static const char *const chat_patterns[] = {
        "你好", "您好", "嗨", "hello", "hi ", "早上好", "下午好", "晚上好",
        "谢谢", "再见", "你是谁", "你叫什么", "介绍一下你自己", "介绍下你自己",
        "自我介绍", "你能做什么", "你可以做什么", "who are you", "what can you do",
        "今天", "天气", "在吗", NULL};
    for (int i = 0; chat_patterns[i]; i++) {
        if (strstr(input, chat_patterns[i]))
            return 1;
    }
    /* 极短输入（≤16 字节）且未命中任务动词/聊天模式 → 视为简单对话 */
    if (input_len <= 16)
        return 1;
    return 0;
}

/**
 * @brief Heuristic confirmation: no LLM/parse failure → full input as the final goal.
 */
airy_gccp_goal_t *gccp_heur_goal(const char *input, size_t input_len)
{
    return gccp_prefill(input, input_len);
}
