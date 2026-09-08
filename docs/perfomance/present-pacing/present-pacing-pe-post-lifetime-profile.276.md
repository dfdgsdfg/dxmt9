---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Post-lifetime PE attribution and deferred guard/cache candidates"
type: evidence
status: measured-attribution-no-claimed-gain
updated: 2026-09-08
source: experiments/output/pe-post-lifetime-profile-20260908/analysis
related: present-pacing-pe-cached-com-lifetime.275.md; present-pacing-pe-constant-loop.267.md; present-pacing-pe-native-thread-cpu.268.md
---

# Current-image PE attribution

## Decision

The producer remains the useful CPU investigation boundary, but it is not
synonymous with the PE recorder. Fresh `a3e7aa20` stopped-PC samples place
`d3d9.dll` at about 10%/6% of the observed 05/06 thread populations. The game
and bridge own larger populations. Inside PE, constant handling and semantic
owner admission/materialization remain visible; warm Present getter removal
is not a demonstrated high-cost candidate. No renderer code, runtime default,
ABI or README FPS value changes in this increment.

Main verified no-op full builds in all four canonical directories, then the
sampler and native-thread observer's four focused tests. The analysis root
freezes all five runtime images and a matching PE32 symbol image. Both PE
widths are current, but these benchmark processes exercise PE32. Main alone
stages and runs Wine. No build, test or second benchmark overlaps a scene.

## Stopped-PC evidence

Separate named GT2 runs use Sikarugir cx-24.0.7, perf with explicit Info
logging, frame sampling and full 250-Hz self-PC reports. Resolutions are
1024x768 (05) and 1280x720 (06). Replay offload is ON; inline constants,
segmented transport, CPU-ready Tape and index reorder are OFF. These runs
suspend the target thread and are **not FPS or CPU-consumption evidence**.

| Product | Reports / last report Present | Cumulative thread / PE samples | Full exported buckets | Last scene frame |
|---|---:|---:|---:|---:|
| 05 GT2 | 31 / 1,860 | 13,569 / 1,352 | 638 | 1,866 |
| 06 GT2 | 35 / 2,100 | 14,101 / 803 | 450 | 2,143 |

Every report has zero omitted/overflow samples and zero suspend/context/resume
failures; no target-thread mismatch occurs. All five staged-image identities
match the freeze. The PE32 image SHA-256 is
`2c6dc8c97dad3208f3db23760a51b1e957dc8aaa60ed8b5d99e3f7619a4f356b`.
`qualify_profiles.py`, `join_profiles_main.py` and `symbols-qualified.json`
preserve coverage and frozen-image symbol joins.

06's first heavy frame is 1,009. Subtracting the report at Present 960 from
the report at 2,100 leaves 13,375 thread / 798 PE samples. This includes the
small pre-scene gap after 960 and excludes the final unreported tail; it is
an approximate scene prefix, not exact phase isolation. 05 uses its cumulative
prefix through 1,860. All module rows sum to their corresponding populations.

| Module | 05 prefix samples | 06 prefix samples |
|---|---:|---:|
| Game executable | 9,381 / 13,569 (69.14%) | 10,561 / 13,375 (78.96%) |
| `winemetal_dxmt9.dll` | 1,564 (11.53%) | 1,171 (8.76%) |
| `d3d9.dll` | 1,352 (9.96%) | 798 (5.97%) |

These are PC-stop shares, not exclusive CPU-time fractions. Bridge PCs do not
by themselves separate wrapper work, a transition or a synchronous wait.
Do not multiply these fractions by the separate CPU runs below to invent
PE-exclusive milliseconds or an FPS ceiling.

Selected unambiguous **scene-prefix** symbol buckets:

| Symbol | 05 | 06 |
|---|---:|---:|
| `ValidatedConstWrite<16>::applyNonEmpty` | 74 | 36 |
| `SetVertexShaderConstantF` | 48 | 34 |
| `accumulateRetentionDelta` | 59 | 24 |
| `writeSparsePlanPayloadQualified` | 34 | 22 |
| `preparePeSemanticSparsePlanLayout` (body, excluding its lambda) | 31 | 13 |
| `buildSparseStatePlan` | 29 | 22 |
| `acceptSparseStatePlan` | 23 | 16 |
| Prepared identity witness `add` | 20 | 22 |

The join leaves 338/178 cross-symbol samples unassigned. Inlining, interior
padding and 64-byte buckets limit attribution; lexical family labels are not
inclusive call-tree costs. The public backbuffer getter has 0/1 named samples,
which neither proves zero cost nor prices an optimization.

## Evidence preservation: late launcher output

The first 05 log hash changed after the runner returned. The existing launcher
leaves its 120-second focus helper alive after the benchmark exits. It appended
51 missing-window AppleScript errors (8,772 bytes) and overlapped the later
06 **diagnostic**, not another benchmark process. The original qualified
7,330,814-byte prefix still matches its exact SHA-256. No renderer/sample row
was altered. `late-log-tail-audit.json` records both hashes and the byte-exact
prefix proof; the original qualification remains preserved, and a second
qualification describes the stable complete log. Neither profile is used for
economy claims. This observation does not certify older raw-log hashes.

The separate 05 CPU run explicitly uses
`DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC=0`, retaining startup Enter behavior but
disabling repeated focus calls. The helper is gone before the next CPU run.
This recipe difference must not be hidden in historical A/B comparisons.

## Separate native-thread CPU evidence

Two further runs use the same frozen runtime images and provider settings,
Info logging and frame sampling, with the stopped-PC sampler and detailed PE
probes disabled. The external helper selects the unique native
`unix_commit_chunk_entry` thread. Each fresh log has exactly one canonical
transport-1 negotiation and one device/native-TID pair. PID/start identity,
monotonic cumulative counters, selector/log identity and query bounds pass.
`cpu-{05,06}-qualified.json` independently recomputes the raw envelopes and
same-iteration process pairing; no unstable interior interval is dropped to
manufacture savings.

| Product | Scene frames / wall ms | CPU-covered frame increments | Commit-thread CPU ms/frame | Occupied cores | Same-iteration process CPU ms/frame |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,932 / 64,539.713 | 1,875 (97.10%) | 31.040 | 0.950 | 69.605 |
| 06 GT2 | 1,160 / 64,060.293 | 1,126 (97.15%) | 52.509 | 0.966 | 95.346 |

05 uses one 61.279-second native envelope containing 57.780610 s user and
0.418687 s system CPU; 06 uses 61.223 seconds with 58.870850/0.254190 s.
Each joins 31 observer iterations, with zero endpoint frame offsets between
process and native queries. The two clock APIs have a stable nonzero epoch
offset; interval durations agree within 0.1 seconds. Partial log-frame
endpoints remain approximate, not atomic Present boundaries. Process CPU
includes all threads; native CPU includes game, Wine and synchronous Unix,
**not PE-exclusive CPU**. The near-full native core is measured mostly user
work, not proof that a particular PE helper dominates.

Both runner results pass. 05 exits normally; 06 reaches the complete scene
(frames 1,036–2,195), then uses the catalogue-allowed result-GUI timeout. Eight
reported error counters are zero. 05 health covers its logged scene; 06's
last counter Present 2,160 leaves 35 tail frames unproven. The stopped-PC 06
run separately leaves 43 tail frames unproven. Neither counter set is a
whole-scene pixel oracle.

Diagnostic scene FPS is 29.935/18.108. These single Info observations are not
Warn FPS updates, matched optimization A/B evidence or README recovery claims.
CB/frame is 4.001/4.002 and pass/frame 15.786/19.863; summed GPU CB times
1.992/11.910 ms/frame are not GPU critical-path times. All raw recipes,
sampler/CPU logs, frames and hashes remain under the analysis/run roots.

## Candidate audit

1. **Present lookup: deferred, unpriced.** Ordinary `Present` calls public
   `GetBackBuffer`, which queries present parameters once at device level and
   again in the cached swap-chain wrapper. It also performs public COM
   acquisitions and surface validation. The metadata bridge is terminal-only,
   not a replay drain; `chunkBarrierFlush`, ordered Present append and final
   flush are separate obligations and must remain. `cachedBackBuffer0_` is
   cleared by `SetRenderTarget(0)`, so an alias-only shortcut is not a credible
   warm multi-pass path. A future cache-map borrow must use the existing
   canonical maps as its sole authority under recorder ownership, preserving
   first-use/Reset failure, generation, capture and public getter contracts.
   Two fewer crossings per frame have no demonstrated FPS price here.
2. **Scalar size guards: no deletion approved.** The argument that downstream
   `visited` entirely subsumes size checks is false: `rows == 0` skips the
   visitor, so a stale zero-to-nonzero table cannot be detected there. The
   upstream admission guard remains necessary. Removing only the writer's
   duplicate guard might still be justified by upstream qualification and a
   no-mutation interval, but requires that separate dominance proof and PE32/
   PE64 codegen evidence. This audit is not a discovered production rendering
   bug and does not prove the writer-only optimization invalid.

`compactProducerRejectsStaleScalarRowCardinality` adds twelve real compact
producer/owner cases: four scalar table families (render state, TSS, sampler,
transform) times zero-to-one, one-to-zero and one-to-two mutations. A retained
native test-seam Writer deliberately changes cardinality without advancing
the ticket. Each case explicitly proves `plan.validFor(access)` still holds,
then requires `Malformed` preflight, no settlement, unchanged committed prefix
and balanced prefix refs. Rebuilding a one-row plan must append exact generic
builder bytes. This is a bounded ordinary-Draw precondition pin; it is not a
production callback/interleaving failure or a new general concurrency proof.

After all Wine processes ended, Main's native owner/views/differential tests
pass 3/3; x86_64 passes the same tests plus the provider install-name audit
4/4. `native-stale-row-final.log` and `unix-stale-row-final.log` are the final
receipts. The new test changes no runtime code. No new TLA model/run, shader
oracle, mutation-deletion experiment or universal fault proof is claimed.

The next implementation gate is exact work removal in the observed
constant/owner paths, not another unpriced cache. Retention identity, current
source equality and physical pin validity remain distinct authorities. Keep
the H269/H273 failed codegen candidates rejected; smaller source or a lower
bucket count alone does not establish lower CPU cost.
