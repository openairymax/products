# lang_gateway — Inference Language Gateway

> The user's natural language never reaches the model raw: it is first
> normalized into the internal Canonical Data Format, then routed — an
> inference-language lifecycle manager of the AgentRT runtime.

**Language:** English | [简体中文](README_zh.md)

[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## Overview

**lang_gateway** is an ecosystem-layer **policy payload** of the Airymax AI
Agent Runtime platform (`agentrt`), migrated out of the mechanism core
under the **mechanism / policy separation** program (0.1.19, M5-2): the
data-type contract and the ops injection table (`airy_lang_gw_ops.h`) live
in the mechanism core, while this library implements the lifecycle stages.
Mechanism-core consumers dispatch through `are_ops_get_lang_gw()` and never
include this library's headers directly.

## Lifecycle: Three Phases

```
input ──▶ Phase 1 ──▶ model channel ──▶ Phase 3 ──▶ output
              │             ▲
              └── Phase 2 ──┘
```

| Phase | Stage | What it does |
|-------|-------|--------------|
| **1 — pre-decision** | `signal_extractor` | Extracts language / task / context / code-content signals from user input |
| | `lang_router` | Multi-factor routing decision: hard rules → model native capability → input-language alignment → cost-efficiency threshold |
| **2 — in-flight** | `system_prompt` | Injects language-constraint system prompt; input transforms (placeholder protection / history rewrite) |
| **3 — post-output** | `output_post_processor` | Language-drift detection, terminology consistency, free-translation polish |

User input is normalized into the **Canonical Data Format**
(`canonical.h`) before entering the model channel.

## Model Profile & Calibration

Model profiles (`ModelProfile`) are produced by the tokenizer-feature
calibration module (`calibrator`):

- Calibration runs on a **background worker** — the first request is never
  blocked.
- Auto-recalibration of `lang_ratio` (Chinese tokens / English tokens)
  every N instructions (default 100).
- Calibration goes through llm_d's `count_tokens` RPC; when llm_d is
  unavailable the gateway **degrades to heuristic decisions** — the main
  path is never affected.

## Build

Build target: static library `airy_lang_gateway` (single
`CMakeLists.txt`, mirrors the products dual-mode contract).

## Directory Structure

```
lang_gateway/
├── CMakeLists.txt                # Build config (static lib airy_lang_gateway)
├── README.md / README_zh.md      # This file (English / Chinese)
├── LICENSE                       # AGPL-3.0-or-later OR Apache-2.0
├── include/
│   └── lang_gateway.h            # Public API (library-direct; core consumers
│                                 #   go through are_ops_get_lang_gw())
├── src/
│   ├── lang_gateway.c            # Main controller: 3-phase orchestration,
│   │                             #   lifecycle, statistics
│   ├── signal_extractor.c        # Phase 1: signal extraction
│   ├── lang_router.c             # Phase 1: multi-factor routing
│   ├── system_prompt.c           # Phase 2: language constraint injection
│   ├── output_post_processor.c   # Phase 3: drift / terminology / polish
│   ├── calibrator.c              # ModelProfile tokenizer calibration
│   └── canonical.h               # Canonical Data Format
└── tests/unit/
    └── test_lang_gateway.c
```

## Design Notes

- **Modular by stage**: each phase is an independent source file; strategy
  parameters are configuration-adjustable — policy upgrades never change
  the public API.
- **Fail-open to heuristics**: every dependency on llm_d has a heuristic
  fallback; the gateway is usable with zero backend support.
- **Contract boundary**: `airy_lang_gw_ops.h` (mechanism core) is the only
  coupling point; this library is replaceable without kernel changes.
