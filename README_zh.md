# Airymax 产品层 — 桌面应用、Docker 与商业记忆模块

> Airymax AI 智能体运行时平台的**产品打包管理仓**。聚合三个叶子仓 —— 桌面应用、
> Docker 部署镜像、商业 MemoryRovol 记忆提供者 —— 形成面向最终用户、运维人员
> 与企业客户的统一交付面。
>
> [airymaxhub](https://atomgit.com/openairymax/airymaxhub) 伞仓下五个管理仓之一
> （Airymax 0.1.1 发行版：1 伞仓 + 5 管理仓 + 29 叶子仓 + 3 顶层仓 = 38 仓）。

**语言:** [English](README.md) | 简体中文

[![Version](https://img.shields.io/badge/version-0.1.1-5a6b7e)](https://atomgit.com/openairymax/products)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## 概述

**Products 管理仓**（`products/`）是 Airymax 平台的产品打包层。本仓本身不含一
方源码，而是以 git submodule 形式聚合三个独立的叶子仓，对外呈现为平台的可安装 /
可部署交付面：

- **desktop** —— 跨平台个人客户端（Tauri v2 + React 18）。
- **docker** —— 官方容器化部署镜像与 Compose 编排栈。
- **memoryrovol** —— 商业 L3/L4 记忆提供者（闭源）。

### 混合许可证提示

三个叶子仓中有两个（`desktop` 与 `docker`）采用与 Airymax 平台其余部分一致的
**AGPL v3 + Apache 2.0** 双许可证。第三个 `memoryrovol` 是**闭源商业产品**，受
**SPHARX 商业 EULA v1.0**（`LicenseRef-SPHARX-MemoryRovol-EULA-1.0`）约束，并
托管在 **`spharx`** 组织下，而非 `openairymax`。管理仓本身（即本 `products/`
仓库）采用 AGPL v3 + Apache 2.0 双许可证，但递归克隆本仓会拉取到 `memoryrovol`，
而 `memoryrovol` 的实际使用需要有效的 License Key。完整条款见
[§ MemoryRovol 许可证](#memoryrovol-许可证) 与 [§ 许可证](#许可证)。

## 仓库结构

```
products/                       # 本管理仓（AGPL v3 + Apache 2.0）
├── desktop/                    # git submodule → openairymax/desktop
│                               #   Airymax Desktop GUI（Tauri v2 + React 18）
├── docker/                     # git submodule → openairymax/docker
│                               #   Docker 镜像 & docker-compose 编排栈
├── memoryrovol/                # git submodule → spharx/memoryrovol  (闭源)
│                               #   商业 L3/L4 记忆提供者（SPHARX EULA）
├── .gitmodules                 # submodule 配置（3 个条目）
├── .gitignore
├── LICENSE                     # AGPL v3 + Apache 2.0（仅限管理仓本身）
├── NOTICE                      # 版权声明、商标与第三方组件说明
├── README.md                   # 英文版
└── README_zh.md                # 简体中文版（本文件）
```

## 叶子仓

| 模块 | 仓库 URL | 许可证 | 说明 |
|------|----------|--------|------|
| **desktop** | `git@atomgit.com:openairymax/desktop.git` | AGPL v3 + Apache 2.0 | 跨平台桌面应用（Airymax Desktop GUI）—— 基于 Tauri v2 / React 18 / TypeScript / Vite 构建的个人客户端。 |
| **docker** | `git@atomgit.com:openairymax/docker.git` | AGPL v3 + Apache 2.0 | 官方 Docker 镜像与 Docker Compose 编排栈，覆盖 AgentRT 运行时的开发、预发与生产部署。 |
| **memoryrovol** | `git@atomgit.com:spharx/memoryrovol.git` | SPHARX EULA v1.0（专有） | 商业闭源记忆提供者，实现 L3（结构层）与 L4（模式层）记忆。归属 `spharx` 组织，非 `openairymax`。 |

> **许可证说明。** `desktop` 与 `docker` 采用与 Airymax 平台其余部分一致的
> AGPL v3 + Apache 2.0 双许可证（SPDX：`AGPL-3.0-or-later OR Apache-2.0`）。
> `memoryrovol` **不是**开源软件：受 SPHARX 商业 EULA v1.0 约束
> （SPDX：`LicenseRef-SPHARX-MemoryRovol-EULA-1.0`），其使用需要与授权层级
> 绑定的 License Key。

## 产品架构

`products/` 下的三个叶子模块构成 Airymax 平台的**交付面** —— 即最终用户安装、
运维部署、企业集成的产物。它们位于运行时 / SDK / 生态层的下游，不会向上游回
馈代码。

```
                  ┌──────────────────────────────────────┐
                  │  上游源码层                          │
                  │  sdk/  ·  agentrt/  ·  ecosystem/    │
                  │  atoms/memoryrovol/  (bridge API)    │
                  └────────────────┬─────────────────────┘
                                   │
   ┌───────────────────────────────┼───────────────────────────────┐
   │                               │                               │
   ▼                               ▼                               ▼
┌──────────────┐         ┌──────────────────┐         ┌──────────────────────┐
│   desktop    │◀───────▶│      docker      │◀─link───│     memoryrovol      │
│ 个人 GUI     │ 可选    │   OCI 镜像 +     │         │ L3 结构层           │
│ Tauri v2 +   │ gateway │   docker-compose │         │ L4 模式层           │
│ React 18     │ 后端    │   dev/staging/   │         │ (闭源，             │
│ Win/macOS/Lnx│         │   prod           │         │  SPHARX EULA)       │
└──────┬───────┘         └────────┬─────────┘         └──────────┬───────────┘
       │                          │                              │
       ▼                          ▼                              ▼
   最终用户                  运维 / 企业部署                   AgentRT 运行时
   (个人)                                                     (通过 AGENTRT_WITH_
                                                              MEMORYROVOL=ON)
```

### desktop —— 个人客户端

桌面应用将运行时、SDK 与生态能力打包为一个可安装的 GUI，基于 **Tauri v2**
（Rust 内核）+ **React 18 + TypeScript + Vite** 前端构建。目标平台覆盖 Windows、
macOS 与 Linux，支持离线优先 PWA 行为、系统托盘集成、全局快捷键，以及与本地
运行的 AgentRT gateway 的内置连接。

- **上游**：`sdk/agentrt/`（运行时 + SDK 的 gateway HTTP / WebSocket API）、
  `products/docker/`（可选的伴随 gateway 后端）。
- **下游**：最终用户（个人用户），安装产出的 `.exe` / `.dmg` / `.deb` /
  `.AppImage` 制品。

### docker —— 容器化部署

Docker 模块为 AgentRT 运行时、daemon 服务、gateway、OpenLab 与桌面 Web 前端提
供可复现、安全加固的 OCI 镜像，并提供三套 Docker Compose 编排（开发 / 预发 /
生产），已与 PostgreSQL、Redis 以及 Prometheus + Grafana + AlertManager 可观测
性栈打通。

- **镜像**：`Dockerfile.kernel`、`Dockerfile.daemon`、`Dockerfile.openlab`、
  `Dockerfile.desktop`。
- **编排栈**：`docker-compose.yml`（开发）、`docker-compose.staging.yml`、
  `docker-compose.prod.yml`。
- **上游**：`agentrt/` 运行时源码树、`ecosystem/manager/` 配置默认值、
  `products/desktop/` 源码（被 `Dockerfile.desktop` 消费）。
- **下游**：DevOps / SRE / 企业运维人员，在开发、预发或生产环境中运行产出镜像。

### memoryrovol —— 商业记忆提供者

MemoryRovol 以预编译、许可证门控的 C 静态库（`.a` / `.lib`）形式实现 Airymax
四层记忆架构中的 **L3（结构层）** 与 **L4（模式层）**，在构建期链接进 AgentRT
运行时。运行时加载时，CoreLoopThree 桥通过 **弱符号（weak symbol）** 契约解析
MemoryRovol：若链接了 MemoryRovol，桥将记忆调用路由给它；若未链接，桥透明回退
到运行时内置的开源提供者（仅覆盖 L1 与 L2）。

- **上游**：`atoms/memoryrovol/` 桥 API（弱符号）。
- **下游**：AgentRT 运行时（通过 `AGENTRT_WITH_MEMORYROVOL=ON` CMake 选项）。
- **许可证**：SPHARX 商业 EULA v1.0 —— 详见
  [§ MemoryRovol 许可证](#memoryrovol-许可证)。

### 记忆分层

```
L1 原始层       ← 内置（开源，AGPL v3 + Apache 2.0）
L2 特征层       ← 内置（开源，AGPL v3 + Apache 2.0）
L3 结构层       ← MemoryRovol（商业，SPHARX EULA）
L4 模式层       ← MemoryRovol（商业，SPHARX EULA）
```

## MemoryRovol 许可证

`memoryrovol` 是整个 Airymax 0.1.1 发行版中唯一的闭源组件。它受 **SPHARX 商业
EULA v1.0**（SPDX：`LicenseRef-SPHARX-MemoryRovol-EULA-1.0`）约束，由
**SPHARX Ltd.** 出品并所有，托管在 **`spharx`** 组织下
（`git@atomgit.com:spharx/memoryrovol.git`），而非 `openairymax`。

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

## 构建与部署

### 前置条件

- Git ≥ 2.30（支持 submodule）。
- `desktop`：Node.js 18+、Rust（stable）工具链、Tauri v2 系统依赖
  （依操作系统为 WebKit / GTK / MSVC）。
- `docker`：Docker 20.10+、Docker Compose v2.20+。
- `memoryrovol`（仅在链接进 AgentRT 时）：CMake 3.20+、C11 编译器、有效的
  MemoryRovol License Key。

### 带 submodule 克隆

```bash
# 克隆管理仓及全部三个叶子 submodule
git clone --recurse-submodules git@atomgit.com:openairymax/products.git

# 若已克隆但未带 --recurse-submodules：
cd products
git submodule update --init --recursive
```

> **注意**：递归克隆会拉取 `memoryrovol`。获取源码**并不**授予 License Key；
> 要在生产环境中实际链接并使用 MemoryRovol，仍需向 SPHARX Ltd. 申请 License
> Key。

### 构建桌面应用

桌面模块是 Tauri v2 应用，由 npm 脚本驱动。完整平台相关说明见
`desktop/README.md`。

```bash
cd desktop

# 安装前端依赖
npm install

# 启动 Vite 开发服务器 + Tauri 开发壳
npm run tauri dev

# 类型检查 + lint + 前端生产构建
npm run check

# 构建平台原生安装包（.exe / .dmg / .deb / .AppImage）
npm run tauri build
```

产物落在 `desktop/src-tauri/target/release/bundle/`。默认 gateway 端点为
`http://localhost:18789`，可通过 `VITE_AGENTOS_GATEWAY_HOST` /
`VITE_AGENTOS_GATEWAY_PORT` 覆盖（见 `desktop/.env.example`）。

### 构建并运行 Docker 镜像

Docker 模块提供四份多阶段 Dockerfile 与三套 Compose 编排栈。完整说明见
`docker/README.md` 与 `docker/DEPLOYMENT.md`。

```bash
cd docker

# 开发栈（kernel + daemon + gateway + OpenLab + desktop web，
# 加上 PostgreSQL + Redis + 监控）
docker compose --env-file .env.example up -d --build

# 预发栈
docker compose -f docker-compose.staging.yml \
    --env-file .env.staging.example up -d --build

# 生产栈
docker compose -f docker-compose.prod.yml \
    --env-file .env.production.example up -d --build

# 直接构建单个镜像（如 kernel 镜像）
docker build -f Dockerfile.kernel -t airymax-kernel:0.1.1 ..
```

所有镜像均以非 root 的 `agentos:1000` 用户运行，启用 `read_only` 文件系统、
`seccomp` profile、`cap_drop: ALL` 且禁止提权，与 CIS Docker Benchmark 基线对齐。

### 将 MemoryRovol 链接进 AgentRT 运行时（商业）

MemoryRovol 在 AgentRT 构建期通过 CMake 选项被消费。需要有效的 License Key 并
检出 `memoryrovol` submodule。

```bash
# 在 agentrt/ 构建目录内
cmake -S ../agentrt -B build \
    -DAGENTRT_WITH_MEMORYROVOL=ON \
    -DMEMORYROVOL_LICENSE_KEY=<your-license-key>
cmake --build build -j
```

当 `AGENTRT_WITH_MEMORYROVOL=ON` 处于**关闭**状态（或未提供 License Key）时，
运行时透明回退到内置的开源 L1/L2 提供者 —— 下游无需任何源码改动。

## 分支策略

- **本管理仓**：仅 `main` 分支。所有发行 tag 均在 `main` 上切出。
- **叶子仓**（`desktop`、`docker`、`memoryrovol`）：`feature/official-hubs-01`
  为活跃开发分支，由 `.gitmodules` 中的 `branch =` 配置跟踪。各叶子仓的 `main`
  分支在发行时接收合并后的稳定化提交。

## 许可证

**管理仓本身**（`products/`，即本 README、`.gitmodules` 配置、顶层 `LICENSE` /
`NOTICE` 文件）采用以下双许可证：

1. **GNU Affero General Public License v3.0 or later (AGPL v3)**
   —— https://www.gnu.org/licenses/agpl-3.0.html
2. **Apache License, Version 2.0**
   —— https://www.apache.org/licenses/LICENSE-2.0

SPDX：`AGPL-3.0-or-later OR Apache-2.0`。可任选其一。两份许可证全文均包含在
本管理仓根目录的 [`LICENSE`](LICENSE) 文件中。

### 各 submodule 的许可证

每个叶子 submodule 在其自身的 `LICENSE` 文件中携带**各自**的许可证；上述管理仓
许可证不会覆盖它们：

| Submodule | 许可证 | SPDX |
|-----------|--------|------|
| `desktop/` | AGPL v3 + Apache 2.0（双许可证） | `AGPL-3.0-or-later OR Apache-2.0` |
| `docker/` | AGPL v3 + Apache 2.0（双许可证） | `AGPL-3.0-or-later OR Apache-2.0` |
| `memoryrovol/` | **SPHARX 商业 EULA v1.0（专有、闭源）** | `LicenseRef-SPHARX-MemoryRovol-EULA-1.0` |

特别地，**`memoryrovol` 不是开源软件**，不受 AGPL v3 或 Apache 2.0 任一许可证
约束。`memoryrovol` 的使用、再分发或链接要求接受 SPHARX 商业 EULA v1.0，并持有
对应授权层级（Trial / Pro / Enterprise / Enterprise+）的有效 License Key。完整
条款见 [§ MemoryRovol 许可证](#memoryrovol-许可证) 与
[`memoryrovol/LICENSE`](memoryrovol/LICENSE) 文件。

---

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.
