---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE record-local identity distribution: retain the small linear path"
type: evidence
status: diagnostic-only
updated: 2026-09-06
source: experiments/output/app-d3d9-3dmark05-pe-identity-706866a4-n127-20260906; experiments/output/pe-constant-transition-20260906/analysis/head-706866a4-pre-specialize.json; experiments/output/pe-hot-specialization-20260906/analysis/candidate-projection-linked.json; experiments/output/app-d3d9-3dmark05-pe-hot-specialization-gt2-r2-20260906
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-compact-refinement.247.md
---

# PE record-local identity distribution

## Decision

Keep the production linear identity fold. The maximum-capacity native fixture
requires 990 comparisons for 45 distinct identities, but this GT2 sample has
only 2.588 unique identities and 4.494 linear comparisons per measured compact
preparation. These counts do not establish that hashing would be faster, nor
do they rule out a useful specialization for a different workload or tail.

R-CORE-REC-7.39 is exercised by a real workload without changing identity,
retention, wire order or transport. Fixed-width constants and lexical source
qualification are separate implementation work under R-CORE-REC-7.40–7.41.
This run predates those changes and cannot validate their runtime behavior.

## Provenance and population

- Linked artifacts are the previously built and verified `706866a4` set.
  Source edits were in progress, but no rebuild occurred before or during this
  run. The staged PE32/PE64 hash prefixes are `26a520dbbcbf319e` and
  `153f09849ec24681`; the source commit alone is not the artifact proof.
- Named `gt2`, 1024x768, Sikarugir `sikarugir-cx-24.0.7`, perf profile with Info
  logging, `DXMT9_PE_STATS_DECIMATION=127`, semantic-owner phase split enabled,
  frame sampling enabled, no encoder breakdown or GPU trace.
- Runner pass, exit 0, no timeout; 1,674 encoded Presents. Chunk rejects,
  GPU command-buffer errors and no-pipeline draw skips are zero. This is
  harness health, not a new user-confirmed visual or pixel-oracle result.
- The last cumulative observer group covers 1,620 Presents; the final 54 are
  outside this observer population. There are 27 groups / 81 family rows;
  cumulative rows must not be summed together.

| Family | Measured preparations | Mean unique identities | Mean comparisons | Sampled max unique / comparisons | Unmeasured failures |
|---|---:|---:|---:|---:|---:|
| Compact | 21,755 | 2.588 | 4.494 | 17 / 136 | 0 |
| Generic | 124 | 0.065 | 0.065 | 1 / 1 | 0 |
| Constant | 12,857 | 0 | 0 | 0 / 0 | 0 |

Compact exact sums are 56,574 reference occurrences, 56,297 unique identities
and 97,775 comparisons; 1,922 measured preparations have zero work. Generic
has 1,248 reference occurrences but only eight unique identities/comparisons:
null reference occurrences must not be mistaken for non-null lookup work.

## Limits and next gate

Rows explicitly report `identity_distribution=reconstructed_sampled_only`.
They reconstruct successful preparation from the production witness, not CPU
instructions or time. The observer emits sums and maxima, not a histogram or
percentiles. Failed preparation is unmeasured; CapacityPre retry is a distinct
attempt. Reconstruction is outside PrepareAdmission timing but inside its
inclusive admission parent, so the latter is perturbed.

No FPS claim is made from this instrumented run. An indexed replacement needs
actual tail/ordering evidence, a measured small-set crossover, native
null/duplicate/alias/generation/capacity/fault and canonical-byte equivalence,
and matched observer-off throughput evidence. Until then a per-record hash
table would add unpriced initialization and memory work to a small population.

## Associated implementation evidence

R-CORE-REC-7.40–7.41 are implemented on top of `706866a4`, independently of
the identity-strategy decision. All six fixed-width setters use the same
table-derived byte-write projection, checked at compile time for the eight
extent/equality/pending combinations. The actual linked PE32/PE64 loops use
fixed-width loads/stores with no per-register compare/copy call, diagnostic
clock or runtime transition lookup. Allocation and copy on vector growth
remain; native warm-storage tests establish only the reserved case.

Compact Draw rebase fuses the adjacent source/ticket qualification with
destination resolution. Concrete copied tickets avoid a new invalidating
callback; post-retention checks and CapacityPre requalification are preserved.
APPLY_STATE and admission/emission/settlement checks are not removed. Static
total instruction counts include cold branches and are not executed-work
counts or proof of a net CPU reduction.

All four canonical builds, 28 native PE tests, eight x86_64 provider-focused
tests and the unchanged TLA suite pass. Fixed-width observer populations and
reference equality pass at N=1 and N=127. Exports and vtable/RTTI ownership
match the baseline; PE32/PE64 DLL size grows by 6,144/5,632 bytes. Final linked
hashes are `8e39e944acd246f6...` / `0d37fc394ec6edf2...`, recorded fully in
`experiments/output/pe-hot-specialization-20260906/analysis/candidate-projection-linked.json`.
Intermediate linked receipts deliberately remain separate: `candidate-final`
predates the compile-time effect projection and is not the final build.

The final-build GT2 run
`app-d3d9-3dmark05-pe-hot-specialization-gt2-r2-20260906` passes: exit 0,
no timeout, 1,876 Presents, zero chunk rejects, GPU command-buffer errors and
no-pipeline skips. Staged hashes match the final linked receipt; the Unix
provider and both bridge DLLs remain byte-identical to the identity run.
The benchmark `.3dr` is captured with its digest in `result.json`.

The renderer's positive-wall scene sample is **29.048 FPS**, from 1,875 frames
over 64,547.602 ms. Conditions: named GT2, 1024x768, Sikarugir
`sikarugir-cx-24.0.7`, perf/Warn, frame sampling ON, PE decimation and phase
split OFF, no encoder breakdown or GPU trace. This is not the process-elapsed
FPS or a decoded `.3dr` score. There is no matched observer-off baseline or
fresh user-confirmed visual/pixel oracle, so this establishes run health,
not an attributed speedup. The earlier `r1` ran the pre-projection binary;
it is retained as intermediate evidence, not a repeat of the final build.
