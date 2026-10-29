# canary — Canary Self-Evolution Gate

> A change is not trusted because a gate went green once. It is trusted
> because it survived shadow, a staged ramp and a rollback that actually
> worked. This library is the mechanism that enforces that sentence.

**Language:** English | [简体中文](README_zh.md)

---

## Overview

**canary** is the mechanism kernel of the 20th capability slot
(`dpolicy_canary_ops_t`, evolution face). It holds no policy: thresholds,
ramp ratios and gate cut-offs belong to the host. The host mounts a
`gate` / `route` pair on the policy engine; when the mount point is absent
the engine bypasses this library entirely and keeps running.

| Property | Value |
|---|---|
| Slot | `dpolicy_canary_ops_t` (defined by the host, `cupolas/include/dynamic_policy_engine.h`) |
| Face | evolution |
| Dependencies | C11 + libc + `<stdatomic.h>` — no third party |
| Size | 815 lines `src/`, 169 lines `include/`, 525 lines `tests/` |
| Failure mode | absent mount point ⇒ bypass; kill switch ⇒ stop all evolution writes |

## State Machine

```
C0 proposed ──▶ C1 shadow ──▶ C2 canary ──▶ C3 adopted
                                  │              │
                                  └──────────────┴──▶ C4 rolled back
```

`C2` ramps in three stages (10 % → 50 % → 100 %), each stage dwelling at
least two hours (`CANARY_DWELL_MS`). `C3` writes a local overlay manifest
recording the base SHA-256; `C4` restores the previous manifest and never
touches the base tree.

## Blast Radius Guard

One evolution is bounded to **1 module / 3 files / 200 lines**
(`CANARY_MAX_FILES`, `CANARY_MAX_LINES`). A proposal with no diff source
declares `0` and is recorded as `unmeasured` — proxy measurements are not
accepted in their place. `canary_admit()` applies the G30 admission rule:
new function ≤150 lines, modified function ≤200 lines; it returns `-1` for
empty measurements and `-2` when the change exceeds the budget.

## Gates

| Gate | Meaning | Floor |
|---|---|---|
| K1 | correctness | ≥99.9 % (`CANARY_OK_FLOOR`) |
| K2 | performance retention | ≥95 % (`CANARY_PERF_FLOOR`) |
| K3 | crashes in the observation window | must be 0 |
| K4 | measured rollback time | ≤300 s (`CANARY_RB_MAX_SECS`) |
| K5 / K6 | explainability / boundary flags | must be set |

A gate with no observations returns `CANARY_K_UNKNOWN` rather than a
passing score: unknown neither promotes nor rolls back.

## Kill Switch

Four independent channels, any one of which stops all evolution writes:

1. environment variable (`canary_ks_env`)
2. global sentinel file (`canary_ks_file`)
3. per-instance sentinel, created next to the overlay root
4. CLI command (`canary_ks_cli`), which also persists the sentinel so the
   stop survives a restart

Probes are cached for one second (`canary_ks_hit`), so hosts may poll on a
short cycle without touching the filesystem per request.

## Mounting

The host (`cupolas/daemon/src/svc_canary.c`) builds a `dpolicy_canary_ops_t`
and calls `dpolicy_set_canary_ops()`. The `gate` callback gates
`ACTIVATE`; during a canary session every `COMMIT` is rejected so a
staged change cannot bypass the ramp. The `route` callback samples the
hot path and must be thread-safe — it is invoked outside the engine lock.

## Build

Embedded (default): the `agentrt` root adds this directory when
`BUILD_CANARY=ON` (`OFF` builds the tree without it and must still
compile — the bypass path is load-bearing).

Standalone:

```
cmake -S products/canary -B <out-of-tree-build> -DAGENTRT_ROOT=<agentrt>
cmake --build <out-of-tree-build>
ctest --test-dir <out-of-tree-build> --output-on-failure
```

In-source builds and build directories inside the source tree are rejected
at configure time.

## Tests

`tests/unit/test_canary.c` runs as a single `canary_unit` binary with eight
cases: SHA-256 vectors, proposal admission and gate verdicts, manifest
declaration, the full state flow, rollback, the four kill switch channels
(including a `fork()` to prove the sentinel crosses processes), dwell
anchoring, and route sampling. Tests use real filesystem I/O under a
temporary root — there are no stubs.

## License

AGPL-3.0-or-later OR Apache-2.0.
