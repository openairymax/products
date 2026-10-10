# cognition — Cognitive Strategy Payloads

> The cognition policy payloads of the AgentRT runtime: dispatch, planning,
> coordination, review, and intent strategies — pluggable policy, swapped
> without touching the mechanism core.

**Language:** English | [简体中文](README_zh.md)

[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## Overview

**cognition** is a **policy payload library** of the Airymax AI Agent
Runtime platform (`agentrt`). It carries the *strategy* half of the runtime
cognition stage, migrated out of the mechanism core under the
**mechanism / policy separation** program (0.1.19, M5-4): the mechanism core
(`atoms/coreloopthree`) keeps only the contract data types and the ops
dispatch surface (`cognition.h`), while this library implements the actual
strategies and is injected at daemon startup.

The kernel never links this library and ships no default strategy.
Consumers create strategy instances through this library's factories and
inject them via `airy_cognition_set_dispatching_strategy()` or the
`disp_strategy` parameter of `airy_cognition_create*_take()` (ownership
transferred). A missing cognition payload leaves the mechanism core
functional with degraded, built-in behavior.

## Capabilities

| Domain | Strategies | Entry |
|--------|-----------|-------|
| **Dispatch** | weighted / round-robin / priority / ML-based sub-agent dispatch | `dispatch_strategy.h` |
| **Cognitive Parallel Review (CPR)** | multi-sub-agent parallel review (cognitive confirmation + issue / boundary / coverage review); independent LLM calls, intent-weighted voting, risk merge; degrades serially on thread-creation failure | `cog_review_strategy.h` |
| **GRAD** | plan generation + critique loop (LLM-backed, staged) | `grad_strategy.h` |
| **Planner** | reactive / hierarchical / reflective / ML-based planning | `plan_strategy.h` |
| **Intent** | intent classification, entity extraction, rule engine | `intent/` |
| **Coordination** | dual-track coordination, arbitration, adapter | `coord_strategy.h` |
| **GCCP** | heuristic scoring for candidate pruning | `gccp_strategy.h` |
| **Metacognition** | metacognition + thinking-chain foundations (`foundation/mc`, `foundation/tc`) | `foundation/` |
| **Evolution** | cognitive evolution extension point | `cognitive_evolution.h` |
| **Registry** | payload registry binding strategies to injection points | `payload_registry.h` |

All LLM-backed strategies degrade automatically when the LLM service is
unavailable — opinions and decisions are never lost, only the quality tier
drops.

## Build

Dual-mode build contract (single `CMakeLists.txt`):

- **embedded** — when added via `add_subdirectory` from the agentrt root,
  dependency detection, platform macros, compliance injection and the
  `commons` targets are all provided by the root; the embedded form stays
  byte-identical.
- **standalone** — `cmake -S products/cognition -B <build-dir>`; reuses the
  agentrt management repo's CMake modules and `commons` atoms via
  `AGENTRT_ROOT` (cache variable → environment variable → relative default).

Build target: static library `airy_cognition_strategy`.

## Directory Structure

```
cognition/
├── CMakeLists.txt              # Dual-mode build (embedded | standalone)
├── README.md / README_zh.md    # This file (English / Chinese)
├── LICENSE                     # AGPL-3.0-or-later OR Apache-2.0
├── include/                    # Public headers (strategy contracts + factories)
│   ├── dispatch_strategy.h     #   Dispatch strategy object contract
│   ├── cog_review_strategy.h   #   Cognitive parallel review (CPR)
│   ├── grad_strategy.h         #   GRAD plan + critique
│   ├── plan_strategy.h         #   Planner strategies
│   ├── coord_strategy.h        #   Coordination / arbitration
│   ├── gccp_strategy.h         #   Heuristic scoring
│   ├── cognitive_evolution.h   #   Evolution extension
│   └── payload_registry.h      #   Payload registry
├── src/
│   ├── cog_review.c            # CPR: parallel review workers + vote merge
│   ├── coord_*.c               # Coordination adapter / arbiter / dual
│   ├── grad_*.c                # GRAD plan / verify / LLM calls
│   ├── weighted.c / round_robin.c / priority.c / ml_*.c   # Dispatch set
│   ├── intent/                 # Intent classifier, entity extractor, rules
│   ├── planner/                # reactive / hierarchical / reflective / ML
│   ├── foundation/             # metacognition + thinking chain (mc / tc)
│   └── ext/cognitive_evolution.c
└── tests/
    ├── unit/                   # unit tests (strategies, GRAD, threads, e2e)
    └── standalone/             # Standalone-mode entry
```

## Concurrency & Safety

CPR workers run concurrently on the corekern scheduler thread SSoT
(`airy_thread_create/join`); each completion goes through a
mechanism-injected closure with atomically-updated adapter statistics —
no data races. Public APIs are thread-safe (covered by
`test_thread_safety*.c`).

## Module Classification

**Class C — Policy payload.** Depends on the mechanism core only through
the `cognition.h` contract surface and the `commons` atoms; never the
reverse. This library is replaceable, upgradable, and droppable without any
change to the kernel.
