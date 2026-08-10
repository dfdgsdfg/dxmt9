---
type: "Spec Index"
title: "Specifications Index"
description: "Entry point for durable dxmt9 specifications."
tags: [specs, index]
---

# Specifications Index

This is the entry point for durable dxmt9 specifications. Use [README](README.md)
for terminology and reading guidance, [gap](gap.md) for implementation/evidence
status, and [log](log.md) for spec-tree maintenance history.

## Root Documents

- [README](README.md) - terminology, architecture mapping, and reading guidance.
- [Gap index](gap.md) - root rollup for domain-owned implementation and evidence gaps.
- [D3D9 API coverage inventory](d3d9/gap_d3d9.md) - per-item D3D9 API coverage.
- [Wine-test gap inventory](tests/gap_d3d9_wine_test.md) - Wine D3D9 conformance coverage.
- [Spec log](log.md) - root spec-tree maintenance history.

## Gap Documents

- [archicture/gap](archicture/gap.md) - architecture, DOD, copy-policy, and concurrency acceptance.
- [d3d9/gap](d3d9/gap.md) - frontend COM/API, state, resources, queries, and WSI-facing gaps.
- [backend/gap](backend/gap.md) - Metal backend, command queue, resources, and encoder lifecycle.
- [d3d9-renderer/gap](d3d9-renderer/gap.md) - modern renderer implementation and evidence gaps.
- [deploy/gap](deploy/gap.md) - Wine PE / winemetal deployment.
- [verification/gap](verification/gap.md) - TLA+ and deterministic native verification shortfalls.
- [tests/gap](tests/gap.md) - test corpus, Wine conformance, and unit-first DoD gaps.
- [experiments/gap](experiments/gap.md) - wild integration experiment coverage.
- [benchmarks/gap](benchmarks/gap.md) - benchmark harness, workloads, and regression-policy gaps.
- [d3d8/gap](d3d8/gap.md) - D3D8 shim gaps.
- [d3d7/gap](d3d7/gap.md) - D3D7 / DirectDraw shim gaps.

## Domain Logs

- [archicture/log](archicture/log.md) - architecture spec maintenance history.
- [backend/log](backend/log.md) - backend spec maintenance history.
- [benchmarks/log](benchmarks/log.md) - benchmark spec maintenance history.
- [d3d7/log](d3d7/log.md) - D3D7 / DirectDraw spec maintenance history.
- [d3d8/log](d3d8/log.md) - D3D8 spec maintenance history.
- [d3d9/log](d3d9/log.md) - D3D9 frontend spec maintenance history.
- [d3d9-renderer/log](d3d9-renderer/log.md) - modern renderer spec maintenance history.
- [deploy/log](deploy/log.md) - deployment spec maintenance history.
- [experiments/log](experiments/log.md) - experiment spec maintenance history.
- [tests/log](tests/log.md) - test and conformance spec maintenance history.
- [verification/log](verification/log.md) - verification spec maintenance history.
- [winemetal/log](winemetal/log.md) - winemetal spec maintenance history.

## Topics

- archicture: [requirements](archicture/requirements.md), [spec](archicture/spec.md) - project architecture and DOD contracts.
- backend: [requirements](backend/requirements.md), [spec](backend/spec.md) - shared Metal backend contracts.
- backend/render-provider: [requirements](backend/render-provider/requirements.md), [spec](backend/render-provider/spec.md), [gap](backend/render-provider/gap.md) - selectable rendering-mode lifecycle and composition registry.
- benchmarks: [requirements](benchmarks/requirements.md), [spec](benchmarks/spec.md) - benchmark and regression policy.
- d3d7: [requirements](d3d7/requirements.md), [spec](d3d7/spec.md) - D3D7 / DirectDraw shim contracts.
- d3d8: [requirements](d3d8/requirements.md), [spec](d3d8/spec.md) - D3D8 shim contracts.
- d3d9: [requirements](d3d9/requirements.md), [spec](d3d9/spec.md) - D3D9 frontend contracts.
- d3d9-renderer: [requirements](d3d9-renderer/requirements.md), [spec](d3d9-renderer/spec.md) - modern renderer and default passcoalesce-only L1 path.
- deploy: [requirements](deploy/requirements.md), [spec](deploy/spec.md) - Wine runtime and packaging contracts.
- experiments: [requirements](experiments/requirements.md), [spec](experiments/spec.md) - wild integration experiment policy.
- tests: [requirements](tests/requirements.md), [spec](tests/spec.md) - test corpus and conformance design.
- verification: [requirements](verification/requirements.md), [spec](verification/spec.md) - formal model and evidence contracts.
- winemetal: [requirements](winemetal/requirements.md), [spec](winemetal/spec.md) - Wine/Metal bridge contracts.
