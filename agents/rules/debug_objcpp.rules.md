---
description: Objective-C++, Cocoa, macdrv, CAMetalLayer, NSView, autorelease, and macOS window debugging rules
paths:
  - "src/**/*.mm"
  - "src/**/*.m"
  - "src/dxmt9/**"
  - "src/winemetal/**"
  - "tests/integration/wsi_present/**"
globs: "{src,tests}/**/*.{m,mm,h,hpp,cpp}"
alwaysApply: false
---

# Objective-C++ / Cocoa Debugging Rules

Use this for Objective-C++ and macOS host issues: Wine `macdrv`, Cocoa view
lookup, `CAMetalLayer` ownership, autorelease/threading, and macOS capture or
window APIs.

## Keep ObjC++ Behind The Unix / Runtime Boundary

PE-side D3D9 code must stay Windows-shaped. Cocoa, Objective-C objects, and
Metal layer ownership belong on the unix/provider/runtime side.

**Rules:**
- Do not call Cocoa, Metal, or Objective-C APIs from PE-side D3D9 code.
- Do not store `NSView*`, `CAMetalLayer*`, Objective-C objects, or borrowed
  Cocoa pointers in PE-visible records.
- Convert Wine/macOS host objects into opaque handles, retained runtime objects,
  or result evidence before crossing back to PE-visible logic.
- If a test needs Cocoa evidence, keep it in WSI/module-boundary/integration
  harnesses rather than native stateless unit tests.

## CAMetalLayer Evidence

Layer acquisition is a WSI fact. Visible output alone does not prove the layer
came from the requested `HWND`.

**Rules:**
- Record whether acquisition used `macdrv_functions`, legacy
  `macdrv_get_cocoa_view`, fallback view creation, or unavailable/failure.
- If the runtime falls back from `WineWindow` to content view to Metal view,
  record the path in diagnostics instead of treating it as a generic present.
- A full-screen capture can prove "something was visible"; it cannot prove
  `HWND` ownership unless paired with layer/window identity.
- Keep `CAMetalLayer.framebufferOnly` and backbuffer readback tradeoffs explicit
  because capture/readback diagnostics may rely on non-framebuffer-only access.

## ObjC++ Lifetime And Threads

Many macOS failures are lifetime or thread-affinity failures, not draw-state
bugs.

**Rules:**
- Wrap temporary Objective-C object bursts in local autorelease pools when
  running outside normal AppKit event-loop boundaries.
- Retain host objects that outlive the call that discovered them; never keep a
  borrowed Cocoa pointer as durable state.
- UI/window lookup must respect AppKit thread constraints. If a helper runs from
  a Wine/unix worker thread, document and isolate the dispatch point.
- When diagnosing capture failures, separate "window not found", "layer not
  found", "capture tool failed", and "renderer produced wrong pixels".

## Related

- `agents/rules/debug_metal.rules.md` - Metal capture, validation, labels,
  counters, and command-buffer failures.
- `agents/rules/debug_windows.rules.md` - `HWND` and public D3D9 semantics.
- `specs/d3d9/wsi/requirements.md` - WSI contract.
- `specs/winemetal/requirements.md` - Wine/macdrv compatibility requirements.
