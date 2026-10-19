# gateway_biz — 网关业务处理面（嵌入式产品件）

> `gateway_d` 的业务处理面，以产品件形态寄宿于机制核之外。承载统一能力面
> 派发、命名空间转发与 PEP 裁定缓存——在构建期装配进 `gateway_d`，不被任何
> 其它目标链接。

**语言:** [English](README.md) | 简体中文

[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## 概述

**gateway_biz** 是网关 daemon 的**业务处理面**。它在 0.1.19 体积工程
（台账 §340「刀H」）中自机制核 `agentrt/gateway/src/biz/` 迁出：其处理器依赖
daemon 公共层（`svc_common`、`daemon_security`）与 gateway 协议适配头，不属于
纯 gateway 机制，也不应计入机制核体积账。

它是**嵌入式产品件**，而非独立库：

- 唯一消费者是 `gateway_d` 可执行目标（见 `agentrt/link-whitelist.txt`）；
- 配置前要求宿主目标（`gateway_policy_gen`、`svc_common`、`airy_protocols`）
  已存在；
- 宿主探测不到本目录时，`gateway_d` 整体不挂载，且不生成任何桩实现——缺席
  以「丢弃该 daemon」降级，绝不以伪造实现降级。

机制核保留传输、分帧、会话与协议机制；本件保留消费这些机制的**业务路由**。

## 能力

- **统一能力派发。** 网关对外暴露的每个能力在 `agentrt/gateway/.manifest` 中
  唯一声明，构建期生成只读注册表作为唯一权威源（`cap_key = "<ns>.<method>"`）。
  未登记能力 fail-closed（`-32601`），网关只暴露 manifest 声明的能力。
- **命名空间转发。** `<ns>.<method>` 经 L2 协议客户端转发至归属 daemon；wire
  方法名与超时取自注册表，不再对请求字符串做截取推导。
- **协议后端。** 面向外部客户端提供 MCP、OpenAI 兼容与 A2A 的请求/响应适配。
- **Agent 转发。** `agent.run` / `agent.cancel` 转发至 `agent_d`；编排引擎位于
  daemon 内，不在本件。
- **Hall 面。** `hall.*` 任务看板、事件流与决策链在网关进程内实现。
- **PEP 裁定缓存。** 裁定结果按 epoch 键缓存，策略变更时整体失效，安全热路径
  命中零分配。
- **统一服务派发钩子。** `SYS_SVC_CALL` 经微核心钩子路由，服务调用无法绕过
  syscall 边界。
- **按需激活。** `supervisor.activate` 以单向 fire-and-forget 通知投递。
- **内置 MCP 工具。** 工具 JSON schema 由单一权威表注册。

## 构建

本件**仅支持嵌入式装配**。由 `gateway_d` 纳入，继承宿主工具链、平台宏与合规
注入：

```
# 由 agentrt/daemons/gateway_d/CMakeLists.txt 配置
AIRY_GATEWAY_BIZ_AGENTRT_ROOT   # 必填：agentrt 源码根
AIRY_GATEWAY_BIZ_GEN_DIR        # 必填：gateway_policy_gen 产物目录
```

两个变量任一缺失、或必需宿主目标不存在时，配置期大声失败。本件不支持孤立
构建：它没有独立依赖图，强行孤立构建只能靠桩宿主目标，这正是项目禁止的做法。

## 目录结构

```
gateway_biz/
├── include/
│   ├── gateway_business_handler.h    # 公共入口：派发 + 上下文生命周期
│   ├── gateway_biz_internal.h        # 模块私有共享契约
│   ├── gateway_cap_registry.h        # 能力注册表查询面（SSoT）
│   ├── gateway_pep_cache.h           # PEP 裁定缓存
│   ├── gateway_policy.h              # 处理器消费的策略面
│   └── gateway_biz_supactivate.h     # 按需激活通知
├── src/
│   ├── gateway_business_handler.c    # 主派发 + 上下文生命周期
│   ├── gateway_biz_forward.c         # 命名空间转发（L2 客户端 + 白名单）
│   ├── gateway_biz_agent.c           # agent.run / agent.cancel 转发
│   ├── gateway_biz_backend.c         # MCP / OpenAI / A2A 协议后端
│   ├── gateway_biz_hall.c            # hall.* 任务看板 / 事件流 / 决策链
│   ├── gateway_biz_svcdispatch.c     # 统一服务派发钩子
│   ├── gateway_biz_supactivate.c     # 单向激活通知
│   ├── gateway_biz_tools.c           # 内置 MCP 工具注册
│   ├── gateway_cap_registry.c        # cap_key 查找 / 命名空间校验
│   └── gateway_pep_cache.c           # PEP 裁定缓存（epoch 失效）
├── CMakeLists.txt
└── LICENSE
```

## 测试

本件为嵌入式产品件，测试随能够提供真实宿主的消费方存放：
`agentrt/daemons/gateway_d/tests/`。

- `test_gateway_cap_registry`：编译 `src/gateway_cap_registry.c`，对接生成的
  manifest 表，验证查找、命名空间独占、超时与契约版本语义。
- `test_gateway_pep_cache`：编译 `src/gateway_pep_cache.c`，验证命中/未命中、
  epoch 失效与并发。
- `test_svcdispatch_e2e`：端到端验证 `SYS_SVC_CALL -> 钩子 -> gw_svc_call ->
  mock daemon`。
- `bench_gateway_pep`：测量 PEP 热路径。

此处刻意不设 `tests/` 目录：本地测试夹具必须伪造宿主，而这正是项目禁止的
桩行为。

## 模块分类

| 维度 | 取值 |
|------|------|
| 层次 | 形态面（业务路由），位于机制核之外 |
| 聚合形态 | 由 `products` 仓直接追踪 |
| 消费者 | 仅 `gateway_d`（链接白名单） |
| 内核回退 | 本件缺席时不挂载该 daemon |
| 许可证 | AGPL v3 + Apache 2.0 |
