---
description: dxmt9 C++/C/ObjC++ codebase conventions for style, DOD boundaries, Wine bridge safety, Meson, tests, and tooling
paths:
  - "include/**/*"
  - "src/**/*"
  - "tests/**/*"
  - "meson.build"
  - "meson.options"
  - "src/**/meson.build"
  - "tests/**/meson.build"
  - "cross/*.ini"
globs: "{include,src,tests}/**/*.{h,hpp,c,cpp,mm}"
alwaysApply: false
---

# Codebase Convention

These rules describe how to change dxmt9 implementation code. They are
conventions, not feature requirements; durable behavior belongs in `specs/`.

## Current Tooling Baseline

- The build is Meson/Ninja with C++20 and C17.
- The current baseline uses `warning_level=2`, effectively `-Wall -Wextra` on the
  native build. Warnings should be treated as real review issues even though
  `werror` is not enabled by default.
- There is currently no repository `.clang-format`, `.clang-tidy`, or
  `.editorconfig`. Until those are added, do not mass-format files. Match the
  local file style and keep edits scoped to the changed lines.
- Run focused tests through Meson. For docs/rules only, `git diff --check` is
  enough unless the change affects generated tooling.

## Formatting And Layout

- Use 2-space indentation in C++, C, Objective-C++, Meson, and tests.
- Use spaces, not tabs. Do not introduce tabs in source files.
- Keep line length practical. Long ABI declarations and generated-table comments
  may exceed 100 columns, but ordinary implementation code should be wrapped.
- Headers use `#pragma once`.
- Include order should be local project headers first, then standard/system
  headers, following the surrounding file.
- Prefer `namespace dxmt9::area { ... }` and anonymous namespaces for file-local
  helpers. Keep closing namespace comments.
- Prefer `constexpr`, `std::array`, `std::span`, `std::byte`, and explicit fixed
  width aliases (`u8`, `u32`, `u64`, `i32`, `f32`) where the surrounding dxmt9
  code already uses them.
- Comments should explain non-obvious invariants, ABI constraints, Wine quirks,
  TLA+ bindings, or Metal lifetime rules. Do not add comments that restate the
  code.

## C++ Design Shape

- Preserve data-oriented hot paths. Draw/state/bridge/queue changes should use
  flat records, spans, views, fixed arrays, bitsets, handles, and arenas where
  practical.
- Avoid introducing object-heavy compatibility layers on hot paths. Boundary
  facades are acceptable when they isolate COM, Wine, Metal, or tests.
- Do not store borrowed spans, stack pointers, PE COM pointers, Objective-C
  objects, or unix-side object pointers past the call that received them unless
  the receiver first copies, interns, or translates them into owned storage.
- Avoid per-draw or per-state heap allocation in normal rendering paths. If a
  vector or string is unavoidable, keep it in cold/debug/test code or reuse a
  scratch arena.
- Use `DXMT_ASSERT` for debug-mode invariants that correspond to TLA+ safety,
  wire-format bounds, handle generation, queue ordering, or DOD storage shape.
- Prefer pure value transforms for shader, state, descriptor, format, barrier,
  and retention decisions. They should be unit-testable without Wine, Metal, or
  GPU timing.

## Wine / PE / Unix Boundary

- PE-side code must not call Metal, Objective-C, or macOS framework APIs
  directly. It records D3D9 semantics and uses the `winemetal` bridge.
- Cross-boundary records must be POD, versioned or schema-stable,
  bounds-checkable, and pointer-free.
- `winemetal.dll` is a PE bridge. `winemetal.so` is the Wine unixlib provider.
  Do not collapse this boundary for app-local convenience unless the deployment
  specs are changed first.
- Generated bridge files are produced by scripts from `include/dxmt9/device_c.h`
  and the winemetal unix schema. Do not hand-edit generated outputs in `build/`;
  change the schema or generator.
- App-local and builtin lanes share architecture but differ in discovery and
  loading policy. Keep build and runtime changes explicit about which lane they
  affect.

## Metal / ObjC++ Runtime

- Keep Objective-C++ and Metal ownership inside unix/provider/runtime modules.
- Presenter owns `CAMetalLayer` / drawable interaction. Queue and encoder code
  may request present encoding but should not make PE-visible layer ownership.
- Command encoding consumes queue-owned `MetalCommandView`,
  `FlatDrawStateView`, `DrawParam` spans, payload spans, and queue-local handles.
  It must not read PE `DeviceState`.
- Resource lifetime is sequence-ID based. Do not free or recycle underlying Metal
  resources before the queue completion waterline reaches their last use.

## Meson And Build Files

- Keep target ownership narrow: util, D3D9 frontend, dxmt9 runtime, winemetal
  bridge, unix provider, tests.
- Prefer Meson target/dependency declarations over ad-hoc shell steps.
- Add new tests to `tests/meson.build` with stable names beginning with
  `dxmt9-` unless they are intentionally external launcher scripts.
- Generated headers/sources should be represented as Meson `custom_target`s with
  explicit inputs, outputs, and `depend_files`.
- Cross-file paths should remain predictable and reviewable. Avoid baking local
  machine paths into new cross files unless the existing file already does so and
  there is no portable alternative.

## Tests And Verification

- For normal implementation changes, run the smallest relevant Meson test target:
  for example `dxmt9-core-spec`, `dxmt9-state-draw-transform-spec`,
  `dxmt9-shader-transform-spec`, `dxmt9-chunk-record-import-spec`, or
  `dxmt9-resource-hazard-spec`.
- For queue, resource lifetime, present latency, encoder lifecycle, or query
  sequencing changes, run `dxmt9-verify-tla` or `bash scripts/check/verify_tla.sh`.
- For shader translator changes, add source-contract tests before relying on
  runtime pixel probes.
- For bridge/wire-format changes, update layout/import tests before changing
  replay behavior.
- For translation boundaries, test exact value propagation at the boundary, not
  just final behavior. Assert the before/after values for formats, component
  semantics, swizzles/default channels, byte layout, sampler/descriptor state,
  handles, ordering tokens, and HRESULT/status values as applicable.
- Runtime Wine conformance and experiments are evidence layers, not substitutes
  for deterministic unit or fake-backend tests.

## License And Reference Policy

- dxmt9 project code must remain MIT-compatible.
- DXMT MIT material may be used with required notices preserved.
- Wine D3D9 tests are behavioral oracles; do not copy Wine implementation code or
  LGPL-covered source into dxmt9 project code.
- DXVK/D9VK may be used as architecture or algorithm references only unless a
  separate license review approves a compatible import.

## Review Checklist

Before finishing a code change, check:

- Does the change preserve PE/unix/Metal ownership boundaries?
- Did hot-path data stay flat, owned, and bounded?
- Are borrowed spans consumed immediately or copied into owned storage?
- Are sequence IDs, frame tokens, retained handles, and deferred destruction still
  respected?
- Is there focused deterministic test coverage for the changed behavior?
- Did `git diff --check` pass?
