# gateway_biz — Gateway Business Handler (Embedded Product Piece)

> The business-processing face of `gateway_d`, hosted outside the mechanism
> core as a product piece. Dispatches the unified capability surface, forwards
> namespace calls to daemons, and carries the PEP verdict cache — assembled
> into `gateway_d` at build time, never linked by any other target.

**Language:** English | [简体中文](README_zh.md)

[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)

---

## Overview

**gateway_biz** is the **business-processing face** of the gateway daemon. It
was migrated out of the mechanism core (`agentrt/gateway/src/biz/`) under the
0.1.19 volume program (ledger §340, "blade H"): its handlers depend on the
daemon common layer (`svc_common`, `daemon_security`) and on gateway protocol
adaptation headers, so they are not part of the pure gateway mechanism and do
not belong in the mechanism-kernel count.

It is an **embedded product piece**, not a standalone library:

- its only consumer is the `gateway_d` executable (see
  `agentrt/link-whitelist.txt`);
- it requires host targets (`gateway_policy_gen`, `svc_common`,
  `airy_protocols`) to exist before it can be configured;
- when the host cannot find this directory, `gateway_d` is not mounted at all
  and no stub is generated — absence degrades by dropping the daemon, never by
  fabricating an implementation.

The mechanism core keeps the transport, framing, session and protocol
mechanism; this piece keeps the *business routing* that consumes those
mechanisms.

## Capabilities

- **Unified capability dispatch.** Every capability exposed by the gateway is
  declared once in `agentrt/gateway/.manifest`; the generated registry is the
  single authoritative source (`cap_key = "<ns>.<method>"`). Unregistered
  capabilities fail closed (`-32601`), so the gateway exposes exactly what the
  manifest declares.
- **Namespace forwarding.** `<ns>.<method>` calls are forwarded to the owning
  daemon over the L2 protocol client, with the wire method name and timeout
  taken from the registry rather than from string surgery on the request.
- **Protocol backends.** MCP, OpenAI-compatible and A2A request/response
  adaptation for external clients.
- **Agent forwarding.** `agent.run` / `agent.cancel` are forwarded to
  `agent_d`; the orchestration engine itself lives in the daemon, not here.
- **Hall surface.** `hall.*` task board, event stream and decision chain are
  implemented inside the gateway process.
- **PEP verdict cache.** Adjudication results are cached under an epoch key so
  a policy change invalidates the whole cache at once, keeping the security
  hot path allocation-free on hits.
- **Unified service dispatch hook.** `SYS_SVC_CALL` is routed through the
  microkernel hook so service calls cannot bypass the syscall boundary.
- **On-demand activation.** `supervisor.activate` is delivered as a
  fire-and-forget one-way notification.
- **Built-in MCP tools.** Tool JSON schemas are registered from a single
  authoritative table.

## Build

This piece is **embedding-only**. It is added by `gateway_d` and inherits the
host toolchain, platform macros and compliance injection:

```
# configured by agentrt/daemons/gateway_d/CMakeLists.txt
AIRY_GATEWAY_BIZ_AGENTRT_ROOT   # required: agentrt source root
AIRY_GATEWAY_BIZ_GEN_DIR        # required: gateway_policy_gen output directory
```

Configuration fails loudly if either variable is unset or if a required host
target is missing. Building it in isolation is not supported by design: the
piece has no standalone dependency graph, and pretending otherwise would
require stub host targets.

## Directory Structure

```
gateway_biz/
├── include/
│   ├── gateway_business_handler.h    # public entry: dispatch + context lifecycle
│   ├── gateway_biz_internal.h        # module-private shared contract
│   ├── gateway_cap_registry.h        # capability registry query surface (SSoT)
│   ├── gateway_pep_cache.h           # PEP verdict cache
│   ├── gateway_policy.h              # policy surface consumed by handlers
│   └── gateway_biz_supactivate.h     # on-demand activation notification
├── src/
│   ├── gateway_business_handler.c    # main dispatch + context lifecycle
│   ├── gateway_biz_forward.c         # namespace forwarding (L2 client + whitelist)
│   ├── gateway_biz_agent.c           # agent.run / agent.cancel forwarding
│   ├── gateway_biz_backend.c         # MCP / OpenAI / A2A protocol backends
│   ├── gateway_biz_hall.c            # hall.* task board / event stream / chain
│   ├── gateway_biz_svcdispatch.c     # unified service dispatch hook
│   ├── gateway_biz_supactivate.c     # fire-and-forget activation
│   ├── gateway_biz_tools.c           # built-in MCP tool registration
│   ├── gateway_cap_registry.c        # cap_key lookup / namespace validation
│   └── gateway_pep_cache.c           # PEP verdict cache (epoch invalidation)
├── CMakeLists.txt
└── LICENSE
```

## Testing

Because the piece is embedding-only, its tests live with the consumer that can
supply a real host: `agentrt/daemons/gateway_d/tests/`.

- `test_gateway_cap_registry` compiles `src/gateway_cap_registry.c` against the
  generated manifest table and checks lookup, namespace exclusivity, timeout
  and contract-version semantics.
- `test_gateway_pep_cache` compiles `src/gateway_pep_cache.c` and checks
  hit/miss, epoch invalidation and concurrency.
- `test_svcdispatch_e2e` exercises `SYS_SVC_CALL -> hook -> gw_svc_call ->
  mock daemon` end to end.
- `bench_gateway_pep` measures the PEP hot path.

There is no `tests/` directory here on purpose: a local harness would have to
fabricate the host, which is exactly the stub behaviour the project forbids.

## Module Classification

| Axis | Value |
|------|-------|
| Layer | Form face (business routing), outside the mechanism kernel |
| Aggregation | Directly tracked by the `products` repository |
| Consumer | `gateway_d` only (link whitelist) |
| Kernel fallback | Daemon not mounted when this piece is absent |
| License | AGPL v3 + Apache 2.0 |
