# Airymax 产品层 — 策略模块聚合

> Airymax AI 智能体运行时平台的**策略模块聚合仓**。将平台中承载「策略」的模块
> —— 商业 MemoryRovol 记忆提供者、`cupolas` 安全穹顶、推理语言网关 —— 汇集于
> 统一的管理面之下，使策略载荷与机制核保持物理分离。

**语言:** [English](README.md) | 简体中文

[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## 概述

**Products 管理仓**（`products/`）是 Airymax 平台的**策略模块聚合路径**。它聚合
承载*策略*的模块 —— 由 AgentRT 机制核加载或分派的可替换、策略形态的载荷 —— 并
使其与机制核保持物理分离：

- **memoryrovol** —— 商业 L3/L4 记忆提供者（闭源）。
- **cupolas** —— 安全穹顶：沙箱隔离、RBAC/ABAC 权限裁决、输入净化、审计追踪，
  以及动态策略引擎（策略决策点 PDP）。
- **lang_gateway** —— 推理语言网关：推理语言全生命周期管理（信号提取、多因子
  模型路由、System Prompt 语言约束、输出后处理）。

本仓遵循平台的**机制 / 策略分离**原则：机制核位于
[`agentrt/`](../agentrt)（atoms · commons · daemons · gateway · heapstore ·
protocols · tools），而策略形态的模块位于本仓，并通过稳定契约（`ops` 分发表、
弱符号桥、submodule 固定）接入。策略模块可被替换、升级或移除而不触碰内核；内核
保留内置回退，在策略模块缺席时仍保持可用。

与 [`sdk/`](../sdk) —— 纯叶子聚合 —— 不同，`products/` 有意混合两种聚合形态：

- **submodule 叶子**（`memoryrovol`、`cupolas`）—— 各自是独立仓库，固定到精确
  commit。
- **直接追踪模块**（`lang_gateway`）—— 源码由本仓自身管理的模块，暂无独立叶子仓。

## 仓库结构

```
products/                       # 本管理仓（AGPL v3 + Apache 2.0）
├── memoryrovol/                # git submodule → openairymax/memoryrovol  (闭源)
│                               #   商业 L3/L4 记忆提供者（SPHARX EULA）
├── cupolas/                    # git submodule → openairymax/cupolas
│                               #   安全穹顶 + 动态策略引擎（PDP）
├── lang_gateway/               # 直接追踪的策略模块（暂无独立叶子仓）
│                               #   推理语言网关（厂商面）
├── .gitmodules                 # submodule 配置（2 个条目）
├── .gitignore                  # 白名单：文件 + submodule 挂点
├── LICENSE                     # AGPL v3 + Apache 2.0（仅限管理仓本身）
├── NOTICE                      # 版权声明、商标与第三方组件说明
├── README.md                   # 英文版
└── README_zh.md                # 简体中文版（本文件）
```

## 策略模块

| 模块 | 形态 | 位置 | 许可证 | 说明 |
|------|------|------|--------|------|
| **memoryrovol** | submodule | `memoryrovol/` | SPHARX EULA v1.0（专有） | 商业闭源记忆提供者，实现 L3（结构层）与 L4（模式层）记忆。 |
| **cupolas** | submodule | `cupolas/` | AGPL v3 + Apache 2.0 | 应用语义安全层：四层内生安全 + 动态策略引擎（策略决策点 PDP）。 |
| **lang_gateway** | 直接追踪 | `lang_gateway/` | AGPL v3 + Apache 2.0 | 推理语言网关（`airy_lang_gateway`）—— 将自然语言输入标准化并路由至合适的模型。 |

> **许可证说明。** `cupolas` 与 `lang_gateway` 采用与 Airymax 平台其余部分一致的
> AGPL v3 + Apache 2.0 双许可证（SPDX：`AGPL-3.0-or-later OR Apache-2.0`）。
> `memoryrovol` **不是**开源软件：受 SPHARX 商业 EULA v1.0 约束
> （SPDX：`LicenseRef-SPHARX-MemoryRovol-EULA-1.0`），其使用需要与授权层级
> 绑定的 License Key。

## 策略架构

策略模块位于机制核**之上**并依赖机制核 —— 而非相反。内核暴露稳定的分派契约；
每个策略模块在契约之后实现其一，且每个模块都有内核侧回退，使得模块缺席时降级而
非崩溃。

```
                  ┌──────────────────────────────────────────────┐
                  │  机制核（agentrt/）                            │
                  │  atoms · commons · daemons · gateway          │
                  │  heapstore · protocols · tools                │
                  └───────────────────┬──────────────────────────┘
                                      │  稳定契约：
                                      │  ops 分派 · 弱符号 · submodule 固定
        ┌─────────────────────────────┼─────────────────────────────┐
        │                             │                             │
        ▼                             ▼                             ▼
┌──────────────────┐       ┌──────────────────┐       ┌──────────────────────┐
│   memoryrovol    │       │     cupolas      │       │    lang_gateway      │
│ L3 结构层        │       │ 沙箱 / RBAC      │       │ 推理语言             │
│ L4 模式层        │       │ 净化 / 审计      │       │ 标准化 + 路由        │
│ (闭源，          │       │ + 策略引擎       │       │ (ops 分派，          │
│  SPHARX EULA)    │       │ (PDP)            │       │  内核回退)           │
└────────┬─────────┘       └────────┬─────────┘       └──────────┬───────────┘
         │                          │                            │
         ▼                          ▼                            ▼
   AgentRT 运行时             gateway / daemons             CoreLoopThree
   (AIRY_WITH_MEMORYROVOL,   (在每个安全边界调用            (策略存在时经
    弱符号桥)                  cupolas)                      are_ops_get_lang_gw)
```

### memoryrovol —— 商业记忆提供者

MemoryRovol 以预编译、许可证门控的 C 静态库（`.a` / `.lib`）形式实现 Airymax
四层记忆架构中的 **L3（结构层）** 与 **L4（模式层）**，在构建期链接进 AgentRT
运行时。运行时加载时，CoreLoopThree 桥通过 **弱符号（weak symbol）** 契约解析
MemoryRovol：若链接了 MemoryRovol，桥将记忆调用路由给它；若未链接，桥透明回退
到运行时内置的开源提供者（仅覆盖 L1 与 L2）。

- **上游**：`atoms/memory/memoryrovol/` 桥 API（弱符号）。
- **下游**：AgentRT 运行时（`AIRY_WITH_MEMORYROVOL` CMake 选项，默认开启；
  后端通过 `AIRY_MEMORY_BACKEND=builtin|memoryrovol` 选择）。
- **许可证**：SPHARX 商业 EULA v1.0 —— 详见
  [§ MemoryRovol 许可证](#memoryrovol-许可证)。

### cupolas —— 安全穹顶与策略引擎

cupolas 是**应用语义安全层**：每个智能体动作在抵达内核前都必须经过该穹顶。它
实现四层内生安全模型 —— 沙箱隔离、RBAC/ABAC 权限裁决、输入净化、审计追踪 —— 并
构建单一静态库 `airy_cupolas`。在四层之上，它充当**策略决策点（PDP）**：动态
策略引擎负责加载、校验、冲突检测、激活与版本回滚（JSON 规则集，可按版本回滚），
随后分发到各 daemon，由每个 daemon 的**策略执行点（PEP）**通过本地缓存就地执行。

- **上游**：`commons`（同步、错误框架、类型、内存宏）；沙箱所依赖的 Linux 内核
  原语（Landlock、seccomp BPF）。
- **下游**：`gateway` 与 `daemons` —— 在每个安全边界（请求鉴权、输入净化、权限
  检查、审计发射）被调用。
- **许可证**：AGPL v3 + Apache 2.0。

### lang_gateway —— 推理语言网关

`lang_gateway` 管理推理语言的全生命周期。用户输入不直接进入模型：网关先将其
标准化为内部标准数据格式（Canonical Data Format），再路由给合适的模型，贯穿
三个阶段 —— Phase 1（决策前：信号提取 + 多因子路由）、Phase 2（推理中：System
Prompt 语言约束 + 输入转换）、Phase 3（输出后：语言漂移检测 + 术语一致性 +
意译润色）。逐模型画像（`lang_ratio = zh_tokens / en_tokens`）经 `llm_d` 的
`count_tokens` RPC 异步校准；`llm_d` 不可用时网关降级为启发式路由，不阻断主路径。

- **契约**：数据类型契约与 ops 注入表位于机制核（`airy_lang_gw_ops.h`）；内核
  消费者经 `are_ops_get_lang_gw()` 分派，不直接依赖本模块头文件。
- **上游**：`airy_rt.h` 与内核侧 `airy_lang_gw_ops.h` 契约。
- **下游**：策略存在时的 CoreLoopThree（机制核）；策略缺席时内核优雅降级。
- **许可证**：AGPL v3 + Apache 2.0。

### 记忆分层

```
L1 原始层       ← 内置（开源，AGPL v3 + Apache 2.0）
L2 特征层       ← 内置（开源，AGPL v3 + Apache 2.0）
L3 结构层       ← MemoryRovol（商业，SPHARX EULA）
L4 模式层       ← MemoryRovol（商业，SPHARX EULA）
```

## MemoryRovol 许可证

`memoryrovol` 是整个 Airymax 平台中唯一的闭源组件。它受 **SPHARX 商业
EULA v1.0**（SPDX：`LicenseRef-SPHARX-MemoryRovol-EULA-1.0`）约束，由
**SPHARX Ltd.** 出品并所有。仓库托管在 AtomGit 的
[openairymax](https://atomgit.com/openairymax) 组织下，用于分发与问题跟踪；
托管位置不改变其专有许可属性。

### 授权层级

使用 MemoryRovol 需要绑定以下四个授权层级之一的 **License Key**。层级决定功能
范围、容量上限与支持级别：

| 层级 | 受众 | 典型范围 |
|------|------|----------|
| **Trial** | 评估者、个人开发者 | 时长 / 容量受限的 L3 + L4 功能评估，用于集成测试。 |
| **Pro** | 专业用户、小团队 | 完整的 L3（结构层）与 L4（模式层）能力，用于个人或小团队生产环境。 |
| **Enterprise** | 中大型组织 | 更高的容量上限、多席位部署、优先商业支持。 |
| **Enterprise+** | 大型企业 / OEM | 最高容量、OEM 再分发权利、专属支持与 SLA。 |

层级强制执行由 MemoryRovol 库内置的 **RSA-4096 离线 License Key 校验**机制完成
—— 不外联、不上报遥测，完整支持气隙运行。完整条款见 `memoryrovol` submodule
内的 [`LICENSE`](memoryrovol/LICENSE) 文件；外层管理仓的开源 AGPL v3 +
Apache 2.0 许可证**不适用**于该 submodule。

## 构建与集成

### 前置条件

- Git ≥ 2.30（支持 submodule）。
- `memoryrovol`（仅在链接进 AgentRT 时）：CMake 3.20+、C11 编译器、有效的
  MemoryRovol License Key。
- `cupolas` 与 `lang_gateway`：C11 编译器；两者作为 CMake 目标
  （`airy_cupolas`、`airy_lang_gateway`）被 AgentRT 构建消费。

### 带 submodule 克隆

```bash
# 克隆管理仓及其叶子 submodule
git clone --recurse-submodules https://atomgit.com/openairymax/products.git

# 若已克隆但未带 --recurse-submodules：
cd products
git submodule update --init --recursive
```

> **注意**：`memoryrovol` submodule 以 `update = none` 接线；递归克隆不会自动
> 拉取它。且获取源码**并不**授予 License Key —— 要在生产环境中实际链接并使用
> MemoryRovol，仍需向 SPHARX Ltd. 申请 License Key。

### 将 MemoryRovol 链接进 AgentRT 运行时（商业）

MemoryRovol 在 AgentRT 构建期通过 `AIRY_WITH_MEMORYROVOL` CMake 选项（默认开
启）与 `AIRY_MEMORY_BACKEND` 后端选择器被消费。这要求 `memoryrovol` 源码可用，
且实际使用时需在运行期通过 license 文件配置有效的 License Key（许可与集成细节
见 `memoryrovol/README.md`）。

```bash
# 以 MemoryRovol 支持配置 AgentRT 运行时
cmake -S agentrt -B build-agentrt \
    -DAIRY_WITH_MEMORYROVOL=ON \
    -DAIRY_MEMORY_BACKEND=memoryrovol
cmake --build build-agentrt -j
```

将 `AIRY_MEMORY_BACKEND` 切回 `builtin`（默认值）后，运行时透明回退到内置的
开源 L1/L2 提供者 —— 下游无需任何源码改动。

> **禁止树内构建。** 依 Airymax 工程纪律，`airymaxhub` 源码树永不在原地构建。
> `products/` 只承载策略模块源码与接线；所有编译均发生在外部构建树（见
> `docs/AirymaxRT/50-engineering-standards`）。

## 分支策略

- **本管理仓**：仅 `main` 分支。所有发行 tag 均在 `main` 上切出。
- **叶子仓**（`memoryrovol`、`cupolas`）：集成分支为 `dev/hubs-01`，`main` 为
  发行快照。submodule 固定到精确 commit，保证本仓的每次检出均可复现。
- **直接追踪模块**（`lang_gateway`）：与本仓同步版本化。

## 许可证

**管理仓本身**（`products/`，即本 README、`.gitmodules` 配置、顶层 `LICENSE` /
`NOTICE` 文件）采用以下双许可证：

1. **GNU Affero General Public License v3.0 or later (AGPL v3)**
   —— https://www.gnu.org/licenses/agpl-3.0.html
2. **Apache License, Version 2.0**
   —— https://www.apache.org/licenses/LICENSE-2.0

SPDX：`AGPL-3.0-or-later OR Apache-2.0`。可任选其一。两份许可证全文均包含在
本管理仓根目录的 [`LICENSE`](LICENSE) 文件中。

### 双许可证使用指南

你可以**任选其一**适用——不是同时遵守两个，也不是都不遵守。

**SPDX 表达式**：`AGPL-3.0-or-later OR Apache-2.0`

| 你的场景 | 选择 | 原因 |
|----------|------|------|
| 构建**SaaS 网络服务**并修改这些策略模块 | **AGPL v3** | 网络服务条款要求公开修改后的源代码 |
| 开发**开源衍生作品**（copyleft 项目） | **AGPL v3** | 衍生作品必须同样以 AGPL 开源 |
| 在**商业闭源部署**中使用这些策略模块 | **Apache 2.0** | 宽松许可证，允许闭源衍生 |
| 构建**企业内部工具** | **Apache 2.0** | 无需公开源代码 |
| 需要**专利保护** | **Apache 2.0** | 贡献者明确授予专利使用权 |
| 仅用于学习与研究 | **任一** | 两者均允许个人使用 |

> **注意**：本双许可证指南仅适用于**管理仓本身**及 `cupolas/` / `lang_gateway/`
> 模块。`memoryrovol/` 子模块**不适用**本指南——它受 SPHARX 商业 EULA v1.0 约束，
> 需要单独的 License Key。详见下方[§ 各模块的许可证](#各模块的许可证)。

### 各模块的许可证

每个策略模块在其自身的 `LICENSE` 文件中携带**各自**的许可证；上述管理仓许可证
不会覆盖它们：

| 模块 | 许可证 | SPDX |
|------|--------|------|
| `cupolas/` | AGPL v3 + Apache 2.0（双许可证） | `AGPL-3.0-or-later OR Apache-2.0` |
| `lang_gateway/` | AGPL v3 + Apache 2.0（双许可证） | `AGPL-3.0-or-later OR Apache-2.0` |
| `memoryrovol/` | **SPHARX 商业 EULA v1.0（专有、闭源）** | `LicenseRef-SPHARX-MemoryRovol-EULA-1.0` |

特别地，**`memoryrovol` 不是开源软件**，不受 AGPL v3 或 Apache 2.0 任一许可证
约束。`memoryrovol` 的使用、再分发或链接要求接受 SPHARX 商业 EULA v1.0，并持有
对应授权层级（Trial / Pro / Enterprise / Enterprise+）的有效 License Key。完整
条款见 [§ MemoryRovol 许可证](#memoryrovol-许可证) 与
[`memoryrovol/LICENSE`](memoryrovol/LICENSE) 文件。

---

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.
