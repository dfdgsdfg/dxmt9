---
domain: present-pacing
workload: STALKER CoP H502/r5 original captured input
title: "H610 — captured draw-loop ownership and detailed observer cost"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-encode-phases-h610/analysis.json; traces/stkcop-encode-phases-h610/runs.json; traces/stkcop-encode-phases-h610/native-tests.json; traces/stkcop-encode-phases-h610/detail-tests.json
related: docs/perfomance/present-pacing/present-pacing-encode-owner.609.md; docs/perfomance/present-pacing/present-pacing-draw-loop-scratch.487.md; docs/perfomance/present-pacing/present-pacing-encode-callsite.496.md
---

# H610 — captured draw-loop ownership and detailed observer cost

H609's large loop is not one dominant texture-binding operation. In this
original captured source, outer-loop preparation, encodeDraw setup and the
index/issue region are comparable owners. Detailed observation measurably
raises loop time. No product change, live transfer or removable budget is proved.

## Fixed experiment

Use H602 default archives and the same H502/r5 bundle as H609. Three private
archive members carry H610 instrumentation: queue, chunk encoder and draw
encoder. No PE binary, Wine installation or shared build is modified.

The existing generation-qualified root/outer scopes stay enabled with every
root selected. A separate detailed switch surrounds encodeDraw and follows its
existing phase boundaries. CPU is read at root entry/exit, never per draw;
detail uses per-draw wall clocks only in this captured-input diagnostic. CPU
bounds use root CPU, complementary wall time and endpoint uncertainty, not
wall-as-CPU substitution. Early returns and exceptions close scopes.

Fixed order is O0,D0,D1,O1, where O has detail OFF and D has detail ON. Same
binary, product counters OFF, fresh process/device, no profiler or concurrent
build. Every execution retains nine root sequences. The large captured source
is local sequence8:526 DrawRuns,530 subrange invocations and3125 encodeDraws.
Source numbering differs from the older H481 input; do not equate their local
sequence numbers or treat their timings as paired results.

## Observation cost

All times below are milliseconds per execution of this captured source, not
per live frame. The loop includes encodeDraw and outer draw preparation.

| Run | Root CPU bound | Whole-loop wall | Detail |
|---|---:|---:|---|
| O0 | 37.002–37.030 | 13.492 | OFF |
| D0 | 39.206–39.228 | 14.834 | ON |
| D1 | 40.049–40.069 | 15.375 | ON |
| O1 | 37.182–37.207 | 13.319 | OFF |

Detailed loop walls exceed both coarse controls by1.342–2.056ms. Root CPU
interval differences are2.000–3.068ms. These are finite observed contrasts,
not statistical or universal overhead bounds. Root includes cold prefetch and
other wrapper work; do not assign its entire difference to the clocks or
subtract one global tax from individual phases. Detailed values are useful for
ownership, not a calibrated unobserved product cost.

## Disjoint detailed CPU bounds

| Region | D0 | D1 |
|---|---:|---:|
| Outer loop, excluding encodeDraw | 2.849–3.197 | 3.411–3.588 |
| encodeDraw scope remainder | 0.606–0.954 | 0.789–0.967 |
| Setup | 2.818–3.165 | 3.172–3.350 |
| Argbuf/uniform and binding-plan preparation | 1.307–1.654 | 1.547–1.725 |
| Stream preparation | 0.976–1.323 | 1.233–1.411 |
| FFP vertex | 0–0.129 | 0–0.130 |
| Programmable vertex binding | 0.075–0.422 | 0.293–0.471 |
| Texture/base-state binding | 0.209–0.557 | 0.398–0.576 |
| Index setup / issue | 3.062–3.409 | 2.957–3.135 |
| Final fallthrough | 0–0.024 | 0–0.023 |

These are semantic code regions, not pure driver versus dxmt9 categories.
Index/issue contains index resolution, volatile payload setup, diagnostics and
the Metal draw call. Setup contains declaration decoding, diagnostic gates,
pipeline/depth selection and labels. The scope remainder includes phase
transition bookkeeping. Only13 draws reach final fallthrough. There is no
replicated unique largest individual draw subphase, nor proof that any whole
region can be deleted.

## Verification and candidate discipline

All four runs match original source/output SHA
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
query transcript, seven mapped sources,670 balanced wrappers and restoration.
12 ARM64/x86_64 native cases,22 corrupt-trace tests and four detail-switch
controls pass. Independent raw-layout/CPU/conservation checks cover36 root
rows. Archive membership and reverse source transformation are verified.

Source/codegen inspection again finds unconditional serial override scratch
initialization. That is **H487's already-tested candidate**, not a new discovery
or justification to repeat its failed finite timing gate. Likewise H497's
raster lookup cache and H499's VS immediate shadow are not reopened.

Two setup mechanisms remain worth distinguishing before implementation:

- `dxmt9_draw_encoder_draw.mm`, `encodeDraw`, calls
  `decodeFixedFunctionVertexLayout` for every draw. The decoder scans declaration
  elements and hashes the result even for programmable draws. Command-local
  reuse must preserve stream0 stride changes from binding overrides; a VS-handle
  cache alone is not a sufficient identity. No saving is measured here.
- The same function recomputes `shaderVariantHashForDraw`, resolves texture
  properties and formats/sets a PSO label on base-state rebind. This is not
  unconditional per draw, and labels must remain correct for capture. Separate
  attribution is required before moving or reusing this work.

Next price these concrete setup operations with fewer diagnostic boundaries
on the frozen input, retaining the coarse loop control. Do not return to broad
FPS repetitions or assume driver calls explain the whole loop. Largest
default-tail dxmt9 CPU owner and qualified removable time remain open.
