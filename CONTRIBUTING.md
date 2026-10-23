# 贡献指南 (Contributing Guide) — Products 管理仓

**最后更新**: 2026-10-23

本仓库（`products`）是策略件聚合仓，承载平台中**策略形态**的可替换载荷，与
机制核（`agentrt/`）物理分离。当前聚合 5 个策略件：

| 策略件 | 形态 | 类别 |
|--------|------|------|
| `memoryrovol` | submodule（非本仓追踪） | 闭源商业记忆产品 |
| `cupolas` | submodule（非本仓追踪） | 安全穹顶 + 策略决策点（PDP） |
| `lang_gateway` | 本仓直接追踪 | 推理语言网关 |
| `cognition` | 本仓直接追踪 | 认知策略载荷 |
| `gateway_biz` | 本仓直接追踪（嵌入式产品件） | 网关业务面，仅由 `gateway_d` 装配 |

三种聚合形态是**有意设计**，不是历史遗留：闭源与独立演进件用 submodule 钉住
确切提交，尚无独立叶子仓的直接追踪，`gateway_biz` 只作为 `gateway_d` 的编译期
装配件存在（缺席即不挂载守护进程，绝不生成桩）。

## 1. 提交前检查

所有构建与测试都在**源码区之外**的外部构建目录进行（`airymaxhub` 源码树严禁
就地编译）：

```bash
cmake -S products/<module> -B <external-build-dir> \
      -DAGENTRT_ROOT=agentrt -DBUILD_TESTS=ON
cmake --build <external-build-dir> --parallel "$(nproc)"
ctest --test-dir <external-build-dir> --output-on-failure
```

| 策略件 | 测试方式 |
|--------|----------|
| `cognition` | `ctest`（dispatch / plan / coord / review / grad / intent） |
| `cupolas` | `ctest`（core / error-code 一致性 / PEP 初始化） |
| `lang_gateway` | `ctest`（`lang_gateway_tests`） |
| `gateway_biz` | 无独立 `tests/`，由宿主 `gateway_d/tests` 链接 `airy_gateway_biz` 覆盖 |
| `memoryrovol` | 不接收外部源码贡献，见 §2 |

`gateway_biz` 缺席时 `gateway_d` 不挂载业务面，因此其回归必须在
`gateway_d` 的测试树中随宿主一起验证。

## 2. 闭源产品（memoryrovol）

MemoryRovol 是闭源商业产品（SPHARX EULA v1.0，SPDX:
`LicenseRef-SPHARX-MemoryRovol-EULA-1.0`），**不接受外部源码贡献**。该
submodule 以 `update = none` 装配，取得源码不等于取得授权；商业合作或授权请联系
**business@spharx.cn**。

## 3. 版本策略

策略件的版本号不自行声明，一律从 agentrt 管理仓的 `VERSION` 文件读取
（`AGENTRT_ROOT/VERSION`），保证全平台单一版本 SSoT。当前版本 `0.1.19`。

## 4. 许可与署名

`cupolas`、`lang_gateway`、`cognition`、`gateway_biz` 与本管理仓同为
**AGPL-3.0-or-later OR Apache-2.0** 双许可。每个源文件必须保留 SPDX 头；每个
提交必须包含 `Signed-off-by:`（DCO）。`memoryrovol` 不受此许可覆盖。

## 5. 提交信息

遵循内核风格：`subsystem: summary` + 正文（why > how > impact）+ tags，标题
祈使句、无句号，正文每行 ≤75 列，`Fixes:` / `Closes:` / `Link:` 标签不拆行。

---

© 2025-2026 SPHARX Ltd. 保留所有权利。
