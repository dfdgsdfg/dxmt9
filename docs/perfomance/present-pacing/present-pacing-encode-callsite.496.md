---
domain: present-pacing
workload: STALKER CoP H481 original Normal input, H494 frozen x86 Encode profile
title: "Resolved Encode call boundaries separate copies, resource lookup and driver work"
type: evidence
status: callsite-attribution-no-exclusive-cost-or-default-transfer
updated: 2026-10-11
source: traces/stkcop-encode-callsite-h496/{map.py,callsites.json,callsites.txt,classify.py,classification.json,indirect-symbols.txt}; traces/stkcop-encode-cpu-stack-h494/{analysis-v2.json,encode-observations-v2.json}; traces/stkcop-v4-pending-lookup-h493/candidate-build.json
related: present-pacing-encode-cpu-stack.494.md; present-pacing-binding-counter-gate.495.md; present-pacing-root-game.477.md
---

# Call-boundary attribution, not exclusive CPU ownership

H493–H495 were committed as `221d6f60`. Focused x86 tests passed 6/6 on
recheck: binding counters OFF/ON, V4 identity, counter table/callsite audits
and install names. H496 changes no runtime or experiment policy and launches
no game, profiler or GPU replay. It inspects the preserved H494 observations
against their exact frozen H493 native image, not the newly committed image.

The image SHA-256 is checked against its build receipt. Each of 21 observations
with known `encodeChunk` ancestry supplies a resolved x86 image/load address.
`map.py` finds the first frame in that image and disassembles its enclosing
text symbol. `classify.py` verifies the recorded address lies inside exactly
one call instruction and resolves imported targets through the Mach-O indirect
symbol table. In this export each address is the final byte of the call,
consistent with adjusted caller return addresses. Unknown leading PCs remain
unknown; this is not a Rosetta unwinder.

## Corrections to the interpretation of H494

H494's disjoint ancestry buckets remain a faithful description of exported
names. However, **five of its twelve other-encodeChunk observations are at the
call to `encodeDraw`**, with no resolved callee frame. They cannot be priced as
loop/dispatch work independent of draw encoding. Two more are at calls to the
draw-subrange lambda. Neither call-site discovery reconstructs the missing
callee or establishes which instruction consumed the CPU sample.

| Resolved image offset | Observations | Verified call boundary | Valid interpretation |
|---|---:|---|---|
| `0x2682dc` | 5 | DrawRun subrange → `encodeDraw` | Draw-call boundary; callee ancestry incomplete |
| `0x251a75`, `0x251afb` | 2 | DrawRun command → draw subrange | Nested draw work cannot be charged to the outer loop |
| `0x255603` | 1 | `materializeDrawUniformPayload` → `memcpy`, length 512 | Fixed payload copy boundary, not uniform validation |
| `0x33e263` | 1 | `ResourceArena::uploadBuffer` → `memcpy` | Slab-copy boundary, not evidence of upload mutex waiting |
| `0x277e93`, `0x2f9dee` | 1 each | `encodeDraw` → `Pool::findSurface`; `findSurface` → `lock_shared` | Resource lookup and shared-lock acquisition exist; contention duration unknown |
| `0x275918` | 1 | `encodeDraw` → `shaderVariantHashForDraw` | Variant hash for runtime PSO labeling on base-state rebind |
| `0x278069` | 1 | `encodeDraw` → `CommandBindingPlan::prepareStreamOverride` | Plan update boundary, not proven full plan rebuild |
| `0x2de57b` | 1 | `samplerStateFor` → hash-table `find` | Cache lookup; fragment shadow hits already skip this path |
| `0x233d68`, `0x2c1b2b` | 3 total | Clear/pass begin → Metal render encoder creation | Known driver ancestry, not CPU-side plan-only work |
| `0x2a9f7e` | 1 | `recordedSetVertexBytes` → Metal command encoding | Driver allocation ancestry exists under vertex-byte binding |
| `0x2610f2` | 1 | Chunk split → Metal command-buffer commit | Includes driver pass-finalization ancestry |
| `0x273bea` | 1 | `encodeDraw` → signpost emission | Profiler-active diagnostic work; not default OFF cost |
| `0x31130b` | 1 | Binding counter → `perf::enabled` | H495 already removed repeated gates; saving remains unproven |

All observations remain Running/Timer-Fired from H494's raw/model join.
Running at a lock-acquisition boundary does not establish blocking or wait
time. Counts are not converted to milliseconds, CPU shares, source-qualified
multiplicity or removable budgets. H494 still includes cold prefetch and
active signposts; none of this ranks the default game's mixed Primary thread.

## Work-removal candidates after checking existing reuse

### First bounded candidate: command-local raster-target metadata

`dxmt9_draw_encoder_draw.mm`, `encodeDraw`, resolves `bindingPacketSurface`
before every binding-plan preparation, even when `native.commandBindingPlan`
already exists. This pointer is used only for texture presence and width/height
in that block. `dxmt9_resource_pool.hpp`, `HandleArena::find`, takes a shared
lock for lookup. The existing `CommandBindingPlan` freezes sampler projection,
but its `prepareStreamOverride` still rebuilds extra-stream and raster values
from per-draw inputs. It does not freeze raster-target metadata.

The narrow next design is a command-local, identity-qualified raster-target
value, reusing the existing command lifetime rather than introducing a global
pointer cache. It must preserve attachment generation and missing/textureless
fallback to 1×1, permit stream/index overrides without changing target identity,
and reset at command/source boundaries. Dynamic viewport/scissor, cull and
pretransformed inputs must still feed the raster reducer per draw. Standalone
and parallel-child paths without that capability must remain correct. Confirm
that no supported in-command transition replaces the backing before reuse.

Before claiming a saving, compare the old resolver and reused metadata on
foreign/stale attachment identity, missing targets, stream overrides and
pass/session transitions. Existing `testCommandBindingPlanRefinement` is a
useful differential seam but is not alone a resource-lifetime proof. Then use
the unchanged original source/output oracle and an unprofiled frozen A/A,
AB/BA comparison. The observed lookup is motivation, not proof this is largest.

### Larger representation candidate: command uniform materialization

`encodeDrawRunCommand` reconstructs a full `DrawUniformPayload` once per
command; it does **not** do so unconditionally per draw. Same-handle parameters
reuse the command scratch, and changed handles have
`DrawUniformPayloadMaterializeCache`. The materializer still copies the full
fixed-function payload, including transforms, alongside programmable constants.
The resolved 512-byte copy is consistent with that fixed-field path.

A stage-specific borrowed view could remove unnecessary fields rather than
adding another whole-payload cache. That is a wider representation contract:
first enumerate all fixed-field readers, hashing/identity dependencies and
scratch lifetimes, and establish actual programmable versus fixed-function
usage in the original input. A single copy observation does not justify
partially initializing the existing payload or claiming a byte/time saving.

### Do not reopen these as unconditional per-draw work

- Fragment sampler shadow matching already precedes sampler-cache resolution.
- PSO label hashing/formatting is under `!effectiveSkipBaseStateBind`; it is
  not unconditional for every draw. Labels are mutable diagnostic attribution,
  not compiled pipeline identity. Preserve capture labels if moving their work.
- Transient slab append already has a non-wrapped fast path avoiding overlap
  scans. The observed copy is not evidence of an overlap-scan bottleneck.
- H495's small counter change failed its timing gate; do not repeat or broaden
  it into a claimed solution to the tail gap.

The result narrows the next implementation hypothesis while leaving the goal
open: largest default dxmt9 single-thread CPU owner, whole-tail representativeness
and qualified removable milliseconds are still unresolved.
