# canary — 金丝雀自演化门控

> 一个变更之所以可信，不是因为门禁绿过一次，而是因为它熬过了影子期、
> 分级放量和一次真实生效的回滚。本库就是执行这句话的机制。

**语言：** [English](README.md) | 简体中文

---

## 定位

**canary** 是第 20 个能力格（`dpolicy_canary_ops_t`，演化面）的机制核。
它不含策略：阈值、灰度比例与门禁线都在宿主侧。宿主把 `gate` / `route`
两个回调挂到策略引擎上；挂载点缺席时引擎整体旁路本库继续运行。

| 项 | 值 |
|---|---|
| 能力格 | `dpolicy_canary_ops_t`（由宿主定义，`cupolas/include/dynamic_policy_engine.h`） |
| 形态面 | 演化面 |
| 依赖 | C11 + libc + `<stdatomic.h>`，零第三方 |
| 规模 | `src/` 815 行，`include/` 169 行，`tests/` 525 行 |
| 失效形态 | 挂载点缺席即旁路；kill switch 即停止一切演化写入 |

## 状态机

```
C0 提案 ──▶ C1 影子 ──▶ C2 灰度 ──▶ C3 采纳
                          │            │
                          └────────────┴──▶ C4 回滚
```

`C2` 分三级放量（10 % → 50 % → 100 %），每级驻留不少于两小时
（`CANARY_DWELL_MS`）。`C3` 写本地 overlay manifest 并记录 base
SHA-256；`C4` 只恢复上一份 manifest，绝不改动 base 树。

## 爆炸半径守卫

单次演化限 **1 模块 / 3 文件 / 200 行**（`CANARY_MAX_FILES`、
`CANARY_MAX_LINES`）。没有 diff 来源的提案须申报 `0`，账本记为
`unmeasured`，禁止用代理量冒充。`canary_admit()` 执行 G30 准入：
新增函数 ≤150 行、修改后 ≤200 行；空度量返回 `-1`，超预算返回 `-2`。

## 门禁

| 门禁 | 语义 | 下限 |
|---|---|---|
| K1 | 正确性 | ≥99.9 %（`CANARY_OK_FLOOR`） |
| K2 | 性能保持 | ≥95 %（`CANARY_PERF_FLOOR`） |
| K3 | 观察窗崩溃数 | 必须为 0 |
| K4 | 回滚实测耗时 | ≤300 s（`CANARY_RB_MAX_SECS`） |
| K5 / K6 | 可解释 / 边界标记 | 必须置位 |

无观测样本的门禁返回 `CANARY_K_UNKNOWN`，而不是给一个及格分：
未知既不推进也不回滚。

## Kill Switch

四条独立通道，任一命中即停止一切演化写入：

1. 环境变量（`canary_ks_env`）
2. 全局哨兵文件（`canary_ks_file`）
3. 实例哨兵文件，随实例落在 overlay 根旁
4. CLI 指令（`canary_ks_cli`），同时持久化哨兵，重启后仍然生效

探测结果缓存一秒（`canary_ks_hit`），宿主可短周期轮询而不必每请求
访问文件系统。

## 挂载

宿主（`cupolas/daemon/src/svc_canary.c`）构造 `dpolicy_canary_ops_t`
并调用 `dpolicy_set_canary_ops()`。`gate` 回调裁决 `ACTIVATE`；
灰度会话进行中所有 `COMMIT` 一律拒绝，避免暂存变更绕过放量。
`route` 回调在热路径采样，必须在锁外多线程安全。

## 构建

嵌入态（默认）：`agentrt` 根在 `BUILD_CANARY=ON` 时纳入本目录
（`OFF` 时必须仍能整体编译——旁路路径是承重结构，不是可选项）。

独立构建：

```
cmake -S products/canary -B <出树构建目录> -DAGENTRT_ROOT=<agentrt>
cmake --build <出树构建目录>
ctest --test-dir <出树构建目录> --output-on-failure
```

源码内构建与落在源码树内的构建目录，在配置阶段即被拒绝。

## 测试

`tests/unit/test_canary.c` 编译为单个 `canary_unit` 二进制，八个用例：
SHA-256 向量、提案准入与门禁判定、manifest 申报、全状态流转、回滚、
kill switch 四通道（含 `fork()` 证明哨兵跨进程生效）、驻留锚定、
路由采样。测试在临时根下走真实文件系统 IO，禁用桩。

## 许可

AGPL-3.0-or-later OR Apache-2.0。
