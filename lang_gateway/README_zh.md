# lang_gateway — 推理语言网关

> 用户自然语言不直接进模型：先标准化为内部标准数据格式（Canonical
> Data Format），再路由——AgentRT 运行时的推理语言全生命周期管理器。

**语言：** [English](README.md) | 简体中文

[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## 概述

**lang_gateway** 是 Airymax AI Agent 运行时平台（`agentrt`）的生态层
**策略载荷**，依据**机制/策略分离**纲领（0.1.19，M5-2）自机制核迁出：
数据类型契约与 ops 注入表（`airy_lang_gw_ops.h`）归机制核，本库实现
生命周期各阶段。机制核消费者经 `are_ops_get_lang_gw()` 分发，不直接
包含本库头文件。

## 生命周期：三阶段

```
输入 ──▶ Phase 1 ──▶ 模型通道 ──▶ Phase 3 ──▶ 输出
              │             ▲
              └── Phase 2 ──┘
```

| 阶段 | 部件 | 职责 |
|------|------|------|
| **1 — 决策前** | `signal_extractor` | 提取语言 / 任务 / 上下文 / 代码含量信号 |
| | `lang_router` | 多因子路由决策：硬规则 → 模型原生能力 → 输入语言对齐 → 性价比阈值 |
| **2 — 推理中** | `system_prompt` | 注入语言约束 System Prompt；输入转换（占位符保护 / 历史重写） |
| **3 — 输出后** | `output_post_processor` | 语言漂移检测、术语一致性、意译润色 |

用户输入先标准化为**标准数据格式**（`canonical.h`）再进入模型通道。

## 模型画像与校准

模型画像（`ModelProfile`）由 Tokenizer 特征校准模块（`calibrator`）产出：

- 校准经**后台 worker** 异步执行——首轮请求永不阻塞。
- 每 N 次指令（默认 100）自动重校准 `lang_ratio`（中文 token /
  英文 token）。
- 校准经 llm_d 的 `count_tokens` RPC 完成；llm_d 不可用时网关**降级为
  启发式决策**——主流程永不受影响。

## 构建

构建目标：静态库 `airy_lang_gateway`（单一 `CMakeLists.txt`，
遵循 products 双模式构建契约）。

## 目录结构

```
lang_gateway/
├── CMakeLists.txt                # 构建配置（静态库 airy_lang_gateway）
├── README.md / README_zh.md      # 英文版 / 本文件（中文版）
├── LICENSE                       # AGPL-3.0-or-later OR Apache-2.0 双许可
├── include/
│   └── lang_gateway.h            # 公共 API（库直接 API；机制核消费者
│                                 #   经 are_ops_get_lang_gw() 分发）
├── src/
│   ├── lang_gateway.c            # 主控制器：三阶段编排 + 生命周期 + 统计
│   ├── signal_extractor.c        # Phase 1：信号提取
│   ├── lang_router.c             # Phase 1：多因子路由
│   ├── system_prompt.c           # Phase 2：语言约束注入
│   ├── output_post_processor.c   # Phase 3：漂移 / 术语 / 润色
│   ├── calibrator.c              # ModelProfile Tokenizer 校准
│   └── canonical.h               # 标准数据格式
└── tests/unit/
    └── test_lang_gateway.c
```

## 设计要点

- **按阶段模块化**：各阶段独立源文件；策略参数可配置调整——策略升级
  不改公共 API。
- **启发式兜底**：对 llm_d 的每处依赖均有启发式回退；零后端支撑时
  网关依然可用。
- **契约边界**：`airy_lang_gw_ops.h`（机制核）是唯一耦合点；本库可
  整体更换，机制核零改动。
