# cognition — 认知策略载荷库

> AgentRT 运行时的认知策略载荷：分发、规划、协调、审查、意图策略——
> 可插拔策略，更换不触碰机制核。

**语言：** [English](README.md) | 简体中文

[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## 概述

**cognition** 是 Airymax AI Agent 运行时平台（`agentrt`）的**策略载荷库**，
承载运行时认知阶段的"策略"半边。依据**机制/策略分离**纲领（0.1.19，
M5-4）自机制核迁出：机制核（`atoms/coreloopthree`）仅保留契约数据类型
与 ops 分发面（`cognition.h`），本库实现具体策略，由 daemon 启动期注入。

机制核不链接本库、不携带默认策略。消费者经本库工厂创建策略实例，通过
`airy_cognition_set_dispatching_strategy()` 或 `airy_cognition_create*_take()`
的 `disp_strategy` 形参注入（TRANSFER 所有权）。认知载荷缺席时，机制核
以内置降级行为保持可用。

## 能力矩阵

| 域 | 策略 | 入口 |
|----|------|------|
| **分发** | 加权 / 轮询 / 优先级 / ML 四类子 agent 分发 | `dispatch_strategy.h` |
| **认知并行审查（CPR）** | 多子 agent 并行审查（认知确认 + 问题/边界/覆盖审查）；各自独立 LLM 调用，意图加权投票 + 风险合并；线程创建失败自动降级串行（意见不丢失） | `cog_review_strategy.h` |
| **GRAD** | 规划生成 + 批判循环（LLM 支撑、分阶段） | `grad_strategy.h` |
| **规划器** | reactive / hierarchical / reflective / ML 四类规划 | `plan_strategy.h` |
| **意图** | 意图分类、实体抽取、规则引擎 | `intent/` |
| **协调** | 双轨协调、仲裁、适配 | `coord_strategy.h` |
| **GCCP** | 候选剪枝启发式评分 | `gccp_strategy.h` |
| **元认知** | 元认知 + 思维链基础件（`foundation/mc`、`foundation/tc`） | `foundation/` |
| **演化** | 认知演化扩展点 | `cognitive_evolution.h` |
| **注册表** | 载荷注册表：策略绑定注入点 | `payload_registry.h` |

所有 LLM 支撑策略在 LLM 服务不可用时自动降级——意见与决策不丢失，
仅质量档位下降。

## 构建

双模式构建契约（单一 `CMakeLists.txt`）：

- **embedded** —— 由 agentrt 根 `add_subdirectory` 纳入时，依赖检测、
  平台宏、合规注入、commons 目标均由根统一提供；embedded 形态字节不变。
- **standalone** —— `cmake -S products/cognition -B <出树目录>`；经
  `AGENTRT_ROOT`（缓存变量 → 环境变量 → 相对默认值）复用 agentrt 管理仓
  的 cmake 模块与 commons 原子件。

构建目标：静态库 `airy_cognition_strategy`。

## 目录结构

```
cognition/
├── CMakeLists.txt              # 双模式构建（embedded | standalone）
├── README.md / README_zh.md    # 英文版 / 本文件（中文版）
├── LICENSE                     # AGPL-3.0-or-later OR Apache-2.0 双许可
├── include/                    # 公共头（策略契约 + 工厂）
│   ├── dispatch_strategy.h     #   分发策略对象契约
│   ├── cog_review_strategy.h   #   认知并行审查（CPR）
│   ├── grad_strategy.h         #   GRAD 规划 + 批判
│   ├── plan_strategy.h         #   规划器策略
│   ├── coord_strategy.h        #   协调 / 仲裁
│   ├── gccp_strategy.h         #   启发式评分
│   ├── cognitive_evolution.h   #   演化扩展
│   └── payload_registry.h      #   载荷注册表
├── src/
│   ├── cog_review.c            # CPR：并行审查 worker + 投票合并
│   ├── coord_*.c               # 协调适配 / 仲裁 / 双轨
│   ├── grad_*.c                # GRAD 规划 / 校验 / LLM 调用
│   ├── weighted.c / round_robin.c / priority.c / ml_*.c   # 分发四件
│   ├── intent/                 # 意图分类、实体抽取、规则引擎
│   ├── planner/                # reactive / hierarchical / reflective / ML
│   ├── foundation/             # 元认知 + 思维链（mc / tc）
│   └── ext/cognitive_evolution.c
└── tests/
    ├── unit/                   # 单元测试（策略、GRAD、线程、e2e）
    └── standalone/             # standalone 模式入口
```

## 并发与安全

CPR worker 经 corekern 调度器线程 SSoT（`airy_thread_create/join`）并发
执行；每次补全经机制核注入的闭包独立调用，适配器统计已原子化——无数据
竞争。公共 API 线程安全（`test_thread_safety*.c` 覆盖）。

## 模块分类

**Class C —— 策略载荷。** 仅通过 `cognition.h` 契约面与 `commons` 原子件
依赖机制核，反向依赖为零。本库可更换、可升级、可摘除，机制核零改动。
