---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE prepared emission and compact producer integration"
type: evidence
status: partial
updated: 2026-09-05
source: src/d3d9/d3d9_pe_semantic_owner.hpp; experiments/output/app-d3d9-3dmark05-gt2-perf-prepared-emission-{base-off,base-on,candidate-off,candidate-on}-20260905
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-admission-reuse.243.md
---

# PE prepared emission and compact producer integration

R-CORE-REC-7.14–7.16 target repeated sparse layout work, generic constant
admission, and duplicate binding-row storage within the sole production
semantic owner. Compact source integration must not revive a second production
builder or bypass the recorder transaction. Implementation, native/formal
checks, release builds, and GT2 direction runs are complete. Independent
integrated review found no concrete production-reachable blocker; these runs
do not establish an FPS promotion.

## Matched baseline

Both cells use clean `8bccbb57` binaries, Sikarugir
`sikarugir-cx-24.0.7`, GT2, 1024x768, `perf`, `DXMT_LOG_LEVEL=info`, and
`DXMT9_PE_SEGMENTED_TRANSPORT=0`. OFF selects stats decimation / phase split
`0/0`; ON selects `127/1`. No build, TLA, copy ledger, or GPU trace overlaps
the runs. FPS is the benchmark-owned `.3dr` value.

| Output suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `gt2-perf-prepared-emission-base-off-20260905` | 1,681 | 26.872 | 3.9994 | 15.7615 |
| `gt2-perf-prepared-emission-base-on-20260905` | 1,600 | 25.564 | 3.9994 | 15.7756 |

Every suffix has the `experiments/output/app-d3d9-3dmark05-` prefix.
Both cells pass with zero chunk rejects, GPU command-buffer errors, and
no-pipeline skips. Loaded artifacts are PE32 `e414289ac684d7c5`
(1,756,672 bytes), PE64 `af8fe021a08fa6c6` (1,624,064 bytes), Unix
`f7935d35db31eb12` (9,779,896 bytes), bridge PE32 `e31242c3d6c67e9a`
(140,800 bytes), and bridge PE64 `cc46aabe66969513` (145,920 bytes).

The same PE32/PE64 binaries previously measured 27.527 OFF FPS in H243.
The new 26.872 cell is about 2.4% lower without a code change: ambient/run
variation is material. Neither an isolated before/after FPS difference nor
the August PE sampling share establishes a current performance ceiling.

## Baseline phase population

| Phase | Calibrated ns/event |
|---|---:|
| Prepare admission | 424.0 |
| Fixed/direct pins | 145.7 |
| Sparse/variable copy | 397.6 |
| Canonical materialization/metrics | 362.5 |
| PendingDelta settlement | 148.9 |
| Settle/clear (per chunk) | 33,839.6 |
| ExactFixed inclusive (per chunk) | 6,002.9 |

The empty clock-pair mean is 196.1ns, subtracted from each sampled mean.
Append events/samples are 4,237,530 / 33,366; chunk events/samples are
25,006 / 196. The final phase snapshot is near decimated Present 1,560,
whereas the run completes 1,600 Presents. Prefer per-event values; whole-run
Present normalization undercounts the unreported tail. The inclusive append
parent (3,675.5ns/event) contains child observer work and is non-additive.

## Candidate direction evidence

The frozen candidate uses the same settings and runtime. All five artifacts
were freshly staged before OFF; ON reuses those exact artifacts. PE32 is
`245b8541d56f7fb7` (1,835,008 bytes), PE64 is `0a8fa399284b0a0d`
(1,705,472 bytes); both bridge DLLs and the Unix provider match baseline.
Only the read-only independent source review overlaps the candidate runs;
no builds, native/TLA tests, other Wine runs, or GPU trace overlaps them.

| Output suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `gt2-perf-prepared-emission-candidate-off-20260905` | 1,684 | 26.919 | 3.9994 | 15.8076 |
| `gt2-perf-prepared-emission-candidate-on-20260905` | 1,674 | 26.748 | 3.9994 | 15.7688 |

Both pass with zero rejects, GPU errors, and no-pipeline skips. The retained
end-scene screenshots show the expected forest scene, but this is a limited
visual smoke, not a full-frame pixel or all-workload oracle. OFF FPS is
26.872 → 26.919 (+0.18%): effectively neutral. ON is 25.564 → 26.748
(+4.63%), which is diagnostic direction rather than a production gain.
The OFF pass rate rises 0.29%; the ON pass rate falls 0.04%. They remain near
the same locality shape, but a strict non-increase promotion is not proved.
OFF records/bytes per Present move 2,702.8/1,270,500 → 2,710.3/1,274,787;
ON moves 2,700.0/1,268,875 → 2,702.5/1,270,428. Time-stepped populations
are not byte-identical traces; exact cadence/value refinement belongs to the
native fixtures, not a rounded per-Present runtime comparison.

| Phase | Baseline calibrated ns/event | Candidate calibrated ns/event |
|---|---:|---:|
| Prepare admission | 424.0 | 341.2 |
| Fixed/direct pins | 145.7 | 24.3 |
| Sparse/variable copy | 397.6 | 371.8 |
| Canonical materialization/metrics | 362.5 | 305.5 |
| PendingDelta settlement | 148.9 | 255.2 |
| Five admission children, sum | 1,478.6 | 1,298.0 |
| Settle/clear (per chunk) | 33,839.6 | 26,799.7 |
| ExactFixed inclusive (per chunk) | 6,002.9 | 6,069.3 |

The candidate empty pair is 205.2ns; append events/samples are
4,411,479/34,736 and chunk events/samples 25,915/204. The final phase
snapshot is near decimated Present 1,620 of 1,674 completed. Five child
phases fall about 12.2% in this population, while settlement increases and
the inclusive append parent remains about 3.70us versus 3.68us. Child
sampling, calibration, execution mix, and observer overhead prevent treating
these values as additive product CPU time or assigning the difference to one
of the three implementation changes. ExactFixed remains small here.

The supported result is reduced duplicate representation and some lower
sampled admission phases, with no established production FPS recovery.
PendingDelta settlement, generic/plan stack probes, bounded identity dedup,
pin-index clearing, and warm retention remain measurable candidates. This
does not show that PE as a whole is finished or explain the complete
post-v0.1.1 regression.

## Implementation and verification boundary

Prepared section descriptors supply canonical offsets and quota deltas once.
Seven duplicate binding arenas are removed; qualified occurrence ordinals
connect source rows to prepared pins and wire indices, and committed visitors
read canonical metadata. Normal DrawPrimitive, DrawIndexedPrimitive, and delta
APPLY_STATE use the recorder-bound compact source plan through the same owner;
UP/full-snapshot/explicit sparse inputs remain on its generic adapter. All six
standalone constant families use a closed input and small prepared witness.

All four canonical builds pass. Eighteen focused native specs and x86_64
owner/batch/producer differential plus install-name checks (4/4) pass after
integration and the codegen correction below. The six constant kinds compare owned A/B/A
bytes, source mutation, quota failure, settlement rollback/retry, inherited
count/byte acceptance, failure precedence, and observer phase populations
against the generic owner. The physical ledger expectation now excludes the
removed binding-row storage rather than charging nonexistent materialization.
The seven-family binding matrix covers duplicates, null/inactive references,
retained-only promotion, exact/segmented bytes, lease visits, rollback and warm
retry. Producer differential tests cover nonzero all-category plans, ordinary
and explicit full-snapshot selection, CapacityPre re-finalization, stale
PendingDelta/epoch, and a same-number epoch from a different recorder. Failed
partial preparation revokes borrowed authority through RAII.

The final existing `dxmt9-verify-tla` protocol suite passes in 155.19 seconds.
No TLA model changed: this checks existing acceptance/settlement transitions,
not new canonical bytes or compact-plan integration by itself. Native
refinement supplies that bounded value/ownership evidence, not Wine COM,
all-family fault behavior, or final pixels.

## Release codegen boundary

The initial integration merged a 5,208-byte prepared-witness frame into the
PE32 family dispatcher, causing `__alloca` stack probing even on constants.
The two input-specialized observed/unobserved admission bodies now remain
out of line. The constant bodies reserve 120/100 bytes respectively and have
no stack probe. Generic and compact-plan bodies still reserve about 5 KiB
(5,128/5,112 bytes) and probe; this is a real remaining cost. The common
transaction, callbacks, and observer outcomes are unchanged.

Constant staging and active plan revocation have no external memcpy/memset
call in the selected PE32 disassembly. Full plan initialization occurs only
when building a new plan. These codegen facts do not establish zero copy in
the entire call chain or a runtime gain. The additional occurrence/layout
witness trades call-local stack for removal of persistent duplicate rows.

## Independent review and remaining evidence

The read-only integrated review covered production draw/apply/constant
routing, recorder capability/ticket lifetime, CapacityPre rebase, indexed
receipts, failure/reset/poison revocation, prepared serializers, typed
retention/rollback, canonical visitors, and the modified native tests. It
found no concrete production-reachable correctness blocker. The review
worker was released after its completion report.

The native tests invoke the production owner API but do not dynamically drive
the complete `D3D9DeviceImpl` build-plan/arm/append envelope. The compact-owner
byte oracle has a dense nonzero all-category case and focused rebase/rollback
negatives; the broader randomized compact-plan corpus still ends at
`CommandChunkBuilder`. Compact-owner FVF and repeated destination-rebase byte
differentials remain useful extensions. The compact layout helper also relies
on `buildSparseStatePlan` to validate constant range end bounds; a manually
forged plan can bypass that producer check. This is an owner-API hardening gap,
not a production-reachable failure in the reviewed routing. These limits must
not be presented as complete malformed-plan, Wine, or pixel proof.

Baseline objects, generated selected release disassembly, phase summaries,
and the TLA receipt are retained under the baseline OFF artifact `analysis/`.
Candidate source patch, native receipts, final codegen, and the pre-outline
dispatcher are retained there as well. The source baseline is `8bccbb57`;
HEAD advanced through `c72c311a` and `ab1a458a` via documentation-only
commits during this work. No stable FPS gain or transport promotion is established.
