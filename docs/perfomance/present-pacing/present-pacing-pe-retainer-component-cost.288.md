---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Actual PE retainer component pricing"
type: evidence
status: diagnostic-attribution
updated: 2026-09-08
source: experiments/output/pe-retainer-component-cost-20260908/analysis
related: present-pacing-pe-retainer-epoch-shapes.287.md; present-pacing-pe-binding-wire-authority.286.md; ../../../specs/d3d9/recorder/requirements.md
---

# Actual PE retainer component pricing

H288 prices the existing retainer's loaded PE bodies without changing product
code. The warm PE32 100-entry index rebuild costs 1.705 microseconds; an
interleaved 124→100-entry expiry costs 5.583 microseconds. These controlled
cases separate index work from release-inclusive work, but do not explain all
of [H287's](present-pacing-pe-retainer-epoch-shapes.287.md) 18.574/16.438
microsecond sampled GT2 epochs. A production index rewrite is not promoted.

## Actual-image measurement boundary

`candidate.json` freezes HEAD `f2887896`, all tracked `src/include/cross`
inputs and the five ABI-matched images. All four canonical full builds pass;
the runtime image set is unchanged from H287. Two fixtures are compiled with
the same current retainer header and target-specific llvm-mingw compiler.
`fixture-build-r1.json` records commands, dependency/config hashes and binaries.

The fixture loads the frozen `d3d9.dll`, verifies PE machine, executable-section
RVA bounds and each entry's first 16 bytes, and calls its actual
`RetentionIndex::rebuildFrom`, `endEpoch` and resource C wrappers. The canonical
conformance runner stages the image set into Sikarugir `sikarugir-cx-24.0.7`;
each process reconciles the full staged-image hashes before and after running.
No source substitute is used for the timed methods.

The fixture constructs a genuine `D3D9PePendingCommandRetainer`, not a mirror
of its private vectors. Compiler-emitted record layouts locate the actual
private subobjects:

| PE width | Retainer bytes | `entries_` offset | `index_` offset |
|---|---:|---:|---:|
| 32 | 56 | 0 | 12 |
| 64 | 96 | 0 | 24 |

PE32 uses the explicit lowered `thiscall` convention; PE64 uses its unified
member-call convention. This is an exact-build internal-ABI experiment, not a
portable API or a production/test access seam. Retainer capacity 1024 reserves
2048 index slots. At most 222 entries are used: loaded rebuild reuses storage,
and expiry only shrinks/compacts it. No vector allocation is transferred across
the EXE/DLL allocator boundary. The compiler executable itself is not hashed;
the receipt binds commands, emitted layout, dependencies and output images.

All resources are real opaque C-ABI objects: 222 separate 64-byte vertex
buffers and one 64×64 texture. The caller holds base references until retainer
use ends. Buffer/texture, device and factory cleanup must return zero before
the pass marker. There is no fake resource dereference or Metal call from PE.
No draw is submitted, so this is not a GPU lifetime or pixel oracle.

## Cases and denominators

Each width has one small smoke and two separate longer processes. Every long
process runs all six index sizes for 1,000,000 calls each after 1000 warm calls,
two epoch cases for 5000 samples each, and 1,000,000 non-final releases per
resource kind. Resource setup, addref prepayment, membership checks and cleanup
are outside the timers. The product PE observer and perf counters are OFF.
Processes run serially with no benchmark or build alongside.

- Index rebuild: fixed 2048-slot table and 0/6/23/100/128/222 buffer entries.
  Fieldwise snapshots and all memberships must be unchanged.
- Tombstone epoch: retain 124 buffers, advance once, append and roll back a
  spare before timing. The loaded epoch must rebuild with 124 survivors and
  zero expiry. `projectRetainerEpoch` must report `FastTombstoneRebuild`.
- Expiry epoch: of 124 entries, 24 interleaved cold entries expire and all
  100 hot survivors move during stable compaction. Projection conservation and
  each live/expired membership are checked after the loaded epoch.
- Release: repeatedly release one object after prepaying references. The
  returned counts must sum to `N(N+1)/2`, end at one, then reach zero in untimed
  final cleanup. This excludes final destruction and is not a mixed-resource
  or COM `Release` measurement.

Index and release batches use one QPC interval per case, including the loop,
indirect call and checksum/oracle overhead. Epoch samples each have their own
QPC pair, with a separate empty-pair calibration. QPC is 10 MHz in these runs;
no calibration is subtracted. These are elapsed times, not exclusive CPU.

## Repeated warm prices

Values below are the arithmetic means of two process means, in microseconds
per operation. The smoke is excluded. `final-audit.json` retains each repeat.

| Loaded operation | PE32 | PE64 |
|---|---:|---:|
| Rebuild, 0 entries | 1.296 | 1.167 |
| Rebuild, 6 entries | 1.318 | 1.184 |
| Rebuild, 23 entries | 1.387 | 1.221 |
| Rebuild, 100 entries | 1.705 | 1.365 |
| Rebuild, 128 entries | 1.834 | 1.447 |
| Rebuild, 222 entries | 2.228 | 1.657 |
| Tombstone epoch, 124→124 | 1.984 | 1.480 |
| Interleaved expiry epoch, 124→100 | 5.583 | 2.150 |
| Non-final buffer release | 0.132 | 0.022 |
| Non-final texture release | 0.133 | 0.024 |

PE32 100-entry rebuild repeats are 1.709/1.701 microseconds; expiry repeats
are 5.581/5.584. PE64 counterparts are 1.365/1.365 and 2.137/2.163. Empty
epoch QPC pairs average 130–134 ns on PE32 and 41–43 ns on PE64. The table
does not subtract them or assert statistical confidence from two processes.
Different PE widths and warm resource patterns are not interchangeable with
the PE32 game's in-situ distribution.

The non-final release loops are especially warm and repeatedly touch one
object. Epochs release distinct buffers and include dispatch, scan, moves and
rebuild. Subtracting these experiments does not isolate a production component
or establish that their means add exactly. H287 also lacks per-kind/final-
release and pointer/collision distributions. Its diagnostic QPC epochs remain
a separate observation; the difference is not assigned to Wine, the allocator
or diagnostics without matching evidence.

## Decision and remaining gate

The fixed-table reset is a real PE cost: even an empty explicit rebuild costs
about 1.30 microseconds. However, substituting this warm 100-buffer rebuild at
every H287 epoch event gives only 26.91/32.10 microseconds per Present at
15.783/18.825 events per Present. This is a hypothetical scale check, not
workload weighting, an upper bound, measured CPU usage or removable time.
Any replacement must still maintain the index and can add hot lookup costs.

Keep the current implementation. An incremental index needs bounded reference
equivalence for compaction, rollback, collisions and wrap, plus actual-PE
lookup/admission and epoch economics; rebuilding faster alone is insufficient.
Any release-batching proposal additionally needs real per-kind/final-release
attribution and unchanged lifetime/ordering semantics. Neither change is
justified as an FPS recovery by these component prices. Broader owner admission
and repeated wire/state work remain higher-priority profiling targets.

All six actual PE processes pass the nonempty component and cleanup oracles;
all four long runs agree on semantic checksums. Sixteen parser pins reject
missing/duplicate cases, wrong machine/layout/slot counts, bad denominators,
release/projection conservation failures and failure markers. `final-audit.json`
revalidates full logs, six runtime receipts, both fixture builds and unchanged
production sources/images. The fixture and raw artifacts are ignored local
evidence, not a new installed harness API or new TLA+ proof.

No fresh game run is needed to compare unchanged product code, and H288 makes
no new FPS claim. H287's separate diagnostic-OFF 05/06 GT2 spots remain
30.036/18.228 FPS, excluding the earlier user-interfered run. Their stated
health-coverage and single-run limitations still apply. README is unchanged;
repeatable whole-PE CPU economy and durable FPS recovery remain open.
