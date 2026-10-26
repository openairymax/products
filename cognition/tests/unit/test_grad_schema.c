// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_grad_schema.c
 *   GRAD 计划 schema v1 校验测试（0.1.20 C1）
 *
 * 验证目标：
 *   1. 合法计划（全字段/缺省可补元数据）通过；
 *   2. plan_id 模式与缺失整份拒绝；
 *   3. 节点字段域（id/role/goal/cost/timeout/priority/guard/origin）；
 *   4. 结构层（id/outputs 唯一、depends 存在与无自环、entry 引用）；
 *   5. inputs 三态（lit: 豁免 / producer 闭包 / 格式与存在性）。
 */

#include "test_grad_internal.h"

#ifdef AIRY_HAS_CJSON

#include <cjson/cJSON.h>

/* 校验辅助：解析并校验，返回 AIRY_SUCCESS/EINVAL/ESERVICE（释放 root） */
static airy_err_t schema_try(const char *json, const char *plan_id, grad_schema_err_t *err)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return AIRY_ESERVICE;
    airy_err_t rc = grad_plan_schema_validate(root, plan_id, err);
    cJSON_Delete(root);
    return rc;
}

/* 全字段合法两节点 DAG（含 entry/闭包内 inputs/双维 cost） */
void test_schema_valid_full(void)
{
    const char *j =
        "{\"nodes\":["
        "{\"id\":\"S_01\",\"goal\":\"collect source\",\"role\":\"retrieve\","
        "\"inputs\":[\"lit:seed-query\"],\"outputs\":[\"src_doc\"],"
        "\"cost_time_ms\":3000,\"cost_mem_mb\":128,"
        "\"timeout_ms\":60000,\"priority\":10,\"invariant_guard\":true},"
        "{\"id\":\"S_02\",\"goal\":\"write report\",\"role\":\"act\","
        "\"depends\":[\"S_01\"],\"inputs\":[\"S_01.src_doc\"],"
        "\"outputs\":[\"report\"],\"cost_time_ms\":5000,\"cost_mem_mb\":64,"
        "\"plan_origin\":\"generated\"}"
        "],\"entry\":[\"S_01\"]}";
    grad_schema_err_t err;
    TEST_CHECK(schema_try(j, "plan-1.2", &err) == AIRY_SUCCESS, "valid full plan accepted");
    TEST_PASS("schema full-field plan accepted");
}

/* outputs/inputs/cost 可缺省（C4 推导补全域），depends null 视为缺省 */
void test_schema_optional_metadata(void)
{
    const char *j =
        "{\"nodes\":["
        "{\"id\":\"A\",\"goal\":\"do a\",\"role\":\"plan\",\"depends\":null},"
        "{\"id\":\"B\",\"goal\":\"do b\",\"role\":\"verify\",\"depends\":[\"A\"]}"
        "]}";
    grad_schema_err_t err;
    TEST_CHECK(schema_try(j, "grad", &err) == AIRY_SUCCESS,
               "metadata-optional plan accepted");
    TEST_PASS("metadata-optional plan accepted");
}

/* plan_id：缺失/空/非法字符/超长 整份拒绝 */
void test_schema_plan_id(void)
{
    const char *j = "{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\"}]}";
    grad_schema_err_t err;
    TEST_CHECK(schema_try(j, NULL, &err) == AIRY_EINVAL && err.index == -1 &&
                   strcmp(err.field, "plan_id") == 0 && strcmp(err.reason, "missing") == 0,
               "NULL plan_id rejected as missing");
    TEST_CHECK(schema_try(j, "", &err) == AIRY_EINVAL, "empty plan_id rejected");
    TEST_CHECK(schema_try(j, "bad id!", &err) == AIRY_EINVAL, "bad pattern plan_id rejected");
    TEST_CHECK(schema_try(j,
                          "p0123456789012345678901234567890123456789012345678901234567890123",
                          &err) == AIRY_EINVAL,
               "oversize plan_id rejected");
    TEST_PASS("plan_id domain enforced");
}

/* 节点字段域违例逐项拒绝（role 非法枚举/cost 负值/guard 类型/origin 枚举） */
void test_schema_node_fields(void)
{
    grad_schema_err_t err;
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\","
                          "\"role\":\"creator\"}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.field, "role") == 0,
               "illegal role rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\","
                          "\"role\":\"plan\",\"cost_time_ms\":-5}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.field, "cost_time_ms") == 0,
               "negative cost rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"cost_mem_mb\":1000001}]}",
                          "p", &err) == AIRY_EINVAL,
               "over-max cost rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"invariant_guard\":\"yes\"}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.field, "invariant_guard") == 0,
               "non-bool guard rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"timeout_ms\":600001}]}",
                          "p", &err) == AIRY_EINVAL,
               "over-max timeout rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"priority\":256}]}",
                          "p", &err) == AIRY_EINVAL,
               "over-max priority rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"plan_origin\":\"magic\"}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.field, "plan_origin") == 0,
               "illegal origin rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"a b\",\"goal\":\"g\",\"role\":\"plan\"}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.field, "id") == 0,
               "illegal id pattern rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"\",\"role\":\"plan\"}]}",
                          "p", &err) == AIRY_EINVAL,
               "empty goal rejected");
    TEST_PASS("node field domains enforced");
}

/* 结构层：nodes 边界 / id dup / outputs dup / depends 悬空与自环 / entry 引用 */
void test_schema_structure(void)
{
    grad_schema_err_t err;
    TEST_CHECK(schema_try("{\"nodes\":[]}", "p", &err) == AIRY_EINVAL,
               "empty nodes rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"depends\":[\"X\"]}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.reason, "missing_dep") == 0,
               "dangling depends rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"depends\":[\"A\"]}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.reason, "self_dep") == 0,
               "self depends rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\"},"
                          "{\"id\":\"A\",\"goal\":\"h\",\"role\":\"act\"}]}",
                          "p", &err) == AIRY_EINVAL && err.index == 1 &&
                   strcmp(err.reason, "dup") == 0,
               "duplicate id rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"outputs\":[\"o\"]},"
                          "{\"id\":\"B\",\"goal\":\"h\",\"role\":\"act\","
                          "\"outputs\":[\"o\"]}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.field, "outputs") == 0,
               "duplicate output rejected");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\"}],"
                          "\"entry\":[\"Z\"]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.reason, "missing_ref") == 0,
               "dangling entry rejected");
    TEST_PASS("structure constraints enforced");
}

/* inputs：producer 存在性 / 闭包归属 / lit: 豁免 / 传递闭包间接引用 / 格式 */
void test_schema_inputs(void)
{
    grad_schema_err_t err;
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"outputs\":[\"o\"],"
                          "\"inputs\":[\"X.o\"]}]}",
                          "p", &err) == AIRY_EINVAL &&
                   strcmp(err.reason, "producer_missing") == 0,
               "missing producer rejected");
    /* B 依赖 A，P 与 A/B 无依赖边：引用 P 的输出越闭包 → 拒绝 */
    TEST_CHECK(schema_try("{\"nodes\":["
                          "{\"id\":\"P\",\"goal\":\"p\",\"role\":\"plan\",\"outputs\":[\"po\"]},"
                          "{\"id\":\"A\",\"goal\":\"a\",\"role\":\"retrieve\","
                          "\"outputs\":[\"ao\"]},"
                          "{\"id\":\"B\",\"goal\":\"b\",\"role\":\"act\",\"depends\":[\"A\"],"
                          "\"inputs\":[\"P.po\"]}]}",
                          "p", &err) == AIRY_EINVAL &&
                   strcmp(err.reason, "producer_not_in_depends") == 0,
               "out-of-closure producer rejected");
    /* 传递闭包间接引用（B→A→P，B 只声明依赖 A 而引 P 输出）合法 */
    TEST_CHECK(schema_try("{\"nodes\":["
                          "{\"id\":\"P\",\"goal\":\"p\",\"role\":\"plan\",\"outputs\":[\"po\"]},"
                          "{\"id\":\"A\",\"goal\":\"a\",\"role\":\"retrieve\",\"depends\":[\"P\"],"
                          "\"outputs\":[\"ao\"]},"
                          "{\"id\":\"B\",\"goal\":\"b\",\"role\":\"act\",\"depends\":[\"A\"],"
                          "\"inputs\":[\"P.po\"]}]}",
                          "p", &err) == AIRY_SUCCESS,
               "transitive-closure producer accepted");
    /* lit: 豁免 producer 检查；bad format（无 '.'）拒绝 */
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"inputs\":[\"lit:hello\"]}]}",
                          "p", &err) == AIRY_SUCCESS,
               "lit: input exempt from closure");
    TEST_CHECK(schema_try("{\"nodes\":[{\"id\":\"A\",\"goal\":\"g\",\"role\":\"plan\","
                          "\"inputs\":[\"nodot\"]}]}",
                          "p", &err) == AIRY_EINVAL && strcmp(err.reason, "format") == 0,
               "input without dot rejected");
    TEST_PASS("inputs closure semantics enforced");
}

#else /* !AIRY_HAS_CJSON */

void test_schema_valid_full(void)
{
    TEST_CHECK(1, "schema tests require cJSON (skipped)");
}
void test_schema_optional_metadata(void) {}
void test_schema_plan_id(void) {}
void test_schema_node_fields(void) {}
void test_schema_structure(void) {}
void test_schema_inputs(void) {}

#endif /* AIRY_HAS_CJSON */
