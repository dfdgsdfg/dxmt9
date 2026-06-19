---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: encode-phase
order: 152
title: Command Uniform Materialize Cache
date: 2026-06-19
type: code-cleanup
status: accepted-scoped-cleanup-runtime-unmeasured
source: src/dxmt9/dxmt9_backend_types.hpp, src/dxmt9/dxmt9_draw_encoder.mm, tests/native/core/state_draw_transform_spec.cpp
related: docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.151.md
---

# Encode Phase 152 - Command uniform materialize cache

## Question

Can the H161 "legacy compact consumer" residual be reduced without changing the
frontend `DrawUniformPayload` snapshot policy or the Metal constant ABI?

## Verdict

Yes, but only as a scoped encode cleanup. The draw encoder now keeps a
command-local `DrawUniformPayloadMaterializeCache` for compact draw-run commands.
When a non-front per-draw override uniform handle repeats inside the same
DrawRun command, the encoder reuses the already materialized override scratch
instead of reconstructing the full legacy `DrawUniformPayload` again.

The command-front payload stays in its own scratch. This separation is required:
the cache owns one mutable scratch slot, so a cache miss for an override handle
would overwrite any retained command-front pointer if both used the same cache.

This does not solve the larger H161/H149 owners:

- frontend uniform materialization and hash still happen before the chunk reaches
  the backend;
- compact append storage is unchanged;
- the change is runtime-counter unmeasured until the next no-gputrace run;
- any FPS interpretation remains gated by the `v0.0.3` visual-safe anchor.

## Code shape

`DrawUniformPayloadMaterializeCache` is keyed by both the compact uniform handle
and the command's compact payload-record span. The extra source identity matters
because `DrawUniformHandle` indices are slot-local. The encoder resets the cache
at each draw-run command boundary and uses it only for per-param override
payloads; the command-front payload is materialized into a separate scratch.

```mermaid
flowchart TD
  A["encode draw-run command"] --> B["reset materialize cache"]
  B --> C["materialize command uniform handle\nseparate command scratch"]
  C --> D["encode each DrawParam"]
  D --> F{"param uses command handle?"}
  F -- "Yes" --> G["reuse command payload pointer"]
  F -- "No, same override source + handle" --> H["reuse cached override scratch"]
  F -- "No, different handle/source" --> I["materialize requested handle"]
  H --> J["bind constants / draw"]
  I --> J
  G --> J

  classDef cleanup fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef gate fill:#fff3cd,stroke:#a80,color:#640
  class H cleanup
  class I gate
```

## Verification

Native coverage now checks:

- a compact draw-run command view with no command-level legacy uniform pointer
  can resolve through the cache helper;
- a batched draw-run with two repeated non-front override uniform handles reuses
  the override scratch while keeping the command-front scratch stable.

Commands run:

```sh
meson test -C build-arm64-nowine dxmt9-state-draw-transform-spec
meson compile -C build-arm64-nowine
```

## Runtime follow-up

[[state-churn-encode-encode-phase.153]] runs the GT1 no-gputrace follow-up
against a current run gated by the `v0.0.3` visual anchor. It compares:

- `draw_uniform_payload_materialized`
- `draw_uniform_payload_materialized_bytes`
- `draw_uniform_payload_materialize_cpu_ms`
- `draw_uniform_payload_materialized_draw_encoder_command`
- `draw_uniform_payload_materialized_draw_encoder_param`
- `draw_uniform_payload_materialize_draw_encoder_command_cpu_ms`
- `draw_uniform_payload_materialize_draw_encoder_param_cpu_ms`

The counters do not move in the intended direction, so H162 is rejected as a
current GT1 owner. Keep H161's ranking unchanged and return to frontend
compact-owned snapshots, direct compact consumers with measured no-gputrace
movement, or P4/serial-cadence overlap.
