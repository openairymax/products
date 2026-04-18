# SpharxWorks — 数据智能基础设施

SpharxWorks 是一个数据智能基础设施项目，旨在提供完整的智能数据处理和分析能力，包括开放空气智能平台和核心工具集。

## 组件

| 组件 | 说明 |
|------|------|
| [OpenAirymax](OpenAirymax/) | 开放空气智能平台，包含 AgentOS 和 Docs-closed 子模块，提供智能空气监测和分析能力。 |
| [SpharxTools](SpharxTools/) | 核心工具集，包含 Workshop、Deepness 和 Benchmark 子模块，提供数据处理和分析工具。 |

详细文档请参阅各组件的 README。

## 快速开始

```bash
# 克隆仓库（包含子模块）
git clone --recursive git@atomgit.com:spharx/spharxworks.git

# 进入项目目录
cd spharxworks

# 更新子模块
git submodule update --init --remote
```

## 许可证

Apache License 2.0 — 详见 [LICENSE](LICENSE)。