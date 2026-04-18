# SpharxWorks — 数据智能基础设施

<div align="center">

## 🎯 项目简介

SpharxWorks 是 SPHARX 极光感知科技打造的数据智能基础设施，整合了两大核心模块：

- **OpenAirymax**（开源极境）：群体智能的底层支撑，一个开源的 AgentOS 项目
- **SpharxTools**：物理世界数据的生产工具链

**From data intelligence emerges**
**始于数据，终于智能**

</div>

## 🌐 项目导航

| 模块 | 描述 | 文档链接 |
|------|------|----------|
| **OpenAirymax** | 开源极境 - 群体智能的底层支撑 | [OpenAirymax 文档](OpenAirymax/README.md) |
| **AgentOS** | 智能体底层操作系统 | [AgentOS 文档](OpenAirymax/AgentOS/README_zh.md) |
| **SpharxTools** | 物理世界数据生产工具链 | [SpharxTools 文档](SpharxTools/README.md) |
| **Workshop** | 物理世界数据工厂 | [Workshop 文档](SpharxTools/Workshop/README.md) |
| **Deepness** | 深度加工生产线 | [Deepness 文档](SpharxTools/Deepness/README.md) |
| **Benchmark** | 评测工具 | 内部访问 |

## 📋 什么是 SpharxWorks？

SpharxWorks 是一个完整的数据智能基础设施，旨在构建人工智能时代的物理世界数据处理标准。它通过整合物理世界数据采集、处理与智能体操作系统，为 AI 应用提供从原始数据到智能决策的全链路解决方案。

### 核心价值

1. **数据智能闭环**：从物理世界数据采集到智能决策的完整闭环
2. **技术创新**：领先的物理世界理解和智能体操作系统技术
3. **生态整合**：开放的生态系统，支持多行业应用场景
4. **商业价值**：为企业提供数据驱动的智能解决方案

## 🏗️ 核心项目介绍

### 1. AgentOS — 智能体底层操作系统

<div align="center">

[![AtomGit](https://atomgit.com/openairymax/agentos/star/badge.svg)](https://atomgit.com/openairymax/agentos)
[![star](https://gitee.com/spharx/agentos/badge/star.svg?theme=dark)](https://gitee.com/spharx/agentos)
[![GitHub stars](https://img.shields.io/github/stars/SpharxTeam/AgentOS)](https://github.com/SpharxTeam/AgentOS/stargazers)

[![Version](https://img.shields.io/badge/version-1.0.0.9-5a6b7e)](https://atomgit.com/spharx/agentos)
[![License](https://img.shields.io/badge/license-Apache--2.0-4a90d9)](LICENSE)
[![Build](https://img.shields.io/badge/build-passing-2ea44f)](https://atomgit.com/spharx/agentos)

[![C/C++](https://img.shields.io/badge/C%2FC%2B%2B-11%2F17-00599C?logo=c%2B%2B&logoColor=white)](https://isocpp.org)
[![Python](https://img.shields.io/badge/Python-3.10+-3776AB?logo=python&logoColor=white)](https://www.python.org)
[![Go](https://img.shields.io/badge/Go-1.21+-00ADD8?logo=go&logoColor=white)](https://go.dev)
[![Rust](https://img.shields.io/badge/Rust-1.70+-DEA584?logo=rust&logoColor=white)](https://www.rust-lang.org)
[![TypeScript](https://img.shields.io/badge/TypeScript-4.9+-3178C6?logo=typescript&logoColor=white)](https://www.typescriptlang.org)

</div>

**定位**：成为人类计算工程史上，第四个"操作系统哲学"

AgentOS 是一个智能体底层操作系统，为驱动智能体团队提供完整的操作系统级支持。基于基石理论《体系并行论》构建，实现了从内核到应用的完整架构。

#### 💡 创新要点

- **纯净内核**：内核仅提供原子机制，纯净高效，内核代码量仅 ~25K LOC
- **认知循环**：认知、规划、行动的智能体行为模式
- **记忆卷载**：原始层、特征层、结构层、模式层的四层记忆系统
- **安全内生**：沙箱隔离、权限裁决、输入净化、审计追踪的四重安全体系
- **高效 Token**：工程级比传统框架节省约 **500%** 的 Token 消耗
- **丰富 SDK**：原生支持 Go、Python、Rust、TypeScript 四种编程语言

#### 🏗️ 系统架构

```
⬇️ 应用层 (openlab)    - 用户交互与应用逻辑
⇅ 服务层 (daemon)      - 后台服务与守护进程
⇅ 内核层 (atoms)       - 核心原子机制与系统调用
⇅ 安全层 (cupolas)     - 安全穹顶与权限管理
⇅ 支撑层 (commons)     - 公共库与基础设施
⬆️ SDK 层 (toolkit)    - 多语言开发工具包
```

#### 🎯 设计原则

基于 ARCHITECTURAL_PRINCIPLES 构建，遵循五维正交体系：

- **系统观**：反馈闭环 · 层次分解 · 总体设计部 · 涌现管理 → 实时响应 <10ms
- **内核观**：内核极简 · 接口契约化 · 服务隔离 · 可插拔策略 → 内核 ~25K LOC
- **认知观**：双系统协同 · 增量演化 · 记忆卷载 · 遗忘机制 → Token 节省 500%
- **工程观**：安全内生 · 可观测性 · 资源确定性 · 跨平台一致 → 测试覆盖 >90%
- **设计美学**：简约至上 · 极致细节 · 人文关怀 · 完美主义 → API <50 个/模块

#### 🆚 与传统框架对比

| 维度 | AgentOS | 传统框架 |
|------|---------|----------|
| **定位** | 多智能体协作 OS | 单一智能体 |
| **架构** | 微内核 + 严格分层 | 松耦合模块 |
| **安全** | 四重内生安全 | 应用层防护 |
| **记忆** | 四层卷载系统 | 向量数据库 |
| **Token 效率** | 节省约 500% | 无优化 |

#### 🎯 应用场景

**✅ 特别适合**
- 🎯 复杂多步骤任务编排
- 🧠 长期记忆与知识积累需求
- 🔒 高安全性企业应用
- 💾 资源受限嵌入式场景 (atomslite)
- 🌐 多语言开发团队

**❌ 不适合**
- 🚫 简单单次调用任务（杀鸡用牛刀）

#### ⭐️ GitHub Star History

[![Star History Chart](https://api.star-history.com/chart?repos=SpharxTeam/AgentOS&type=date&legend=top-left)](https://star-history.com/#SpharxTeam/AgentOS)

---

### 2. Workshop — 物理世界数据工厂

<div align="center">

[![AtomGit](https://atomgit.com/spharx/workshop/star/badge.svg)](https://atomgit.com/spharx/workshop)
[![star](https://gitee.com/spharx/workshop/badge/star.svg?theme=dark)](https://gitee.com/spharx/workshop)
[![GitHub](https://img.shields.io/github/stars/SpharxTeam/Workshop?style=social)](https://github.com/SpharxTeam/Workshop)

[![Version](https://img.shields.io/badge/version-3.0.0-5a6b7e)](https://atomgit.com/spharx/workshop)
[![License](https://img.shields.io/badge/license-GPL--3.0-4a90d9)](LICENSE)
[![Build](https://img.shields.io/badge/build-passing-2ea44f)](https://atomgit.com/spharx/workshop)

[![Python](https://img.shields.io/badge/Python-3.8+-3776AB?logo=python&logoColor=white)](https://www.python.org)
[![Docker](https://img.shields.io/badge/Docker-20.10+-2496ED?logo=docker&logoColor=white)](https://www.docker.com)
[![Prometheus](https://img.shields.io/badge/Prometheus-2.45-E6522C?logo=prometheus&logoColor=white)](https://prometheus.io)
[![Grafana](https://img.shields.io/badge/Grafana-10.2-F46800?logo=grafana&logoColor=white)](https://grafana.com)

</div>

**定位**：构建 AI 时代的物理世界数据基础设施

Workshop V3.0 是 SpharxWorks 平台的核心数据采集和预处理子系统，基于 AgentOS 微内核架构和 Deepness 模块化设计构建，采用五层架构设计。从原始传感器数据到标准化高质量数据集，实现完整处理链路。

#### 💡 核心特性

- **五层架构**：Core → Commons → Orchestration → Services → Pipelines
- **模块化设计**：基于 BasePipeline ABC 的标准化管道框架
- **生产级质量**：100+ 错误码体系，输入验证覆盖率 99%
- **安全内生**：SQL/XSS 检测、路径遍历防护、审计日志
- **可观测性**：分布式追踪、性能监控、健康检查
- **DevOps 完善**：GitHub Actions CI/CD，Docker 多阶段构建

#### 🏗️ 系统架构

**V3.0 五层架构** · 参考 AgentOS 和 Deepness 设计模式

```
┌─────────────────────────────────────────────────────────┐
│                    Pipelines 层                          │
│    run_00_ingest → run_01_quality → ... → delivery      │
├─────────────────────────────────────────────────────────┤
│                    Services 层                           │
│         Gateway · Monitor · Exporter                    │
├─────────────────────────────────────────────────────────┤
│                  Orchestration 层                        │
│       Scheduler · TaskQueue · WorkflowEngine            │
├─────────────────────────────────────────────────────────┤
│                     Commons 层                           │
│          Utils · Schemas · Decorators                   │
├─────────────────────────────────────────────────────────┤
│                      Core 层                             │
│  Abstractions · Services · Security · Observability     │
└─────────────────────────────────────────────────────────┘
```

#### 🔄 六阶段处理流水线

| 阶段 | 模块 | 核心功能 |
|------|------|---------|
| 1 | **00_ingest** | ROS Bag 解析、图像压缩、隐私脱敏 |
| 2 | **01_quality** | 模糊检测、曝光分析、帧丢弃检测 |
| 3 | **02_enhance** | YOLO 目标检测、HDVS 分割 |
| 4 | **03_calibrate** | 棋盘格相机校准、重投影误差评估 |
| 5 | **04_pack** | 多格式数据集打包 (ROS/COCO/YOLO/VOC/KITTI) |
| 6 | **05_delivery** | OSS 上传、交付通知、完整性校验 |

#### 📐 设计原则

基于 AgentOS K-1 至 K-4 架构原则：

- **K-1 极简内核**：核心基础设施层最小化，仅包含必要抽象
- **K-2 接口契约**：BasePipeline ABC 强制标准化生命周期
- **K-3 服务隔离**：每个 Pipeline 模块独立容器化部署
- **K-4 插拔策略**：运行时算法切换与配置热加载

#### 📈 当前应用状况

Workshop V3.0 版本已达到生产就绪状态，在实际项目中稳定运行：
- ✅ 完成硬件同步方案（3×D455 相机）
- ✅ 实现 RealSense 数据解析功能
- ✅ 集成 YOLOv8 目标检测
- ✅ 建立完整的质量控制体系
- ✅ 支持多种数据交付方式
- ✅ 六阶段流水线全部模块生产就绪

---

### 3. Deepness — 深度加工生产线

<div align="center">

[![AtomGit](https://atomgit.com/spharx/deepness/star/badge.svg)](https://atomgit.com/spharx/deepness)
[![star](https://gitee.com/spharx/deepness/badge/star.svg?theme=dark)](https://gitee.com/spharx/deepness)
[![GitHub](https://img.shields.io/github/stars/spharx/deepness?style=social)](https://github.com/spharx/deepness)

[![Version](https://img.shields.io/badge/version-2.0.0-5a6b7e)](https://atomgit.com/spharx/deepness)
[![License](https://img.shields.io/badge/license-GPL--3.0-4a90d9)](LICENSE)
[![Build](https://img.shields.io/badge/build-passing-2ea44f)](https://atomgit.com/spharx/deepness)

[![Python](https://img.shields.io/badge/Python-3.11+-3776AB?logo=python&logoColor=white)](https://www.python.org)
[![PyTorch](https://img.shields.io/badge/PyTorch-2.5.1-EE4C2C?logo=pytorch&logoColor=white)](https://pytorch.org)
[![Docker](https://img.shields.io/badge/Docker-20.04+-2496ED?logo=docker&logoColor=white)](https://www.docker.com)

</div>

**定位**：始于数据，终于智能

Deepness 是 SpharxWorks 平台的核心深度加工子系统，致力于将原始物理世界数据转化为富含语义和物理属性的高价值数据资产。系统采用模块化、容器化的设计理念，通过四个核心处理管道实现从基础数据到高级应用的完整转换链路。

#### 🎯 核心价值

- **物理世界数字化**：将现实场景转化为可计算、可仿真的数字孪生体
- **语义丰富化**：为3D场景注入丰富的语义和物理属性信息
- **交互建模**：记录和分析物理交互行为，支持反事实推演
- **标准输出**：生成符合主流评测框架的标准化数据格式

#### 🏗️ 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Deepness 系统架构                         │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────┐    ┌──────────────┐    ┌─────────────────┐ │
│  │   输入层     │───→│  处理层       │───→│    输出层        │ │
│  │             │    │              │    │                 │ │
│  │ • 原始数据   │    │ • 空间先验    │    │ • Benchmark     │ │
│  │ • RGB-D     │    │ • 物理预测    │    │   格式          │ │
│  │ • 时间戳     │    │ • 因果轨迹    │    │ • 应用接口      │ │
│  │             │    │ • 评测导出    │    │                 │ │
│  └─────────────┘    └──────────────┘    └─────────────────┘ │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

#### 🔄 四大核心处理管道

**1. run_01_spatial_prior** (空间先验生成)
- 基于 NVIDIA Cosmos 世界模型实现空间理解
- 生成场景的语义分割和几何先验
- 为下游任务提供基础空间表征

**2. run_02_physics_jepa** (物理属性预测)
- 基于 Meta JEPA 架构预测物体物理属性
- 推断材质、密度、摩擦系数等参数
- 构建物理一致的 3D 场景模型

**3. run_03_causal_trajectory** (因果轨迹记录)
- 整合 YOLOv8 动态检测和 ORB-SLAM3/LIO-SAM 定位
- 记录物理交互轨迹和因果链
- 支持反事实推演和轨迹分析

**4. run_04_benchmark_export** (评测数据导出)
- 转换为标准评测格式（Genie Sim 3.0、JEPA、Spatial）
- 支持多种世界模型评估框架
- 提供数据质量报告和统计信息

#### 🔧 技术架构

**基础设施**:
- **Docker 容器化**：基于 CUDA 12.1 的 Ubuntu 22.04 基础镜像
- **Python 环境**：Miniforge3 + Python 3.11，预装科学计算包
- **深度学习**：PyTorch 2.5.1 (CUDA 12.1)、PyTorch3D 0.7.6
- **3D 处理**：Open3D 0.18.0、Kaolin 0.17.0、gsplat 渲染引擎
- **世界模型**：NVIDIA Cosmos、Meta JEPA
- **视觉算法**：YOLOv8、ORB-SLAM3、LIO-SAM

**架构设计**:
- **四层架构**：应用层 → 编排层 → 处理层 → 核心层
- **微内核设计**：核心仅提供基础抽象，服务可插拔
- **依赖注入**：通过接口契约实现松耦合
- **Pydantic V2**：类型安全的数据模型

#### 📈 开发进度

当前版本 v2.0.0 开发进度 90%:
- ✅ 基础架构搭建完成
- ✅ Docker 容器化配置就绪
- ✅ 四个核心处理模块全部实现
  - ✅ run_01_spatial_prior (Cosmos 适配器)
  - ✅ run_02_physics_jepa (JEPA 物理预测)
  - ✅ run_03_causal_trajectory (SLAM+ 因果推理)
  - ✅ run_04_benchmark_export (多格式导出)
- ✅ 核心架构重构完成
  - ✅ 四层架构（应用/编排/处理/核心）
  - ✅ 安全与可观测性层
  - ✅ 统一包结构
- ⏳ 端到端集成测试中 (90%)
- 🔲 性能基准测试

---

### 4. Benchmark — 评测工具

**定位**：数据质量与系统性能评估

Benchmark 是 SpharxWorks 的评测工具，用于数据质量和系统性能评估。它提供了完整的测试框架和评估指标，确保数据处理流程的质量和性能。

#### 🎯 核心功能

- **数据质量评估**：评估数据集的完整性、准确性、一致性
- **系统性能测试**：测试系统吞吐量、延迟、资源利用率
- **基准对比**：与行业标准进行对比分析
- **报告生成**：自动生成详细的评估报告

#### 📊 评估维度

| 维度 | 指标 | 说明 |
|------|------|------|
| **数据质量** | 完整性、准确性、一致性 | 评估数据集质量 |
| **系统性能** | 吞吐量、延迟、资源利用率 | 测试系统性能 |
| **基准对比** | 与行业标准对比 | 对比分析 |

## 🌟 技术创新

### 1. 物理世界理解
- **多模态融合**：整合 RGB-D 相机、IMU 等多种传感器数据
- **空间先验**：基于 NVIDIA Cosmos 世界模型的空间理解
- **物理属性预测**：基于 Meta JEPA 架构的物体物理属性推断
- **因果推理**：记录物理交互轨迹和行为模式

### 2. 智能体操作系统
- **微内核设计**：内核极简，接口契约化，服务隔离
- **认知架构**：双系统协同，增量演化，记忆卷载，遗忘机制
- **安全设计**：虚拟工位，权限裁决，输入净化，审计追踪
- **跨平台兼容**：支持 Linux、macOS、Windows (WSL2)

### 3. 工程实践
- **容器化部署**：基于 Docker 的微服务架构，支持弹性扩展
- **配置驱动**：模块化配置管理体系，支持热更新
- **监控可观**：完善的日志系统和实时状态监控
- **持续集成**：自动化测试和部署流水线

## 📈 发展路线图

### 短期目标 (2026 Q2-Q3)
- ✅ **Workshop v3.0.0** 生产就绪 - 六阶段流水线全部模块完成
- ⏳ **Deepness v2.0.0** 端到端集成 (90%) - 四大处理管道联调中
- ✅ **AgentOS v1.0.0.9** 核心功能完善

### 中期规划 (2026 Q4-2027)
- 🚀 **Deepness v2.1.0** 正式发布 - 完成性能基准测试和生产部署
- 🔄 **Workshop → Deepness → AgentOS 完整数据流** - 实现三个系统的无缝集成
- 🌐 **开发者生态** - 完善 SDK 和示例

### 长期愿景 (2027+)
- 🌐 **物理 AI 基础设施** - 成为人工智能时代的物理世界数据处理标准
- 🤝 **全球化开源社区** - 建立开放包容的技术生态
- 🏆 **下一代技术引领** - 物理世界理解与智能决策技术的持续创新

## 💼 商业价值

### 行业应用

| 行业 | 应用场景 | 价值 |
|------|----------|------|
| **智能机器人** | 环境感知、任务规划、自主导航 | 提升机器人智能水平和可靠性 |
| **自动驾驶** | 环境理解、行为预测、决策规划 | 提高自动驾驶安全性和准确性 |
| **AR/VR** | 空间映射、环境理解、虚实融合 | 增强用户体验和交互能力 |
| **工业制造** | 质量检测、流程优化、预测维护 | 提高生产效率和产品质量 |
| **智慧城市** | 环境监测、交通管理、安全监控 | 提升城市管理水平和居民生活质量 |

### 商业模式

- **开源生态**：通过开源社区建设，扩大技术影响力
- **商业授权**：为企业提供商业闭源授权和技术支持
- **解决方案**：提供行业定制化解决方案
- **硬件集成**：与硬件厂商合作，提供一体化解决方案

## 🤝 生态合作

我们诚邀各界合作伙伴共同建设数据智能基础设施生态：

### 技术合作伙伴
- **硬件厂商**：传感器、相机、GPU/NPU计算设备提供商
- **算法团队**：3D 重建、SLAM、物理仿真、大模型等领域专家
- **应用企业**：机器人、自动驾驶、AR/VR、智能助理等落地场景

### 投资机会

SpharxWorks 作为数据智能基础设施，具有广阔的市场前景：
- **技术领先**：掌握物理世界理解和智能体操作系统核心技术
- **市场潜力**：人工智能时代的基础设施需求巨大
- **生态优势**：开放的生态系统，支持多行业应用
- **团队实力**：核心团队来自顶尖科技公司和研究机构

## 📧 联系方式

### 商务合作
- **商务洽谈**：zhouzhixian@spharx.cn
- **技术合作**：lidecheng@spharx.cn
- **投资咨询**：wangliren@spharx.cn

### 官方网站
- **主站**：https://spharx.cn
- **开发者社区**：https://atomgit.com/spharx

## 📄 许可证

| 模块 | 许可证 |
|------|--------|
| **SpharxWorks** | Apache License 2.0 |
| **AgentOS** | Apache License 2.0 |
| **SpharxTools** | GPL-3.0 开源协议 + 商业闭源授权 |

## 🙏 致谢

感谢所有为开源社区做出贡献的开发者们，正是你们的努力让这样的项目成为可能。

<div align="center">

<h3>From data intelligence emerges</h3>
<h3>始于数据，终于智能</h3>

<p><em>构建 AI 时代的数据智能基础设施</em></p>

© 2026 SPHARX Ltd. All Rights Reserved.

</div>