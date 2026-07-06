# Airymax Products — Packaging & Distribution

> Product packaging layer for the Airymax AI Agent Runtime Platform.
> One of four management repositories under the [airymaxhub](https://atomgit.com/openairymax/airymaxhub) umbrella.

**Language:** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.1-5a6b7e)](https://atomgit.com/openairymax/products)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## Overview

The **Products management repo** aggregates Airymax's product packaging: the desktop application, Docker deployment images, and the commercial MemoryRovol memory provider. These are the deliverable artifacts that end users install and deploy.

## Leaf Repositories

| Module | Repository | License | Description |
|--------|-----------|---------|-------------|
| **desktop** | `git@atomgit.com:openairymax/desktop.git` | AGPL v3 + Apache 2.0 | Desktop application (personal client) |
| **docker** | `git@atomgit.com:openairymax/docker.git` | AGPL v3 + Apache 2.0 | Docker deployment images & docker-compose |
| **memoryrovol** | `git@atomgit.com:spharx/memoryrovol.git` | SPHARX EULA v1.0 | Commercial memory provider (closed-source, C-class isolation layer) |

> **Note**: `memoryrovol` belongs to the `spharx` organization (not `openairymax`) and is licensed under a commercial EULA. All other repositories in the Airymax project use the AGPL v3 + Apache 2.0 dual license.

## Architecture

```
products/
├── desktop/       ← Desktop application (personal client)
├── docker/        ← Docker images & deployment configs
├── memoryrovol/   ← Commercial memory provider (SPHARX EULA)
├── .gitmodules
└── README.md      ← This file
```

### Module Details

#### desktop

The desktop application provides a personal client for interacting with the Airymax runtime. It is built on top of the SDK layer and provides a graphical interface for agent management, task submission, and result visualization.

- **Upstream**: SDK (`sdk/`), runtime (`agentrt/`)
- **Downstream**: End users (personal users)

#### docker

The Docker module provides containerized deployment for the Airymax runtime. It includes Dockerfiles, docker-compose configurations, and deployment scripts for production environments.

- **Upstream**: Runtime (`agentrt/`), ecosystem configs (`ecosystem/manager/`)
- **Downstream**: Operators, enterprise deployments

#### memoryrovol

MemoryRovol is the commercial memory provider for Airymax, implementing the L3 (Structure) and L4 (Pattern) memory layers. It is a **closed-source** product licensed under the SPHARX Commercial EULA v1.0, with authorization tiers: Trial / Pro / Enterprise / Enterprise+.

- **Upstream**: `atoms/memoryrovol/` bridge API (weak symbol)
- **Downstream**: Runtime (via `AGENTRT_WITH_MEMORYROVOL=ON` CMake option)
- **SPDX**: `LicenseRef-SPHARX-MemoryRovol-EULA-1.0`

## Memory Stratification

```
L1 Raw Layer       ← Built-in (open-source, AGPL v3 + Apache 2.0)
L2 Feature Layer   ← Built-in (open-source, AGPL v3 + Apache 2.0)
L3 Structure Layer ← MemoryRovol (commercial, SPHARX EULA)
L4 Pattern Layer   ← MemoryRovol (commercial, SPHARX EULA)
```

## Branch Strategy

- This management repo: **`main`** only
- Leaf repos: **`feature/official-hubs-01`** (active development)

## License

- **desktop** & **docker**: AGPL v3 + Apache 2.0 (SPDX: `AGPL-3.0-or-later OR Apache-2.0`)
- **memoryrovol**: SPHARX Commercial EULA v1.0 (SPDX: `LicenseRef-SPHARX-MemoryRovol-EULA-1.0`)

See individual [LICENSE](LICENSE) files in each leaf repo for details.

Copyright (c) 2025-2026 **SPHARX Ltd.** All Rights Reserved.
