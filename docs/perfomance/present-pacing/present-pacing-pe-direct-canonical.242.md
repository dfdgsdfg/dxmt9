---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE direct canonical value payload"
type: evidence
status: partial
updated: 2026-09-05
source: src/d3d9/d3d9_pe_semantic_owner.hpp; tests/native/bridge/pe_semantic_owner_spec.cpp; experiments/output/app-d3d9-3dmark05-gt2-perf-direct-canonical-{base,candidate}-{off,on}-20260905
related: specs/d3d9/recorder/requirements.md; docs/perfomance/present-pacing/present-pacing-pe-prepared-reset.241.md
---

# PE direct canonical value payload

R-CORE-REC-7.11 removes duplicate PE value ownership. Scalar sparse rows,
standalone and embedded constants, UP bytes, and Clear rectangles are serialized
directly from synchronous input into the semantic owner's canonical payload.
The seven binding arenas, six qualified pin families, logical admission quotas,
wire ABI, transport selection, and chunk cadence remain unchanged.

The representation change and native byte/ownership checks pass. GT2 OFF/ON
direction checks also pass, but sampled admission timings do not demonstrate
lower CPU cost and single-run FPS does not establish a gain.

## Representation and proof boundary

Previously these values were copied into typed/byte arenas and copied again
into canonical roles during the same admission. No production consumer needed
the intermediate values after canonical materialization. The candidate removes
those physical arrays, their admission copies, and their settle-time clears.
The native production-alias `storageBytes` decreases from 10,514,896 to
8,458,704 bytes: 2,056,192 bytes (about 1.96 MiB) of fixed storage removed.
The retained `analysis/storage_size.cpp` compares the saved before-header with
the candidate using the native compiler; this is not measured process RSS.
It retains logical range counts and byte/category quotas to avoid silently
enlarging chunks. `ownedMaterializedBytes` counts only actual retained record,
pin, binding, and canonical-role extents, not the removed staging.

Private serializers consume `peSemanticSparse(input)` so a prepared destination
overlay remains authoritative. Input spans do not enter persistent storage.
Seal, capture, retry, and segmented emission read committed canonical roles;
resource-hazard and pending-lease visitors use the unchanged qualified pins.
Failed settlement restores the committed prefix and logical frontiers. Empty
Clear payloads skip zero-length copies because the borrowed span may be null.
The removed test-only arena accessors are replaced with canonical-byte checks.

The baseline includes the preceding uncommitted prepared-reset patch on
`e1e86794`; this experiment is not a clean HEAD-versus-candidate comparison.
Before-image headers and the inherited patch were saved before implementation.
The source before-image was checked against HEAD and contains only that reset
change. Separate Orca workers own source implementation, native refinement,
and independent read-only review; main owns integration and measurements.

Existing recorder acceptance/settlement transitions do not change.
`dxmt9-verify-tla` passes, including expected counterexamples. Wire-value
ownership is instead tested by bounded native differential checks against
`CommandChunkBuilder`: all 21 families, 1,024 scalar masks with distinct nonzero
values, 64 constant masks, every scalar category's quota edge, all eight
length-three A/B value sequences, mixed binding/value records, source mutation,
logical exhaustion, retry, and both emission forms. These are not Wine fault
or Metal pixel proofs.

## Matched run configuration

All cells use Sikarugir `sikarugir-cx-24.0.7`, 1024x768, `perf`, info logging,
frame sampling, and `DXMT9_PE_SEGMENTED_TRANSPORT=0`. No GPU trace or copy ledger
is enabled. Phase OFF uses `DXMT9_PE_STATS_DECIMATION=0` and
`DXMT9_PE_SEMANTIC_OWNER_PHASE_SPLIT=0`; ON uses `127` and `1` respectively.
FPS comes from the benchmark-owned `.3dr`, not process-elapsed estimates.

The baseline runs reuse the previously verified staged binaries with
`--skip-stage`, preventing concurrent source work from triggering a rebuild.
Both baseline cells load PE32 `2592b9e8afe54f74`, PE64 `dc260194575934f3`, and
Unix `f7935d35db31eb12`; bridge hashes are `e31242c3d6c67e9a` (PE32) and
`cc46aabe66969513` (PE64).

| Cell / suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `gt2-perf-direct-canonical-base-off-20260905` | 1,681 | 26.874 | 3.9994 | 15.7781 |
| `gt2-perf-direct-canonical-base-on-20260905` | 1,618 | 25.851 | 3.9994 | 15.7651 |
| `gt2-perf-direct-canonical-candidate-off-20260905` | 1,713 | 27.383 | 3.9994 | 15.7741 |
| `gt2-perf-direct-canonical-candidate-on-20260905` | 1,638 | 26.169 | 3.9994 | 15.7723 |

All suffixes have the `experiments/output/app-d3d9-3dmark05-` prefix.
Both baseline cells report pass, zero command-chunk rejects, GPU command-buffer
errors, and no-pipeline skips. The ON capture helper emitted one window-image
warning but retained `actual.png`; its runner reports no failure. That warning
does not establish a rendering defect or an exhaustive visual pass.

Candidate OFF also passes with zero rejects, GPU errors, pipeline build failures,
and no-pipeline skips. Main inspected its saved scene without an obvious visual
defect; this is a one-frame smoke, not an exhaustive pixel oracle. PE32 is
`3ae1a1a336295310` (1,763,840 bytes), PE64 `6feff32ba15d2f9c` (1,629,696 bytes),
and Unix/bridge hashes are unchanged. Its OFF FPS delta is +1.90%, but the same
baseline binary previously produced 27.435 FPS in the preceding leaf. The
single-run delta therefore cannot establish a stable gain. Candidate ON uses
`--skip-stage`, loads the same candidate binaries, and also reports zero
rejects, GPU/pipeline failures, and no-pipeline skips. Its FPS delta versus
baseline ON is +1.23%. Small normalized pass differences across different
rendered-frame populations are not a new pass-boundary attribution.

| Calibrated ON phase | Base ns/event | Candidate ns/event | Base ms/P | Candidate ms/P |
|---|---:|---:|---:|---:|
| Prepare admission | 389.2 | 411.6 | 1.023 | 1.102 |
| Fixed/direct pins | 96.5 | 167.4 | 0.254 | 0.448 |
| Sparse/variable admission | 400.9 | 369.9 | 1.053 | 0.990 |
| Canonical materialization/metrics | 340.9 | 398.6 | 0.896 | 1.067 |
| PendingDelta settlement | 144.5 | 145.4 | 0.380 | 0.389 |
| Settle/clear | 39,230.4 | 37,408.8 | 0.603 | 0.588 |
| ExactFixed inclusive | 6,230.4 | 6,082.0 | 0.096 | 0.096 |

The last baseline phase snapshot follows Present 1,560; the final total is
1,618. Candidate uses Present 1,620 versus final 1,638. Null clock-pair corrections
are 200.36/190.24 ns/sample. Timers and differing
reporting populations prevent treating these estimates as exact recoverable
CPU time. Inclusive parent/child times must not be added.

Sparse/variable admission improves 7.7% per event, but canonical construction
increases 16.9%; their sum moves 741.8 to 768.5 ns/event (+3.6%). The unchanged
fixed/direct-pin phase also rises, so this pair cannot independently attribute
the timing changes to copied bytes, added count guards, code generation, or
storage layout. The safe conclusion is reduced representation/footprint with
healthy GT2 execution, not a demonstrated CPU/FPS win. Further optimization
needs matched phase/code-generation evidence rather than assuming that every
removed copy has an equal wall-time payoff.

## Candidate gate

Main passed all four canonical builds, thirteen focused native specs, the
x86_64 owner spec and install-name audit, and existing TLA checks (150.51 s).
Independent review found no concrete behavioral regression. Its stale cursor
comment and zero-valued fixture gaps were corrected; all ten scalar quota
edges and a post-materialization settlement rejection/retry are covered.
Main also corrected a mechanical pure-helper compile error and new-test oracle
integration mistakes (owner-template specificity, invalid UP input shape,
reference multiplicity, and append after oracle seal). None was used as a
passing runtime result. The full integrated tests were rerun successfully.

Orca run `run_548d51432461` contains five completed tasks; the three owned
worker terminals were released after completion or transferred to their exact
follow-up before release. `analysis/` beside candidate ON retains the source
before/after images, inherited and integrated patches, native/TLA logs, and
the summarizer plus all four cells in `comparison.jsonl`. Documentation source
audit and `git diff --check` pass. README and transport defaults are unchanged;
the measured candidate was uncommitted and its exact patch is retained.

No stable FPS gain or broader promotion is claimed. Binding staging, repeated
qualified-pin lookup, and contiguous ExactFixed gather are outside this patch;
the PE owner is not globally copy-free. Broader Wine fault/capture and GT1/GT3/
SFIV visual evidence remain separate from this GT2 direction check.
