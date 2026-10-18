---
domain: present-pacing
workload: STALKER CoP H502/r5 original captured input
title: "H627 — lexical override projection removes full copies but fails the CPU screen"
type: experiment
status: rejected-screen
updated: 2026-10-16
source: traces/stkcop-override-projection-h627/{contract.md,prepare.py,derivation.json,projection.hpp,actual-helper.hpp,values.json,mutants.json,native-tests.json,build.json,preflight.json,timing-runs.json,timing-analysis.json}; traces/stkcop-owner-composition-h621/{N0,N1}/joint.json
related: present-pacing-layout-reuse.626.md; present-pacing-draw-loop-scratch.487.md; present-pacing-encode-live-detail.622.md; present-pacing-owner-composition.621.md
---

# H627 — full binding-context copies are removable work, not a qualified saving

The serial DrawRun path copies an 8,112-byte `FlatDrawStateRecord` and a
2,248-byte `DrawShaderLayoutContext` for each binding override. Declaration
elements are fixed storage, not a per-draw heap allocation. Shader bytecode
and stream-buffer references use shared ownership; copying and destroying the
layout performs reference bookkeeping as well as value copies.

A private lexical projection reduces full context copies from 3,982 to 757,
with 3,225 partial restores. Original pixels, queries and lifetimes match.
The frozen Encode CPU screen nevertheless fails: median OFF-minus-ON is
+0.722ms per captured execution, median lower bound +0.605ms, and the paired
sham bound is 0.937ms. Only five of eight block lower bounds are positive.
There is no qualified live saving or product promotion.

## Distinct mechanism and ownership

H487 conditionally constructed scratch to avoid default initialization; it
retained full copies on binding overrides. H627 targets those full copies.
It does not incorporate H626's decoder memo, H499's native draw/shadow policy,
or the H606–608 Replay state-prefix shortcut.

`src/dxmt9/dxmt9_draw_encoder_chunk.mm`, `encodeDrawSubrange`, retains the
original OFF branch and per-draw scratch declarations. ON uses one projection
owned by the enclosing `encodeDrawRunCommand` invocation. The first binding
draw copies the immutable source hot/shader contexts. Subsequent binding draws
restore previously changed stream fields, stream-mask/index and corresponding
key fields from that source, then invoke the unchanged
`src/dxmt9/dxmt9_draw_encoder_draw.mm`, `applyDrawBindingOverride`.

Stream buffer references must also be restored: the production helper clears
them when the effective handle differs from the base handle. Restoring only
offset/stride would retain stale cleared references on an A→B→A sequence.
None/alpha-only overrides continue to use the original base view; their
interposition does not invalidate the next partial restore. The next prepare
invalidates the prior returned view, just as the existing synchronous caller
must not retain its per-draw stack view. Uniform selection remains per draw.

The immutable source dominates the projection lifetime. Its references already
keep base resources alive, so retaining another base reference within that
lexical run does not extend an object's last-owner lifetime beyond the source.
No cross-source cache, queue state, draw/pass order or GPU retirement policy
changes. Original draw-function ABI and parallel path remain unchanged.

## Qualification

The native checker includes a byte-for-byte source-derived copy of the actual
production helper body, authenticated against the current source. It compares
whole hot/layout values with fresh-copy-plus-helper reference semantics:

- ARM64 and x86_64 each pass 1,048,576 comparisons: eight-action, length-four
  histories at every stream position, shader present/absent and OFF/ON.
  Actions cover none, alpha-only, same/different/null buffer, index-only,
  combined overrides, disjoint streams and repeated/ABA transitions.
- The base remains unchanged. Shared buffer ownership remains live throughout
  consumption and reference counts return after lexical owners are destroyed.
- Four mutants independently omit hot-stream, key-stream, shader-stream or
  index restoration; all are rejected. These finite value/lifetime checks
  bind the new sequential transition, not arbitrary concurrent schedules.
- Observer controls pass 12 native cases and 22 corrupt traces. Candidate and
  observer namespaces are disjoint; combined-header gate-independence tests
  pass on both architectures.
- Private source changes reverse to production, the draw implementation is
  unchanged, parent/archive hashes and install names pass, and loaded images
  are authenticated. No shared build or Wine installation is modified.

The first native scaffold compile lacked the surrounding `u32` alias required
by the extracted helper; adding that alias fixed the scaffold without changing
the helper body. This happened before GPU qualification or timing.

Original-input OFF/ON and ordinary-queue OFF/ON controls pass. Original
source/output SHA-256 remains
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Both original runs retain 670 balanced wrappers, seven mapped sources, nine
Encode roots and the 9/5/4/5 query transcript. Witnesses establish
OFF calls/copies/reuses = 3,982/3,982/0 and ON = 3,982/757/3,225.
Ordinary-queue controls skip draws and validate attachment behavior; the
original fixture supplies pixel evidence. These do not establish arbitrary
live rendering or actual-Metal occlusion correctness.

## Fixed timing result

Same private image and H502/r5 source, fresh processes; two warmups, eight OFF
shams, eight OFF/ON/ON/OFF blocks. Root-only observation remains, product perf
and projection witnesses are OFF. No per-draw clocks, concurrent builds,
workloads or external parsers run during the population. All 42 executions
preserve exact original effects. Independent decoding verifies 378 roots and
4,195 hashes. No replacement, extension or outlier exclusion occurs.

Metric: sum of nine same-thread Encode-root CPU intervals per captured
execution. This includes preparation/cold work within those roots and is not
a live ms/frame, pure memcpy cost or FPS measure. OFF includes candidate
branch/storage shape, not byte-identical production entry. Bounds are
deterministic endpoint enclosures, not confidence intervals.

| ABBA block | OFF-minus-ON CPU ms | Conservative interval, ms |
|---|---:|---:|
| 0 | +1.069 | +0.954 to +1.187 |
| 1 | +0.412 | +0.292 to +0.529 |
| 2 | −0.406 | −0.516 to −0.299 |
| 3 | +1.032 | +0.918 to +1.155 |
| 4 | +1.663 | +1.556 to +1.769 |
| 5 | −0.510 | −0.612 to −0.405 |
| 6 | +1.499 | +1.384 to +1.621 |
| 7 | +0.072 | −0.048 to +0.180 |

The predeclared gate requires every block lower bound positive and the median
lower bound above the largest absolute paired-sham bound. Both conditions
fail. This closes this population without promotion; it does not prove zero
intrinsic copy cost. Do not price all of H622's caller-loop budget as removable
or repeat this candidate until a favorable sample appears.

## Return to the unresolved maximum-owner proof

H621's same-episode Primary renderer upper bounds remain 19.196/18.879ms/frame,
overlapping Encode's draw loop. Their raw decomposition shows 1,777,147 and
1,837,012 enclosed root calls. The declared 200ns/root endpoint allowance alone
adds 3.173/3.087ms/frame, respectively. Measured enclosing root elapsed is
16.022/15.791ms/frame and still includes waits; it is not a CPU estimate and
must not replace the qualified upper bound.

The next ownership task is to tighten that bound with new qualified evidence,
not to lower the existing clock allowance after seeing the result. A targeted
same-thread off-CPU enclosure or a separately validated timing method can
reduce uncertainty while preserving identity and the common camera interval.
Lifecycle-tail/default-transfer exclusions remain. Another small cache/copy
candidate is not a substitute for closing this global comparison. No largest
default-path owner or live removable amount is yet proved.
