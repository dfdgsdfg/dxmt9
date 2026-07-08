---
type: "Spec Gap Index"
title: "Spec Gap Overview"
description: "Root overview for domain-owned implementation and evidence gaps."
tags: [specs, gap, index]
---

# Spec Gap Overview

This root document is only the project-level overview. Detailed implementation and evidence rows live in the owning domain `gap.md`; older structural notes live in each domain `log.md`.

## Domain Overview

| Domain | Status | Current gap | Log | Scope |
|---|---|---|---|---|
| Architecture | partial | [archicture/gap](archicture/gap.md) | [archicture/log](archicture/log.md) | Project architecture, DOD, copy-policy, and concurrency acceptance. |
| D3D9 | partial | [d3d9/gap](d3d9/gap.md) | [d3d9/log](d3d9/log.md) | Frontend COM/API, state, resources, queries, WSI-facing behaviour. |
| Backend | partial | [backend/gap](backend/gap.md) | [backend/log](backend/log.md) | Metal backend, command queue, resources, encoder lifecycle. |
| D3D9 Renderer | partial / opt-in | [d3d9-renderer/gap](d3d9-renderer/gap.md) | [d3d9-renderer/log](d3d9-renderer/log.md) | Modern renderer opt-in path. |
| Deployment | partial | [deploy/gap](deploy/gap.md) | [deploy/log](deploy/log.md) | Wine PE / winemetal builtin and app-local deployment. |
| Verification | partial | [verification/gap](verification/gap.md) | [verification/log](verification/log.md) | TLA+ and deterministic native verification shortfalls. |
| Tests | partial | [tests/gap](tests/gap.md) | [tests/log](tests/log.md) | Test corpus, Wine conformance, and unit-first DoD gaps. |
| Experiments | partial | [experiments/gap](experiments/gap.md) | [experiments/log](experiments/log.md) | Wild integration experiment coverage. |
| Benchmarks | not started | [benchmarks/gap](benchmarks/gap.md) | [benchmarks/log](benchmarks/log.md) | Benchmark harness, workloads, and regression-policy gaps. |
| D3D8 | not started | [d3d8/gap](d3d8/gap.md) | [d3d8/log](d3d8/log.md) | D3D8 shim implementation and evidence gaps. |
| D3D7 / DirectDraw 7 | not started | [d3d7/gap](d3d7/gap.md) | [d3d7/log](d3d7/log.md) | D3D7 / DirectDraw shim implementation and evidence gaps. |
| Winemetal | tracked elsewhere | Tracked through [deploy/gap](deploy/gap.md) and [backend/gap](backend/gap.md) | [winemetal/log](winemetal/log.md) | Wine/Metal bridge contract documentation; implementation gaps currently roll up through deployment/backend. |

## Detailed Inventories

- [D3D9 API coverage inventory](d3d9/gap_d3d9.md) - per-item D3D9 API coverage matrix.
- [Wine D3D9 test inventory](tests/gap_d3d9_wine_test.md) - Wine `d3d9/tests/*` oracle coverage matrix.

## Update Rule

When a requirement is partial, missing, or newly evidenced, update the owning domain `gap.md`. Update this root overview only when domain ownership, overall status, or navigation changes.
