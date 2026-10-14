// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gccp.c
 * @brief GCCP 策略载荷：probe/step/confirm 两阶段交互状态机（M5-4 迁出）。
 *
 * 两阶段交互式目标完备性确认：
 *   - probe：推理输入是否需要澄清，产出初始目标与问题集（≤5）；
 *   - step：逐问推进，决策收敛或继续追问；
 *   - confirm：合并用户答案，产出完整目标模型。
 *
 * LLM 驱动为主路径，启发式降级为兜底路径（机制/策略分离）。0.1.19
 * M5-4（§266）：策略从 atoms/coreloopthree 迁出至 products/cognition，
 * 落位"机制在引擎、策略在产品层"的两段式异步通道——机制核经
 * airy_gccp_ops_t 分发并持有交互回调与哨兵协议；策略侧只持
 * airy_gccp_complete_fn 补全闭包，不与机制核符号链接耦合。
 *
 * 协议语义（与 atoms 时代保持一致）：
 *   - probe：简单对话前置门短路；LLM JSON 失败逐级降级启发式；
 *   - step：answered=0 → done=1 收敛；无 LLM → 空 question 交还队列；
 *     绝不回写 next_q（防同题死循环）；
 *   - confirm：confidence≥0.6 → CONFIRMED，否则 AMBIGUOUS。
 */

#include "gccp.h"
#include "gccp_internal.h"
#include "logging.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

airy_err_t gccp_probe(airy_gccp_complete_fn complete, void *complete_ctx, const char *model,
                      const char *input, size_t input_len, airy_gccp_probe_t **out_probe)
{
    if (!input || !out_probe)
        return AIRY_EINVAL;
    *out_probe = NULL;

    if (input_len == 0)
        return AIRY_EINVAL;

    /* 简单对话前置门：跳过 LLM probe 与问题集，直接不交互
     * （need_interaction=0），confirm 走 prefill 快速确认。 */
    if (gccp_is_simple(input, input_len)) {
        airy_gccp_question_t *questions =
            (airy_gccp_question_t *)AIRY_CALLOC(1, sizeof(airy_gccp_question_t));
        airy_gccp_probe_t *probe =
            (airy_gccp_probe_t *)AIRY_CALLOC(1, sizeof(airy_gccp_probe_t));
        if (!probe || !questions) {
            AIRY_FREE(questions);
            AIRY_FREE(probe);
            return AIRY_ENOMEM;
        }
        probe->questions = questions;
        probe->prefill = gccp_prefill(input, input_len);
        if (!probe->prefill) {
            AIRY_FREE(questions);
            AIRY_FREE(probe);
            return AIRY_ENOMEM;
        }
        probe->need_interaction = 0;
        *out_probe = probe;
        AIRY_LOG_INFO("GCCP: simple chat detected, skip interaction (len=%zu)", input_len);
        return AIRY_EOK;
    }

    /* 补全闭包缺失：无 LLM 通道，直接启发式降级（不发起调用） */
    if (!complete) {
        *out_probe = gccp_heur_probe(input, input_len);
        if (!*out_probe)
            return AIRY_ENOMEM;
        AIRY_LOG_INFO("GCCP: probe degraded (no LLM), questions=%zu", (*out_probe)->question_count);
        return AIRY_EOK;
    }

#ifdef AIRY_HAS_CJSON
    static const char *PROBE_PROMPT =
        "You are a goal elicitation assistant. Given the user's task instruction, "
        "reason about whether the goal is complete enough to execute. Produce a JSON "
        "response ONLY, with this schema: "
        "{\"need_interaction\":0|1,\"confidence\":0.0-1.0,"
        "\"prefill\":{\"endpoint\":\"goal end state\",\"start\":\"current state\","
        "\"bottleneck\":\"constraints\",\"audience\":\"stakeholders\","
        "\"verify\":\"verifiable completion criteria\"},"
        "\"questions\":[{\"id\":\"endpoint|start|bottleneck|audience|verify\","
        "\"question\":\"question to ask user in Chinese\","
        "\"hint\":\"answer direction\",\"required\":0|1}]}. "
        "CRITICAL RULES: "
        "1) For greetings, self-introductions, casual chat, opinion/QA questions, and "
        "simple single-step requests (e.g. 'introduce yourself', 'hello', 'what is X'), "
        "you MUST return need_interaction=0 with an EMPTY questions array, filling "
        "prefill directly from the request. "
        "2) need_interaction=1 ONLY for genuinely complex multi-step tasks where the "
        "endpoint, starting state, constraints, or acceptance criteria are unclear "
        "and executing without them would produce a wrong result. "
        "3) When in doubt, prefer need_interaction=0 — the goal is assumed clear. "
        "For each unclear dimension, add one question. Always include the 'verify' "
        "dimension question (verifiable completion criteria) when interaction is "
        "needed. Limit questions to 5, do not repeat ids. Use Chinese for question/hint.";
    char *resp = gccp_llm_call(complete, complete_ctx, model, PROBE_PROMPT, input);
    if (!resp) {
        *out_probe = gccp_heur_probe(input, input_len);
        if (!*out_probe)
            return AIRY_ENOMEM;
        return AIRY_EOK;
    }

    char *json_text = gccp_find_json(resp);
    AIRY_FREE(resp);
    if (!json_text) {
        *out_probe = gccp_heur_probe(input, input_len);
        if (!*out_probe)
            return AIRY_ENOMEM;
        return AIRY_EOK;
    }

    cJSON *root = cJSON_Parse(json_text);
    AIRY_FREE(json_text);
    if (!root) {
        AIRY_LOG_WARN("GCCP: probe LLM JSON parse failed, degrading");
        *out_probe = gccp_heur_probe(input, input_len);
        if (!*out_probe)
            return AIRY_ENOMEM;
        return AIRY_EOK;
    }

    /* questions 预分配 4+1 上限槽位（prompt 硬截 ≤5），避免按 LLM 返回
     * 数量二次分配；CALLOC 保证清零。 */
    airy_gccp_question_t *questions =
        (airy_gccp_question_t *)AIRY_CALLOC(5, sizeof(airy_gccp_question_t));
    airy_gccp_probe_t *probe = (airy_gccp_probe_t *)AIRY_CALLOC(1, sizeof(airy_gccp_probe_t));
    if (!probe || !questions) {
        AIRY_FREE(questions);
        AIRY_FREE(probe);
        cJSON_Delete(root);
        return AIRY_ENOMEM;
    }
    probe->questions = questions;

    cJSON *need = cJSON_GetObjectItemCaseSensitive(root, "need_interaction");
    if (need && cJSON_IsNumber(need))
        probe->need_interaction = (int)need->valuedouble;

    cJSON *prefill = cJSON_GetObjectItemCaseSensitive(root, "prefill");
    if (prefill && cJSON_IsObject(prefill)) {
        probe->prefill = (airy_gccp_goal_t *)AIRY_CALLOC(1, sizeof(airy_gccp_goal_t));
        if (!probe->prefill) {
            AIRY_FREE(questions);
            AIRY_FREE(probe);
            cJSON_Delete(root);
            return AIRY_ENOMEM;
        }
        gccp_apply_json(probe->prefill, prefill);
        if (!probe->prefill->goal_endpoint)
            probe->prefill->goal_endpoint = AIRY_STRDUP(input);
        cJSON *conf = cJSON_GetObjectItemCaseSensitive(root, "confidence");
        if (conf && cJSON_IsNumber(conf))
            probe->prefill->confidence = (float)conf->valuedouble;
        probe->prefill->raw_prompt = AIRY_STRDUP(input);
    } else {
        probe->prefill = gccp_prefill(input, input_len);
    }

    cJSON *qarr = cJSON_GetObjectItemCaseSensitive(root, "questions");
    if (qarr && cJSON_IsArray(qarr)) {
        size_t n = (size_t)cJSON_GetArraySize(qarr);
        /* 数量上限防御（4+1 设计契约）：prompt 已要求 ≤5 问；LLM 失控/
         * 恶意返回更多问题时硬截断到 5，避免无界追问与资源膨胀。 */
        if (n > 5)
            n = 5;
        if (n > 0) {
            probe->question_count = n;
            size_t idx = 0;
            cJSON *q = NULL;
            cJSON_ArrayForEach(q, qarr)
            {
                if (idx >= n)
                    break;
                char *id = gccp_json_field(q, "id");
                if (id) {
                    snprintf(probe->questions[idx].id, sizeof(probe->questions[idx].id), "%s",
                             id);
                    AIRY_FREE(id);
                } else {
                    snprintf(probe->questions[idx].id, sizeof(probe->questions[idx].id), "q%zu",
                             idx + 1);
                }
                char *question = gccp_json_field(q, "question");
                if (question) {
                    snprintf(probe->questions[idx].question,
                             sizeof(probe->questions[idx].question), "%s", question);
                    AIRY_FREE(question);
                }
                char *hint = gccp_json_field(q, "hint");
                if (hint) {
                    snprintf(probe->questions[idx].hint, sizeof(probe->questions[idx].hint), "%s",
                             hint);
                    AIRY_FREE(hint);
                }
                cJSON *req = cJSON_GetObjectItemCaseSensitive(q, "required");
                probe->questions[idx].required = (req && cJSON_IsNumber(req)) ? 1 : 0;
                idx++;
            }
        }
    }

    cJSON_Delete(root);
    *out_probe = probe;
    AIRY_LOG_INFO("GCCP: probe confirmed via LLM (need_interaction=%d, questions=%zu)",
                  probe->need_interaction, probe->question_count);
    return AIRY_EOK;
#else
    (void)complete_ctx;
    (void)model;
    *out_probe = gccp_heur_probe(input, input_len);
    if (!*out_probe)
        return AIRY_ENOMEM;
    return AIRY_EOK;
#endif
}

/**
 * @brief 逐问推进：LLM 对已答内容思考，决定收敛还是继续追问（降级=无追问）。
 *
 * done 判定规则（防 LLM 失控收敛）：
 *   - LLM 明确 done=1 且已完成至少一问 → 收敛；
 *   - 用户跳过上一问（answered=0）→ 不强求，收敛（不再追问）；
 *   - LLM 不可用/解析失败/未给出下一问 → 返回空 question（done=0），
 *     问题队列由调用方持有并推进自己的下一题。
 * 引擎绝不回写 next_q：next_q 是"刚问过的问题"，回写会被调用方当作
 * 针对性追问再次抛给用户，形成同题死循环。
 */
airy_err_t gccp_step(airy_gccp_complete_fn complete, void *complete_ctx, const char *model,
                     const char *input, size_t input_len, const char *answers_json, int answered,
                     const airy_gccp_question_t *next_q, airy_gccp_step_t *out_step)
{
    if (!input || !out_step)
        return AIRY_EINVAL;
    AIRY_MEMSET(out_step, 0, sizeof(*out_step));

    /* 用户已明确跳过上一问：视为意愿不足，直接收敛（机制层不纠缠） */
    if (!answered) {
        out_step->done = 1;
        return AIRY_EOK;
    }

    if (!complete) {
        /* 无 LLM：降级为"无追问"。问题队列由调用方持有，返回空 question
         * 让调用方推进自己的下一题；回写 next_q 会被当作针对性追问再次
         * 抛给用户，造成同题死循环（v0.1.16 现场缺陷根因）。 */
        AIRY_LOG_ERROR("GCCP: step degraded (no llm available)");
        return AIRY_EOK;
    }

#ifdef AIRY_HAS_CJSON
    static const char *STEP_PROMPT =
        "You are an adaptive goal elicitation assistant. The user is answering "
        "questions ONE AT A TIME to converge on a clear, executable goal for a "
        "multi-step task. You see the original instruction, the answers gathered "
        "so far, and the answer to the last question. Think about whether the goal "
        "is now complete enough to execute. "
        "If more clarification is genuinely needed, produce ONE follow-up question "
        "focused on the single most uncertain remaining dimension (do not repeat "
        "already-answered dimensions). If the goal is now clear, set done=1. "
        "Respond with JSON ONLY, schema: "
        "{\"done\":0|1,\"reasoning\":\"brief reasoning about the last answer in Chinese\","
        "\"next\":{\"question\":\"follow-up question in Chinese\","
        "\"hint\":\"answer direction\"}}. "
        "When done=1, next may be omitted.";

    /* 组装 user input：原指令 + 上一问文本 + 已收集答案 + 上一问回答。
     * q8a 修复：STEP_PROMPT 声称 "see the answer to the last question"，
     * 但此前只拼 answers_json（键为 followupN，无问题文本与维度 id），
     * LLM 无法定位答案对应哪个维度，"针对性追问"实质盲猜。现把
     * next_q 的问题文本（含维度 id）一并注入，LLM 可据此判断收敛。 */
    size_t cap = input_len + 128;
    if (answers_json)
        cap += strlen(answers_json) + 64;
    if (next_q && next_q->question[0])
        cap += strlen(next_q->question) + 32;
    char *user_input = (char *)AIRY_MALLOC(cap);
    if (!user_input)
        return AIRY_ENOMEM;
    if (next_q && next_q->question[0])
        snprintf(user_input, cap, "原指令：%.*s\n\n上一问（%s）：%s\n\n已收集答案：%s",
                 (int)(input_len > 4000 ? 4000 : input_len), input,
                 next_q->id[0] ? next_q->id : "followup",
                 next_q->question, answers_json ? answers_json : "（无）");
    else if (answers_json && answers_json[0])
        snprintf(user_input, cap, "原指令：%.*s\n\n已收集答案：%s",
                 (int)(input_len > 4000 ? 4000 : input_len), input, answers_json);
    else
        snprintf(user_input, cap, "原指令：%.*s", (int)(input_len > 4000 ? 4000 : input_len),
                 input);

    char *resp = gccp_llm_call(complete, complete_ctx, model, STEP_PROMPT, user_input);
    AIRY_FREE(user_input);
    if (!resp) {
        AIRY_LOG_ERROR("GCCP: step degraded (llm call failed)");
        return AIRY_EOK;
    }

    char *json_text = gccp_find_json(resp);
    AIRY_FREE(resp);
    if (!json_text) {
        AIRY_LOG_ERROR("GCCP: step degraded (llm output has no json)");
        return AIRY_EOK;
    }

    cJSON *root = cJSON_Parse(json_text);
    AIRY_FREE(json_text);
    if (!root) {
        AIRY_LOG_ERROR("GCCP: step degraded (llm json parse failed)");
        return AIRY_EOK;
    }

    cJSON *done = cJSON_GetObjectItemCaseSensitive(root, "done");
    out_step->done = (done && cJSON_IsNumber(done) && done->valuedouble != 0) ? 1 : 0;

    char *reasoning = gccp_json_field(root, "reasoning");
    if (reasoning) {
        AIRY_STRNCPY_TERM(out_step->reasoning, reasoning, sizeof(out_step->reasoning));
        AIRY_FREE(reasoning);
    }

    cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "next");
    if (next && cJSON_IsObject(next) && !out_step->done) {
        char *q = gccp_json_field(next, "question");
        if (q) {
            AIRY_STRNCPY_TERM(out_step->question, q, sizeof(out_step->question));
            AIRY_FREE(q);
        }
        char *h = gccp_json_field(next, "hint");
        if (h) {
            AIRY_STRNCPY_TERM(out_step->hint, h, sizeof(out_step->hint));
            AIRY_FREE(h);
        }
    }

    /* LLM 未给出下一问（但也没说 done）：视为无追问，队列推进权交还调用方 */
    cJSON_Delete(root);
    AIRY_LOG_INFO("GCCP: step (done=%d, next='%s')", out_step->done, out_step->question);
    return AIRY_EOK;
#else
    (void)complete_ctx;
    (void)model;
    (void)answers_json;
    (void)next_q;
    /* 无 cJSON：同降级语义，队列推进权交还调用方 */
    return AIRY_EOK;
#endif
}

airy_err_t gccp_confirm(airy_gccp_complete_fn complete, void *complete_ctx, const char *model,
                        const char *input, size_t input_len, const char *answers_json,
                        airy_gccp_goal_t **out_goal)
{
    if (!input || !out_goal)
        return AIRY_EINVAL;
    *out_goal = NULL;

    if (input_len == 0)
        return AIRY_EINVAL;

#ifdef AIRY_HAS_CJSON
    if (complete) {
        static const char *CONFIRM_PROMPT =
            "You are a goal completion assistant. Given the user's original task "
            "instruction and their answers to elicitation questions, produce the "
            "complete goal model as JSON ONLY with schema: "
            "{\"endpoint\":\"goal end state\",\"start\":\"current state\","
            "\"bottleneck\":\"constraints/risks\",\"audience\":\"stakeholders\","
            "\"verify\":\"verifiable completion criteria\",\"confidence\":0.0-1.0}. "
            "Infer missing dimensions from the instruction when the answer is absent. "
            "Use Chinese for all values.";

        size_t user_cap = input_len + 64;
        if (answers_json)
            user_cap += strlen(answers_json) + 64;
        char *user_input = (char *)AIRY_MALLOC(user_cap);
        if (!user_input)
            return AIRY_ENOMEM;
        if (answers_json) {
            snprintf(user_input, user_cap, "原指令：%.*s\n\n用户回答：%s",
                     (int)(input_len > 4000 ? 4000 : input_len), input, answers_json);
        } else {
            snprintf(user_input, user_cap, "%.*s", (int)(input_len > 4000 ? 4000 : input_len),
                     input);
        }

        char *resp = gccp_llm_call(complete, complete_ctx, model, CONFIRM_PROMPT, user_input);
        AIRY_FREE(user_input);
        if (resp) {
            char *json_text = gccp_find_json(resp);
            AIRY_FREE(resp);
            if (json_text) {
                cJSON *root = cJSON_Parse(json_text);
                AIRY_FREE(json_text);
                if (root) {
                    airy_gccp_goal_t *goal =
                        (airy_gccp_goal_t *)AIRY_CALLOC(1, sizeof(airy_gccp_goal_t));
                    if (goal) {
                        gccp_apply_json(goal, root);
                        if (!goal->goal_endpoint)
                            goal->goal_endpoint = AIRY_STRDUP(input);
                        goal->raw_prompt = AIRY_STRDUP(input);
                        goal->status = goal->confidence >= AIRY_GCCP_CONFIDENCE_THRESHOLD ?
                                           AIRY_GCCP_STATUS_CONFIRMED :
                                           AIRY_GCCP_STATUS_AMBIGUOUS;
                        cJSON_Delete(root);
                        *out_goal = goal;
                        AIRY_LOG_INFO("GCCP: confirm via LLM (status=%d, conf=%.2f)",
                                      (int)goal->status, goal->confidence);
                        return AIRY_EOK;
                    }
                    cJSON_Delete(root);
                }
            }
        }
        AIRY_LOG_WARN("GCCP: confirm LLM path failed, degrading to heuristic");
    }
#else
    (void)complete;
    (void)complete_ctx;
    (void)model;
    (void)answers_json;
#endif

    *out_goal = gccp_heur_goal(input, input_len);
    if (!*out_goal)
        return AIRY_ENOMEM;
    AIRY_LOG_INFO("GCCP: confirm degraded to heuristic (status=DEGRADED)");
    return AIRY_EOK;
}
