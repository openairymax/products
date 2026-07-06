# Airymax Products — Desktop, Docker & Commercial Memory

> Product packaging management repository for the Airymax AI Agent Runtime
> Platform. Aggregates three leaf repositories — the desktop application, the
> Docker deployment images, and the commercial MemoryRovol memory provider —
> into a single deliverable surface for end users, operators and enterprise
> customers.
>
> One of five management repositories under the
> [airymaxhub](https://atomgit.com/openairymax/airymaxhub) umbrella (Airymax
> 0.1.1 release: 1 umbrella + 5 management + 29 leaf + 3 top-level = 38
> repositories).

**Language:** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.1-5a6b7e)](https://atomgit.com/openairymax/products)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## Overview

The **Products management repository** (`products/`) is the product packaging
layer of the Airymax platform. It does not contain first-party source code of
its own; instead it aggregates three independent leaf repositories as git
submodules and presents them as the installable / deployable surface of the
platform:

- **desktop** — the cross-platform personal client (Tauri v2 + React 18).
- **docker** — the official containerized deployment images and Compose stacks.
- **memoryrovol** — the commercial L3/L4 memory provider (closed-source).

### Mixed-License Notice

Two of the three leaf repositories (`desktop` and `docker`) ship under the
**AGPL v3 + Apache 2.0** dual license that governs the rest of the Airymax
platform. The third one, `memoryrovol`, is a **closed-source commercial
product** governed by the **SPHARX Commercial EULA v1.0**
(`LicenseRef-SPHARX-MemoryRovol-EULA-1.0`), and lives under the **`spharx`**
organization rather than `openairymax`. The management repository itself
(this `products/` repo) is dual-licensed AGPL v3 + Apache 2.0, but cloning it
recursively will pull `memoryrovol` whose use requires a valid License Key.
See [§ MemoryRovol Licensing](#memoryrovol-licensing) and
[§ License](#license) for the exact terms.

## Repository Structure

```
products/                       # this management repository (AGPL v3 + Apache 2.0)
├── desktop/                    # git submodule → openairymax/desktop
│                               #   Airymax Desktop GUI (Tauri v2 + React 18)
├── docker/                     # git submodule → openairymax/docker
│                               #   Docker images & docker-compose stacks
├── memoryrovol/                # git submodule → spharx/memoryrovol  (CLOSED-SOURCE)
│                               #   Commercial L3/L4 memory provider (SPHARX EULA)
├── .gitmodules                 # submodule wiring (3 entries)
├── .gitignore
├── LICENSE                     # AGPL v3 + Apache 2.0 (management repo only)
├── NOTICE                      # copyright, trademarks, third-party notices
├── README.md                   # this file (English)
└── README_zh.md                # Simplified Chinese version
```

## Leaf Repositories

| Module | Repository URL | License | Description |
|--------|----------------|---------|-------------|
| **desktop** | `git@atomgit.com:openairymax/desktop.git` | AGPL v3 + Apache 2.0 | Cross-platform desktop application (Airymax Desktop GUI) — personal client built on Tauri v2 / React 18 / TypeScript / Vite. |
| **docker** | `git@atomgit.com:openairymax/docker.git` | AGPL v3 + Apache 2.0 | Official Docker images and Docker Compose stacks for development, staging and production deployment of the AgentRT runtime. |
| **memoryrovol** | `git@atomgit.com:spharx/memoryrovol.git` | SPHARX EULA v1.0 (proprietary) | Commercial closed-source memory provider implementing the L3 (Structure) and L4 (Pattern) memory layers. Belongs to the `spharx` organization, not `openairymax`. |

> **Licensing note.** `desktop` and `docker` use the same AGPL v3 + Apache 2.0
> dual license as the rest of the Airymax platform
> (SPDX: `AGPL-3.0-or-later OR Apache-2.0`). `memoryrovol` is **not**
> open-source: it is governed by the SPHARX Commercial EULA v1.0
> (SPDX: `LicenseRef-SPHARX-MemoryRovol-EULA-1.0`) and its use requires a
> License Key tied to an Authorization Tier.

## Product Architecture

The three leaf modules under `products/` form the **deliverable surface** of
the Airymax platform — what an end user installs, an operator deploys, or an
enterprise integrates. They are consumed downstream of the runtime / SDK /
ecosystem layers and never produce code that flows back upstream.

```
                  ┌──────────────────────────────────────┐
                  │  Upstream source layers              │
                  │  sdk/  ·  agentrt/  ·  ecosystem/    │
                  │  atoms/memoryrovol/  (bridge API)    │
                  └────────────────┬─────────────────────┘
                                   │
   ┌───────────────────────────────┼───────────────────────────────┐
   │                               │                               │
   ▼                               ▼                               ▼
┌──────────────┐         ┌──────────────────┐         ┌──────────────────────┐
│   desktop    │◀───────▶│      docker      │◀─link───│     memoryrovol      │
│ personal GUI │  optional│   OCI images +   │         │ L3 Structure Layer  │
│ Tauri v2 +   │ gateway  │ docker-compose   │         │ L4 Pattern Layer    │
│ React 18     │ backend  │ dev/staging/prod │         │ (closed-source,     │
│ Win/macOS/Lnx│         │                  │         │  SPHARX EULA)       │
└──────┬───────┘         └────────┬─────────┘         └──────────┬───────────┘
       │                          │                              │
       ▼                          ▼                              ▼
   End users              Operators / enterprise           AgentRT runtime
   (personal)             deployments                     (via AGENTRT_WITH_
                                                          MEMORYROVOL=ON)
```

### desktop — Personal Client

The desktop application packages the runtime, SDK and ecosystem capabilities
into a single installable GUI built on **Tauri v2** (Rust core) with a
**React 18 + TypeScript + Vite** frontend. It targets Windows, macOS and
Linux, supports offline-first PWA behaviour, system tray integration,
global shortcuts and a built-in connection to a locally running AgentRT
gateway.

- **Upstream**: `sdk/agentrt/` (runtime + SDK gateway HTTP / WebSocket API),
  `products/docker/` (optional companion gateway backend).
- **Downstream**: end users (personal users) who install the produced
  `.exe` / `.dmg` / `.deb` / `.AppImage` artifacts.

### docker — Containerized Deployment

The Docker module provides reproducible, security-hardened OCI images for
the AgentRT runtime, daemon services, gateway, OpenLab and the desktop web
frontend, plus three Docker Compose manifests (development / staging /
production) wired with PostgreSQL, Redis and a Prometheus + Grafana +
AlertManager observability stack.

- **Images**: `Dockerfile.kernel`, `Dockerfile.daemon`,
  `Dockerfile.openlab`, `Dockerfile.desktop`.
- **Stacks**: `docker-compose.yml` (dev), `docker-compose.staging.yml`,
  `docker-compose.prod.yml`.
- **Upstream**: `agentrt/` runtime source tree, `ecosystem/manager/`
  configuration defaults, `products/desktop/` source consumed by
  `Dockerfile.desktop`.
- **Downstream**: DevOps / SRE / enterprise operators running the produced
  images in dev, staging or production.

### memoryrovol — Commercial Memory Provider

MemoryRovol implements the **L3 (Structure)** and **L4 (Pattern)** layers
of the Airymax four-layer memory architecture as a precompiled,
license-gated C static library (`.a` / `.lib`) that is linked into the
AgentRT runtime at build time. When the runtime loads, the CoreLoopThree
bridge resolves MemoryRovol through a **weak-symbol** contract: if
MemoryRovol is linked, the bridge routes memory calls to it; if not, the
bridge transparently falls back to the runtime's built-in open-source
provider (which covers only L1 and L2).

- **Upstream**: `atoms/memoryrovol/` bridge API (weak symbols).
- **Downstream**: AgentRT runtime (via the `AGENTRT_WITH_MEMORYROVOL=ON`
  CMake option).
- **License**: SPHARX Commercial EULA v1.0 — see
  [§ MemoryRovol Licensing](#memoryrovol-licensing).

### Memory Stratification

```
L1 Raw Layer       ← Built-in (open-source, AGPL v3 + Apache 2.0)
L2 Feature Layer   ← Built-in (open-source, AGPL v3 + Apache 2.0)
L3 Structure Layer ← MemoryRovol (commercial, SPHARX EULA)
L4 Pattern Layer   ← MemoryRovol (commercial, SPHARX EULA)
```

## MemoryRovol Licensing

`memoryrovol` is the only closed-source component in the entire Airymax 0.1.1
release. It is governed by the **SPHARX Commercial EULA v1.0**
(SPDX: `LicenseRef-SPHARX-MemoryRovol-EULA-1.0`), authored and owned by
**SPHARX Ltd.**, and hosted under the **`spharx`** organization
(`git@atomgit.com:spharx/memoryrovol.git`) rather than `openairymax`.

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

## Build & Deployment

### Prerequisites

- Git ≥ 2.30 (with submodule support).
- For `desktop`: Node.js 18+, Rust (stable) toolchain, Tauri v2 system
  dependencies (WebKit / GTK / MSVC depending on OS).
- For `docker`: Docker 20.10+, Docker Compose v2.20+.
- For `memoryrovol` (only when linking into AgentRT): CMake 3.20+, a
  C11 compiler, and a valid MemoryRovol License Key.

### Clone with submodules

```bash
# Clone the management repo and all three leaf submodules
git clone --recurse-submodules git@atomgit.com:openairymax/products.git

# If already cloned without --recurse-submodules:
cd products
git submodule update --init --recursive
```

> **Note**: cloning recursively will pull `memoryrovol`. Acquiring the
> source does **not** grant a License Key; you still need to obtain one
> from SPHARX Ltd. to actually link and use MemoryRovol in production.

### Build the desktop application

The desktop module is a Tauri v2 app driven by npm scripts. See
`desktop/README.md` for full platform-specific instructions.

```bash
cd desktop

# Install frontend dependencies
npm install

# Run the Vite dev server + Tauri dev shell
npm run tauri dev

# Type-check + lint + production build of the frontend
npm run check

# Build platform-native installers (.exe / .dmg / .deb / .AppImage)
npm run tauri build
```

The produced artifacts land in `desktop/src-tauri/target/release/bundle/`.
The default gateway endpoint is `http://localhost:18789` and can be
overridden via `VITE_AGENTOS_GATEWAY_HOST` / `VITE_AGENTOS_GATEWAY_PORT`
(see `desktop/.env.example`).

### Build and run the Docker images

The docker module ships four multi-stage Dockerfiles and three Compose
stacks. See `docker/README.md` and `docker/DEPLOYMENT.md` for full details.

```bash
cd docker

# Development stack (kernel + daemon + gateway + OpenLab + desktop web,
# plus PostgreSQL + Redis + monitoring)
docker compose --env-file .env.example up -d --build

# Staging stack
docker compose -f docker-compose.staging.yml \
    --env-file .env.staging.example up -d --build

# Production stack
docker compose -f docker-compose.prod.yml \
    --env-file .env.production.example up -d --build

# Build a single image directly (e.g. the kernel image)
docker build -f Dockerfile.kernel -t airymax-kernel:0.1.1 ..
```

All images run as a non-root `agentos:1000` user with `read_only`
filesystems, `seccomp` profile, `cap_drop: ALL` and no privilege
escalation, aligned with the CIS Docker Benchmark baseline.

### Link MemoryRovol into the AgentRT runtime (commercial)

MemoryRovol is consumed at AgentRT build time through a CMake option. This
requires a valid License Key and the `memoryrovol` submodule checked out.

```bash
# Inside the agentrt/ build directory
cmake -S ../agentrt -B build \
    -DAGENTRT_WITH_MEMORYROVOL=ON \
    -DMEMORYROVOL_LICENSE_KEY=<your-license-key>
cmake --build build -j
```

When `AGENTRT_WITH_MEMORYROVOL=ON` is **off** (or no License Key is
provided), the runtime falls back transparently to the open-source L1/L2
built-in provider — no source changes are required downstream.

## Branch Strategy

- **This management repository**: `main` only. All release tags are cut on
  `main`.
- **Leaf repositories** (`desktop`, `docker`, `memoryrovol`):
  `feature/official-hubs-01` is the active development branch tracked by
  the submodules' `branch =` configuration in `.gitmodules`. The `main`
  branch of each leaf repo receives merged stabilization commits at
  release time.

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

### Per-submodule licensing

Each leaf submodule carries **its own** license in its own `LICENSE` file;
the management-repository license above does not override them:

| Submodule | License | SPDX |
|-----------|---------|------|
| `desktop/` | AGPL v3 + Apache 2.0 (dual) | `AGPL-3.0-or-later OR Apache-2.0` |
| `docker/` | AGPL v3 + Apache 2.0 (dual) | `AGPL-3.0-or-later OR Apache-2.0` |
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
