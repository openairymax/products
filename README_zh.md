# Airymax Products — 打包与分发

> Airymax AI 智能体运行时平台的产品打包层。
> [airymaxhub](https://atomgit.com/openairymax/airymaxhub) 伞仓下四个管理仓之一。

**语言:** [English](README.md) | 简体中文

[![Version](https://img.shields.io/badge/version-0.1.1-5a6b7e)](https://atomgit.com/openairymax/products)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## 概述

**Products 管理仓**聚合 Airymax 的产品打包：桌面应用、Docker 部署镜像和商业 MemoryRovol 记忆提供者。这些是最终用户安装和部署的可交付制品。

## 叶子仓

| 模块 | 仓库 | 许可证 | 说明 |
|------|------|--------|------|
| **desktop** | `git@atomgit.com:openairymax/desktop.git` | AGPL v3 + Apache 2.0 | 桌面应用（个人客户端） |
| **docker** | `git@atomgit.com:openairymax/docker.git` | AGPL v3 + Apache 2.0 | Docker 部署镜像 & docker-compose |
| **memoryrovol** | `git@atomgit.com:spharx/memoryrovol.git` | SPHARX EULA v1.0 | 商业记忆提供者（闭源，C 类隔离层） |

> **注意**：`memoryrovol` 归属 `spharx` 组织（非 `openairymax`），采用商业 EULA 许可证。Airymax 项目中其余所有仓库均采用 AGPL v3 + Apache 2.0 双许可证。

## 架构

```
products/
├── desktop/       ← 桌面应用（个人客户端）
├── docker/        ← Docker 镜像 & 部署配置
├── memoryrovol/   ← 商业记忆提供者（SPHARX EULA）
├── .gitmodules
└── README.md      ← 本文件
```

### 模块详情

#### desktop

桌面应用提供与 Airymax 运行时交互的个人客户端。它构建在 SDK 层之上，提供图形界面用于 Agent 管理、任务提交和结果可视化。

- **上游**：SDK（`sdk/`）、运行时（`agentrt/`）
- **下游**：最终用户（个人用户）

#### docker

Docker 模块为 Airymax 运行时提供容器化部署。它包含 Dockerfile、docker-compose 配置和生产环境的部署脚本。

- **上游**：运行时（`agentrt/`）、生态配置（`ecosystem/manager/`）
- **下游**：运维人员、企业部署

#### memoryrovol

MemoryRovol 是 Airymax 的商业记忆提供者，实现 L3（结构层）和 L4（模式层）记忆。它是**闭源**产品，采用 SPHARX 商业 EULA v1.0 许可证，授权层级：Trial / Pro / Enterprise / Enterprise+。

- **上游**：`atoms/memoryrovol/` bridge API（弱符号）
- **下游**：运行时（通过 `AGENTRT_WITH_MEMORYROVOL=ON` CMake 选项）
- **SPDX**：`LicenseRef-SPHARX-MemoryRovol-EULA-1.0`

## 记忆分层

```
L1 原始层   ← 内置（开源，AGPL v3 + Apache 2.0）
L2 特征层   ← 内置（开源，AGPL v3 + Apache 2.0）
L3 结构层   ← MemoryRovol（商业，SPHARX EULA）
L4 模式层   ← MemoryRovol（商业，SPHARX EULA）
```

## 分支策略

- 本管理仓：仅 **`main`** 分支
- 叶子仓：**`feature/official-hubs-01`**（活跃开发）

## 许可证

- **desktop** & **docker**：AGPL v3 + Apache 2.0（SPDX: `AGPL-3.0-or-later OR Apache-2.0`）
- **memoryrovol**：SPHARX 商业 EULA v1.0（SPDX: `LicenseRef-SPHARX-MemoryRovol-EULA-1.0`）

详见各叶子仓的 [LICENSE](LICENSE) 文件。

Copyright (c) 2025-2026 **SPHARX Ltd.** All Rights Reserved.
