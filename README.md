# Airymax Products — Policy Module Aggregation

> Policy-module aggregation repository for the Airymax AI Agent Runtime
> Platform. Collects the platform's policy-type modules — the commercial
> MemoryRovol memory provider, the `cupolas` security dome, and the
> inference language gateway — under a single management surface, keeping
> policy payloads out of the mechanism core.

**Language:** English | [简体中文](README_zh.md)

[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## Overview

The **Products management repository** (`products/`) is the **policy-module
aggregation path** of the Airymax platform. It aggregates the modules that
carry *policy* — the replaceable, policy-shaped payloads that the AgentRT
mechanism core loads or dispatches to — and keeps them physically separate
from the mechanism kernel:

- **memoryrovol** — the commercial L3/L4 memory provider (closed-source).
- **cupolas** — the security dome: sandbox isolation, RBAC/ABAC permission
  adjudication, input sanitization, audit tracing, and the dynamic policy
  engine (Policy Decision Point).
- **lang_gateway** — the inference language gateway: inference-language
  lifecycle management (signal extraction, multi-factor model routing,
  system-prompt language constraint, output post-processing).

This repository follows the platform's **mechanism / policy separation**
principle: the mechanism kernel lives in
[`agentrt/`](../agentrt) (atoms · commons · daemons · gateway · heapstore ·
protocols · tools), while the policy-shaped modules live here and are plugged
in through stable contracts (`ops` dispatch tables, weak-symbol bridges,
submodule pins). A policy module can be swapped, upgraded or dropped without
touching the kernel; the kernel keeps a built-in fallback so it stays
functional when a policy module is absent.

Unlike the developer surface in [`sdk/`](../sdk) — which is a pure leaf
aggregation — `products/` mixes two aggregation styles on purpose:

- **submodule leaves** (`memoryrovol`, `cupolas`) — each is an independent
  repository pinned to an exact commit.
- **directly-tracked modules** (`lang_gateway`) — a module whose source is
  managed by this repository itself, with no independent leaf repository yet.

## Repository Structure

```
products/                       # this management repository (AGPL v3 + Apache 2.0)
├── memoryrovol/                # git submodule → openairymax/memoryrovol  (CLOSED-SOURCE)
│                               #   Commercial L3/L4 memory provider (SPHARX EULA)
├── cupolas/                    # git submodule → openairymax/cupolas
│                               #   Security dome + dynamic policy engine (PDP)
├── lang_gateway/               # directly-tracked policy module (no leaf repo yet)
│                               #   Inference language gateway (provider-facing)
├── .gitmodules                 # submodule wiring (2 entries)
├── .gitignore                  # whitelist: files + submodule mount points
├── LICENSE                     # AGPL v3 + Apache 2.0 (management repo only)
├── NOTICE                      # copyright, trademarks, third-party notices
├── README.md                   # this file (English)
└── README_zh.md                # Simplified Chinese version
```

## Policy Modules

| Module | Kind | Location | License | Description |
|--------|------|----------|---------|-------------|
| **memoryrovol** | submodule | `memoryrovol/` | SPHARX EULA v1.0 (proprietary) | Commercial closed-source memory provider implementing the L3 (Structure) and L4 (Pattern) memory layers. |
| **cupolas** | submodule | `cupolas/` | AGPL v3 + Apache 2.0 | Application-semantic security layer: four-layer inherent security plus the dynamic policy engine (Policy Decision Point). |
| **lang_gateway** | direct | `lang_gateway/` | AGPL v3 + Apache 2.0 | Inference language gateway (`airy_lang_gateway`) — canonicalizes natural-language input and routes it to the right model. |

> **Licensing note.** `cupolas` and `lang_gateway` use the same AGPL v3 +
> Apache 2.0 dual license as the rest of the Airymax platform
> (SPDX: `AGPL-3.0-or-later OR Apache-2.0`). `memoryrovol` is **not**
> open-source: it is governed by the SPHARX Commercial EULA v1.0
> (SPDX: `LicenseRef-SPHARX-MemoryRovol-EULA-1.0`) and its use requires a
> License Key tied to an Authorization Tier.

## Policy Architecture

Policy modules sit **above** the mechanism kernel and depend on it — never
the other way around. The kernel exposes stable dispatch contracts; each
policy module implements one behind that contract, and every module has a
kernel-side fallback so an absent module degrades rather than breaks.

```
                  ┌──────────────────────────────────────────────┐
                  │  Mechanism kernel (agentrt/)                  │
                  │  atoms · commons · daemons · gateway          │
                  │  heapstore · protocols · tools                │
                  └───────────────────┬──────────────────────────┘
                                      │  stable contracts:
                                      │  ops dispatch · weak symbols · submodule pin
        ┌─────────────────────────────┼─────────────────────────────┐
        │                             │                             │
        ▼                             ▼                             ▼
┌──────────────────┐       ┌──────────────────┐       ┌──────────────────────┐
│   memoryrovol    │       │     cupolas      │       │    lang_gateway      │
│ L3 Structure     │       │ sandbox / RBAC   │       │ inference-language   │
│ L4 Pattern       │       │ sanitize / audit │       │ canonicalize+route   │
│ (closed-source,  │       │ + policy engine  │       │ (ops-dispatched,     │
│  SPHARX EULA)    │       │ (PDP)            │       │  kernel fallback)    │
└────────┬─────────┘       └────────┬─────────┘       └──────────┬───────────┘
         │                          │                            │
         ▼                          ▼                            ▼
   AgentRT runtime           gateway / daemons            CoreLoopThree
   (AIRY_WITH_MEMORYROVOL,   (call cupolas at every       (are_ops_get_lang_gw)
    weak-symbol bridge)       security boundary)           when the policy is present
```

### memoryrovol — Commercial Memory Provider

MemoryRovol implements the **L3 (Structure)** and **L4 (Pattern)** layers of
the Airymax four-layer memory architecture as a precompiled, license-gated C
static library (`.a` / `.lib`) that is linked into the AgentRT runtime at
build time. When the runtime loads, the CoreLoopThree bridge resolves
MemoryRovol through a **weak-symbol** contract: if MemoryRovol is linked, the
bridge routes memory calls to it; if not, the bridge transparently falls back
to the runtime's built-in open-source provider (which covers only L1 and L2).

- **Upstream**: `atoms/memory/memoryrovol/` bridge API (weak symbols).
- **Downstream**: AgentRT runtime (the `AIRY_WITH_MEMORYROVOL` CMake option,
  enabled by default; backend selected via
  `AIRY_MEMORY_BACKEND=builtin|memoryrovol`).
- **License**: SPHARX Commercial EULA v1.0 — see
  [§ MemoryRovol Licensing](#memoryrovol-licensing).

### cupolas — Security Dome & Policy Engine

cupolas is the **application-semantic security layer**: every agent action
must pass through the dome before it reaches the kernel. It implements a
four-layer endogenous-security model — sandbox isolation, RBAC/ABAC
permission adjudication, input sanitization, and audit tracing — and builds a
single static library `airy_cupolas`. On top of the four layers it acts as the
**Policy Decision Point (PDP)**: the dynamic policy engine loads, validates,
conflicts-checks, activates and rolls back versioned JSON rule sets, which are
then distributed to daemons and enforced locally by each daemon's **Policy
Enforcement Point (PEP)** through a local cache.

- **Upstream**: `commons` (sync, error framework, types, memory macros); Linux
  kernel primitives (Landlock, seccomp BPF) for the sandbox.
- **Downstream**: `gateway` and `daemons` — invoked at every security boundary
  (request authentication, input sanitization, permission checks, audit
  emission).
- **License**: AGPL v3 + Apache 2.0.

### lang_gateway — Inference Language Gateway

`lang_gateway` manages the full lifecycle of inference language. User input
never reaches the model directly: the gateway canonicalizes it into an
internal Canonical Data Format and routes it to the right model across three
phases — Phase 1 (pre-decision: signal extraction + multi-factor routing),
Phase 2 (inference: system-prompt language constraint + input transformation),
and Phase 3 (post-inference: language-drift detection + terminology
consistency + paraphrase polish). A per-model profile
(`lang_ratio = zh_tokens / en_tokens`) is calibrated asynchronously through
`llm_d`'s `count_tokens` RPC; if `llm_d` is unavailable the gateway degrades
to heuristic routing without blocking the main path.

- **Contract**: the data-type contract and the ops injection table live in the
  mechanism kernel (`airy_lang_gw_ops.h`); kernel consumers dispatch through
  `are_ops_get_lang_gw()` and never depend on this module's headers directly.
- **Upstream**: `airy_rt.h` and the kernel-side `airy_lang_gw_ops.h` contract.
- **Downstream**: CoreLoopThree (the mechanism core) when the policy is
  present; the kernel degrades gracefully when it is not.
- **License**: AGPL v3 + Apache 2.0.

### Memory Stratification

```
L1 Raw Layer       ← Built-in (open-source, AGPL v3 + Apache 2.0)
L2 Feature Layer   ← Built-in (open-source, AGPL v3 + Apache 2.0)
L3 Structure Layer ← MemoryRovol (commercial, SPHARX EULA)
L4 Pattern Layer   ← MemoryRovol (commercial, SPHARX EULA)
```

## MemoryRovol Licensing

`memoryrovol` is the only closed-source component in the Airymax platform.
It is governed by the **SPHARX Commercial EULA v1.0**
(SPDX: `LicenseRef-SPHARX-MemoryRovol-EULA-1.0`), authored and owned by
**SPHARX Ltd.** The repository is hosted under the
[openairymax](https://atomgit.com/openairymax) organization on AtomGit for
distribution and issue tracking; hosting location does not change its
proprietary licensing.

### Authorization Tiers

Use of MemoryRovol requires a **License Key** tied to one of four
Authorization Tiers. The tier determines feature scope, capacity ceilings
and support level:

| Tier | Audience | Typical Scope |
|------|----------|---------------|
| **Trial** | Evaluators, individual developers | Time / capacity-limited evaluation of L3 + L4 features for integration testing. |
| **Pro** | Professional users, small teams | Full L3 (Structure) and L4 (Pattern) capabilities for personal or small-team production use. |
| **Enterprise** | Mid-to-large organizations | Higher capacity ceilings, multi-seat deployment, priority commercial support. |
| **Enterprise+** | Large enterprises / OEMs | Highest capacity, OEM redistribution rights, dedicated support and SLA. |

Tier enforcement is performed by an **RSA-4096 offline License Key
verification** mechanism built into the MemoryRovol library — no phone-home,
no telemetry, fully air-gapped operation is supported. The full terms and
conditions are in the [`LICENSE`](memoryrovol/LICENSE) file inside the
`memoryrovol` submodule; the open-source AGPL v3 + Apache 2.0 license of
the surrounding management repository **does not apply** to that submodule.

## Build & Integration

### Prerequisites

- Git ≥ 2.30 (with submodule support).
- For `memoryrovol` (only when linking into AgentRT): CMake 3.20+, a
  C11 compiler, and a valid MemoryRovol License Key.
- For `cupolas` and `lang_gateway`: a C11 compiler; the modules are consumed
  by the AgentRT build as CMake targets (`airy_cupolas`,
  `airy_lang_gateway`).

### Clone with submodules

```bash
# Clone the management repo and its leaf submodules
git clone --recurse-submodules https://atomgit.com/openairymax/products.git

# If already cloned without --recurse-submodules:
cd products
git submodule update --init --recursive
```

> **Note**: the `memoryrovol` submodule is wired with `update = none`; cloning
> recursively will not fetch it automatically. Acquiring the source does
> **not** grant a License Key either — you still need one from SPHARX Ltd. to
> link and use MemoryRovol in production.

### Integrate MemoryRovol into the AgentRT runtime (commercial)

MemoryRovol is consumed at AgentRT build time through the
`AIRY_WITH_MEMORYROVOL` CMake option (enabled by default) together with the
`AIRY_MEMORY_BACKEND` backend selector. This requires the `memoryrovol`
sources available and, for actual use, a valid License Key provisioned at
runtime via a license file (see `memoryrovol/README.md` for the licensing
and integration details).

```bash
# Configure the AgentRT runtime with MemoryRovol support
cmake -S agentrt -B build-agentrt \
    -DAIRY_WITH_MEMORYROVOL=ON \
    -DAIRY_MEMORY_BACKEND=memoryrovol
cmake --build build-agentrt -j
```

Switching `AIRY_MEMORY_BACKEND` back to `builtin` (the default) makes the
runtime transparently fall back to the open-source L1/L2 built-in provider
— no source changes are required downstream.

> **No in-tree build.** Per the Airymax engineering discipline, the
> `airymaxhub` source tree is never built in place. `products/` carries only
> policy-module sources and wiring; all compilation happens in an external
> build tree (see `docs/AirymaxRT/50-engineering-standards`).

## Branch Strategy

- **This management repository**: `main` only. All release tags are cut on
  `main`.
- **Leaf repositories** (`memoryrovol`, `cupolas`): their integration branch is
  `dev/hubs-01`; `main` is the release snapshot. Submodules are pinned to
  exact commits so that every checkout of this repository is reproducible.
- **Directly-tracked modules** (`lang_gateway`): versioned in lock-step with
  this repository.

## License

The **management repository itself** (`products/`, i.e. this README, the
`.gitmodules` wiring, the top-level `LICENSE` / `NOTICE` files) is
dual-licensed under:

1. **GNU Affero General Public License v3.0 or later (AGPL v3)**
   — https://www.gnu.org/licenses/agpl-3.0.html
2. **Apache License, Version 2.0**
   — https://www.apache.org/licenses/LICENSE-2.0

SPDX: `AGPL-3.0-or-later OR Apache-2.0`. You may choose either license at
your option. The full text of both licenses is included in the
[`LICENSE`](LICENSE) file at the root of this management repository.

### Dual License Guide

You may choose **either** license at your option — not both, not neither.

**SPDX Expression**: `AGPL-3.0-or-later OR Apache-2.0`

| If you are... | Choose | Why |
|---------------|--------|-----|
| Building a **SaaS** or network service that modifies these policy modules | **AGPL v3** | Network service clause requires source disclosure |
| Developing **open-source** derivatives (copyleft) | **AGPL v3** | Derivatives must remain open-source under AGPL |
| Using the policy modules in **commercial closed-source** deployments | **Apache 2.0** | Permissive, allows proprietary derivatives |
| Building **enterprise internal tools** | **Apache 2.0** | No source disclosure required |
| Needing **patent protection** | **Apache 2.0** | Explicit patent grant from contributors |
| Just learning or researching | **Either** | Both permit personal use |

> **Note**: This dual-license guide applies to the **management repository**
> and the `cupolas/` / `lang_gateway/` modules. The `memoryrovol/` submodule
> is **NOT** covered by this guide — it is governed by the SPHARX Commercial
> EULA v1.0 and requires a separate License Key. See
> [§ Per-module licensing](#per-module-licensing) below.

### Per-module licensing

Each policy module carries **its own** license in its own `LICENSE` file;
the management-repository license above does not override them:

| Module | License | SPDX |
|--------|---------|------|
| `cupolas/` | AGPL v3 + Apache 2.0 (dual) | `AGPL-3.0-or-later OR Apache-2.0` |
| `lang_gateway/` | AGPL v3 + Apache 2.0 (dual) | `AGPL-3.0-or-later OR Apache-2.0` |
| `memoryrovol/` | **SPHARX Commercial EULA v1.0 (proprietary, closed-source)** | `LicenseRef-SPHARX-MemoryRovol-EULA-1.0` |

In particular, **`memoryrovol` is NOT open source** and is not covered by
either the AGPL v3 or the Apache 2.0 license. Use, redistribution or
linking of `memoryrovol` requires acceptance of the SPHARX Commercial
EULA v1.0 and a valid License Key for the applicable Authorization Tier
(Trial / Pro / Enterprise / Enterprise+). See
[§ MemoryRovol Licensing](#memoryrovol-licensing) and the
[`memoryrovol/LICENSE`](memoryrovol/LICENSE) file for the full terms.

---

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.
