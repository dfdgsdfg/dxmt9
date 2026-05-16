---
description: Win32, PE D3D9 ABI, COM, HRESULT, HWND, message-loop, and public Windows-visible debugging rules
paths:
  - "src/d3d9/**"
  - "include/dxmt9/**"
  - "tests/conformance/d3d9/**"
  - "tests/integration/wsi_present/**"
  - "tests/module_boundary/**"
globs: "{src,include,tests}/**/*.{c,cpp,h,hpp}"
alwaysApply: false
---

# Windows / PE Debugging Rules

Use this for Windows-visible behaviour: PE exports, D3D9 public ABI, COM
lifetimes, HRESULT/out-pointer rules, `HWND` ownership, message loops, and
Wine-oracle conformance cases.

## Public ABI Before Backend State

When a PE test fails, first decide whether the failure is Windows-visible D3D9
semantics or a backend/rendering issue. HRESULTs, out-pointer mutation, COM
refs, and object identity are frontend evidence.

**Rules:**
- Preserve Wine/Windows-observed HRESULT and out-pointer behaviour even when the
  backend would prefer a simpler error path.
- Public methods must validate D3D9 arguments before emitting backend records or
  retaining backend handles.
- Do not dereference invalid app pointers merely because Wine tests exercise
  access-violation cases. dxmt9 may use a clean invalid-call policy when specs
  explicitly choose safe behaviour.
- COM `AddRef` / `Release` evidence belongs in PE conformance or native COM
  tests, not in renderer probes.

## HWND And Message Loop Evidence

WSI bugs often look like renderer bugs. A visible frame is not proof that the
correct `HWND` mapped to the correct layer unless the result records identity.

**Rules:**
- Record `HWND`, window title, creation success, and presented-frame count in
  WSI smoke logs.
- Classify full-screen or frontmost-window capture as fallback triage. It must
  not prove HWND-to-layer success.
- A blocked message loop, destroyed window, or wrong device-window handle is a
  Windows/WSI failure before it is a Metal failure.
- Keep `wsi_present` smoke small and deterministic; broader app behaviour
  belongs in wild experiments.

## Wine Behaviour Is An Oracle, Not Architecture

Wine tests are valid behavioural evidence for public D3D9 compatibility, but
Wine's implementation structure is not a design requirement for dxmt9.

**Rules:**
- Cite Wine source files and functions only as behavioural anchors.
- Re-express test cases in dxmt9-owned code; do not copy Wine helper code,
  control flow, bulk tables, or inline shader strings into MIT-owned files.
- Keep Wine-oracle conformance separate from native unit tests and WSI/module
  boundary probes.
- When a Wine case is too broad, split local cases by observable D3D9 contract:
  factory validation, reset/lost-device, stateblock, private data, query, etc.

## Related

- `agents/rules/debug_wine.rules.md` - Wine runtime and PE/unix host failures.
- `agents/rules/debug_d3d9.rules.md` - D3D9 render/draw/shader intent.
- `specs/d3d9/requirements.md` - public D3D9 frontend contract.
- `specs/tests/design.md` - Wine-oracle conformance harness rules.
