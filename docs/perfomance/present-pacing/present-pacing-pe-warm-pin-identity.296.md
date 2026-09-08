---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Warm physical-pin identity materialization"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/pe-owner-economy-H296-20260909/analysis
related: present-pacing-pe-compact-identity-insertion.295.md; present-pacing-pe-public-com-parent.294.md
---

# Warm physical-pin identity materialization

H296 starts at `1fe09554`. The sole product edit moves typed identity
materialization out of the existing-pin branch of
`pinPreparedQualifiedBinding`. The warm branch compares the same qualified
kind, generation, object ID and object pointer directly. The new-pin branch
still captures its identity before retain, then publishes that snapshot.
No cache, pin authority, transport, retention or scheduling policy changes.

R-CORE-REC-7.36 explicitly requires the pre-retain snapshot. Its bounded native
fixture covers seven compact binding families, four source-mutation forms,
late retain-callback mutation, preserved-prefix rollback, retry and warm reuse.
The warm repeat also compares exact generic-builder bytes and segmented roles,
while checking all seven reference counts and the retained count. Query is not
a compact sparse binding family. These tests are not an arbitrary reentrancy
proof; existing lifetime transitions remain unchanged.

## Linked implementation

All four canonical builds complete. Host native checks pass 8/8 and the
x86_64-native subset passes 4/4, including install-name validation. The existing
`dxmt9-verify-tla` regression passes in 149.98 seconds; no new TLA model or
scheduling transition is claimed.

Frozen full-image hashes, source snapshots and build-option/coredata identities
bind baseline and candidate. Only the owner header changes under `src`,
`include` and `cross`. Both bridge DLLs and the Unix provider are unchanged.

| Linked scope | Baseline | Candidate |
|---|---:|---:|
| PE32 five pin function spans | 480–496 bytes | 432 bytes each |
| PE32 raw linear instructions | 165–171 | 136 each |
| PE64 five pin function spans | 528 bytes each | 528 bytes each |
| PE64 raw linear instructions | 147 each | 143 each |

These are next-text-symbol-bounded spans and raw instruction counts, including
alignment; they are not cycles or reachable-path counts. The selected compact
admission/append, qualified writer, public DrawPrimitive and H295 identity
insertion control retain their span/count metrics. The audit records explicit
selection among append overloads; it does not claim every parent is unchanged.

Main inspected the PE32 buffer instantiation: baseline identity fields were
spilled before or within the warm branch. Candidate compares binding fields
directly, with only its pin ordinal spilled on that path. New-pin identity
storage remains before retain. The PE32 stack subtraction grows from 28 to
32 bytes despite the warm-work reduction. PE64 avoids preloading the identity
before the warm/cold branch and reduces its stack subtraction from 72 to 56
bytes, retaining eight register pushes and the total function span. There is
no justified whole-parent CPU gain from these codegen observations alone.

## Runtime qualification

Twenty actual PE processes pass: four boundary traces and sixteen logging-OFF
timing processes, four per version/width. The measurement uses H294's frozen
public COM fixture, not private-entry calls or a matched-header surrogate.
Four traces establish no timed recorder flush. Each process checks twelve
getter/FFP-pixel rows; QPC covers setter-only or setter-plus-draw groups, not
exclusive pin CPU. Setup, warmup, query drains and readback are outside QPC.
The fixed recipe is 64 groups of 32 iterations, eight warmup groups, 256
records/1,310,720 bytes, replay offload on, and inline/segmented/tape/index-cache,
PE diagnostics and perf counters off. PE32 runs under Sikarugir WoW64/Rosetta;
PE64 is a separate width, not native ARM64 PE.

| Setter + draw mix | PE32 baseline/candidate ns | Delta | PE64 baseline/candidate ns | Delta |
|---|---:|---:|---:|---:|
| Steady | 465.70 / 469.43 | +0.80% | 306.93 / 307.64 | +0.23% |
| Viewport | 514.92 / 509.23 | -1.10% | 344.82 / 345.68 | +0.25% |
| Viewport + scissor | 537.16 / 556.18 | +3.54% | 374.27 / 386.47 | +3.26% |
| Four scalar states | 900.78 / 871.29 | -3.27% | 538.45 / 555.81 | +3.22% |
| Stream offset | 647.92 / 589.45 | -9.02% | 420.39 / 427.29 | +1.64% |
| Six constant registers | 1,932.32 / 1,813.18 | -6.17% | 1,153.54 / 1,268.48 | +9.96% |

Values are medians of four process-level QPC aggregate values. All twelve
setter-plus-draw min/max ranges overlap; this is not a neutrality proof. The
initial two-repeat PE64 scalar/stream/constant penalties of 10–15% triggered
two further repeats per cell with reversed/interleaved version order. The
constant median remains a warning, not an attributable helper penalty or a
confirmed regression. Setter-only results are also mixed; the non-overlapping
PE32 viewport/scissor difference is about 0.46 ns (+1.33%). No workload
weighting, subtraction of empty-loop cost or aggregate speedup is claimed.

The API CPU totals are 450–490 ms candidate versus 460–480 ms baseline on
PE32, and 440–490 versus 440–480 ms on PE64. These are the runtime's coarse
process-time fallback, including setup/warmup/drains, not exclusive PE/thread
CPU. They do not establish lower CPU usage.

Both-width actual pixel baseline/candidate runs pass the exact four-function
verdict. Each produces 24 signatures of four samples, with candidate samples
exactly matching baseline. The fixture is bounded; it does not prove every
game frame, shader layout or concurrency schedule.

Historical runtime hashes belong to their own run, not to the shared Wine
destination after another version is staged. Each timing receipt preserves
full post-run destination identities, source images, fixture/build/source
receipt identities, driver/parser hashes and dynamic public COM vtable/RVA
binding. No build, native test or second benchmark overlaps these runs.
README remains unchanged.

## GT2 spot checks and disposition

One candidate run per product uses the canonical Sikarugir catalogue runner,
perf/warn profile, validation off, frame sampling on and PE observers off.
Replay offload is on; inline, segmented, tape and index-cache are off. The
05 focus-keepalive helper is explicitly disabled. No manual window operation,
build, test or second benchmark overlaps either measurement.

| Product | Resolution | Consecutive scene frames | Scene wall | Sampled FPS | README delta |
|---|---|---|---:|---:|---:|
| 05 GT2 | 1024x768 | 1–1934 (1,934) | 64,594.877 ms | 29.940 | -0.53% vs 30.1 |
| 06 GT2 | 1280x720 | 989–2157 (1,169) | 63,997.279 ms | 18.266 | +0.92% vs 18.1 |

Positive-wall rows with more than 100 draws select a consecutive scene while
excluding loading; slow scene frames are not trimmed. This is elapsed scene
FPS, not the process-elapsed estimate or an observer-free benchmark score.
The 05 run exits normally and its `.3dr` is recovered. 06 Advanced Edition
produces no `.3dr`; its post-scene GUI timeout/143 is permitted by the catalogue.

All eight reported chunk/GPU/pipeline health counters are zero. 05 has a full
snapshot through frame 1935. 06 has one through frame 2100, leaving 57 final
scene frames without the full health snapshot. The last per-frame GPU-error
samples are not a substitute for those missing categories. Captures show
rendered scenes, not a black window; neither a screenshot nor the bounded
pixel fixture proves full-scene temporal correctness.

The candidate PE32/PE64 image SHA-256 values are
`2bacf683fe3e2d972317a7e25eb5208a0a0f60166f6490be84ec485142d01ea5` and
`fb2489ca003d85f116d211dc7897959ea20122693fa2180b443bb60e51879ec0`.
Both canonical images, both bridges and the Unix provider match the frozen
candidate and shared Wine root after both runs. Recipes, captures, recovered
result, logs, frame tables and their hashes remain in the ignored artifact
tree; `final-audit-main.json` binds the full evidence chain and this text.

Retain the bounded local work removal after independent semantic/economic
review, without performance promotion. Four-repeat whole-parent timings are
mixed, PE64 constant cost remains a warning, and CPU usage reduction is not
established. Near-README single-run FPS is a spot check, not a causal gain or
neutrality proof. Narrowing H295's dummy outlined-helper arguments and better
attributing whole-parent variation remain separate future work.
