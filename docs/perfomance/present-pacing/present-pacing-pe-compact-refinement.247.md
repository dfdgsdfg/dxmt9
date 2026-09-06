---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE compact transaction and live borrow refinement"
type: evidence
status: partial
updated: 2026-09-06
source: src/d3d9/d3d9_pe_semantic_owner.hpp; experiments/output/pe-compact-refinement-20260906/analysis; experiments/output/app-d3d9-3dmark05-gt2-perf-compact-refinement-base-a1-staged-20260906; experiments/output/app-d3d9-3dmark05-gt2-perf-compact-refinement-base-a2-20260906; experiments/output/app-d3d9-3dmark05-gt2-perf-compact-refinement-candidate-b1-20260906; experiments/output/app-d3d9-3dmark05-gt2-perf-compact-refinement-candidate-b2-20260906
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-plan-authority.246.md
---

# PE compact transaction and live borrow refinement

This implements R-CORE-REC-7.22–7.25 over the existing mandatory semantic owner.
Transport, logical quotas, chunk cadence, thread policy, and rendering policy
are unchanged. The scope is PE representation, boundary work, and synchronous
source authority, not a new concurrent protocol or FPS promotion.

## Mechanism

- Twelve typed identity/object indexes use a generation-qualified active
  frontier. Ordinary reset invalidates them without clearing 295,200 bytes;
  uint32 generation wrap performs the physical clear. Bucket size remains
  24 bytes. Rollback rebuilds only the restored active pins under a new
  generation; identity/generation/object collision rejection is unchanged.
- Warm-retainer settlement skips rebuild only for an unchanged clean index.
  Expiry/compaction or rollback tombstones still require rebuilding. Unsigned
  epoch advancement and release multiplicity are unchanged.
- Compact preparation uses 45 identity entries and 45 wire ordinals, bounded
  by seven binding families. Generic retained-only inputs keep 80/64 limits.
  Committed records carry 32-byte receipts instead of the 640-byte all-family
  product; canonical roles own the values. Compact and constant call-local
  staging are only 56/8 bytes. Logical quotas and cold visitors are preserved.
- After pure preparation, compact typed pin acquisition and canonical row
  emission share a traversal inside the existing append checkpoint. Failed
  acquisition or settlement restores the prior committed prefix and ownership.
- A guard issues access only inside an exact-bool, nothrow callback. Borrow
  visitors, source-facts access, destination finalization, and compact
  settlement require matching owner/epoch/lane authority. Source access fails
  closed; no-cap metadata getters return copied values, not borrowed spans.
  Revoke-only invalidation remains valid for poison cleanup. This is ordinary
  C++ synchronous API discipline, not protection against intentionally escaped
  raw references.

The existing sampled phase observer gains `settleRetainerEpoch` and
`settleOwnerReset` children under `settleClear`. They are inclusive attribution,
not additive product cost; the disabled path adds no clock reads.

## Release representation evidence

The baseline source is `724690acd64625da540b0475f3ae6b54bab86e92`. Its preserved
binary/object hashes match H246's final checkpoint. The simultaneous dirty
worktree patch is explicitly **not** the source of those baseline binaries.
Frozen objects, disassembly, manifests, build options, and the native layout
probe are in `experiments/output/pe-compact-refinement-20260906/analysis/`.

| Ordinary compact append | Baseline | Candidate |
|---|---:|---:|
| PE32 stack reservation | 4,336 B; `__alloca` | 3,144 B; no probe |
| PE64 stack reservation | 4,432 B; `___chkstk_ms` | 3,232 B; no probe |

The narrow constant append remains 100/192 bytes on PE32/PE64, without a probe.
The native prepared-type sizes are 4,328 bytes generic, 3,136 compact, and 96
constant; type sizes are not substituted for compiled stack frames. Generic
UP/control staging is still broad and is not claimed to have a smaller frame.

## Runtime protocol and current evidence

The four throughput runs use GT2, 1024x768, Sikarugir `sikarugir-cx-24.0.7`, `perf`,
Info logging, segmented transport 0, PE stats decimation 0, semantic-owner
phase split 0, recorder stats 0, and thread sampler 0. Existing Unix counters
remain enabled identically. FPS comes from benchmark-owned `.3dr`, not the
runner's process-elapsed estimate. Builds/tests/other workers are stopped
during accepted runtime cells. The 35-second screenshot is a scene anchor,
not an exact-frame or whole-scene pixel oracle.

| Run suffix after `gt2-perf-compact-refinement-` | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `base-a1-staged-20260906` | 1,758 | 28.107 | 3.9994 | 15.7679 |
| `base-a2-20260906` | 1,730 | 27.664 | 3.9994 | 15.7723 |
| `candidate-b1-20260906` | 1,757 | 28.096 | 3.9994 | 15.7718 |
| `candidate-b2-20260906` | 1,676 | 26.788 | 3.9994 | 15.7774 |

All four cells have zero rejects, GPU errors, pipeline-library failures,
and no-pipeline skips. Their five staged artifact hashes match their frozen
checkpoint; the Unix provider and both bridge DLLs are identical across A/B.
Only the two PE `d3d9.dll` artifacts change. Both candidate screenshots show
coherent forest/firefly scenes, not a whole-scene pixel equivalence proof.
Two earlier launch attempts (`base-a1` and `base-a1-retry`)
stopped in automatic staging before Wine execution because the cross compiler
was absent from that shell's PATH. They are not runtime samples. The accepted
baseline cells explicitly reused the already-staged, hash-qualified baseline.
Candidate B1 staging emitted missing-`rg` warnings and skipped redundant Meson
freshness checks; all four builds and the Unix fixup audit had already passed,
and all five staged hashes match `candidate-final-binaries.json`. B2 reuses
that same qualified staging. This warning is not hidden as a fresh build check.

The pair harmonic means are **27.884 baseline / 27.427 candidate (-1.64%)**.
Candidate same-build spread is 4.88%, versus 1.60% for baseline. B2 is slower
and remains included; no unsupported contamination exclusion is applied.
Ordered A/A then B/B runs do not remove thermal or run-order bias. The result
does not establish a gain, noise-qualified neutrality, or a causal regression.
The supported outcome is representation/API refinement with bounded native
correctness and healthy GT2 execution, **not FPS promotion**.

## Integrated verification

All four canonical builds pass. Focused native specs pass 21/21; x86_64 specs
pass 8/8, including the Unix install-name audit. The unchanged TLA suite passes
(149.24 seconds); it covers the existing protocol, not concrete index bytes or
Wine/driver behavior. Added native checks cover generation wrap, rollback and
stale membership, reference retainer traces, and wrong-owner/epoch/ticket
rejection. A real indexed producer supplies all seven binding families and 45
occurrences, both all-distinct and null/duplicate variants. A fresh-process
unique-retain fault reaches the final depth-stencil binding before settlement,
preserves the committed prefix/refcounts, and permits a clean retry equal to
the independent generic producer's final wire.

The main integration pass restored a stale-epoch fixture transition lost during
callback migration and a shared test ownership domain (template-local statics
had accidentally made every lambda a different owner). The stale-range test
now requires zero ordinary clears while retaining its independent fresh-owner
byte comparison. These are test fixes, not relaxations of production rejection.

Final source/object/build receipts use the `candidate-final` prefix. Earlier
`candidate` artifacts predate the final test fixes and are codegen checkpoints
only. The final patch includes the shared capability header as well as sources
and tests. Broader Wine fault/capture and whole-scene pixel evidence remain open.

## Sampled boundary attribution (not throughput)

The separate
[`candidate-observed-n127` run](../../../experiments/output/app-d3d9-3dmark05-gt2-perf-compact-refinement-candidate-observed-n127-20260906/result.json)
uses the same candidate hashes with stats decimation 127 and phase split 1.
It passes 1,630 Presents, zero rejects/errors, 3.9994 CB/P and 15.7712 passes/P.
Its 26.048 benchmark FPS is diagnostic-only and excluded from the OFF pair.
The final cumulative PE log records 25,792 boundary events / 203 samples and
34,899 null-pair calibration samples totaling 8.182 ms (234.45 ns/pair).

| Boundary scope | Raw sampled ms | Null-pair-corrected estimate, ms/P |
|---|---:|---:|
| `settleClear` (inclusive parent) | 4.695 | 0.362 |
| `settleRetainerEpoch` | 4.463 | 0.344 |
| `settleOwnerReset` | 0.077 | 0.0023 |

Estimate: `max(0, sampled_ms - sampled * null_pair_ms) * events / sampled /
1630`. Rounded log values and deterministic sampling limit precision. Children
are inside the parent, not additive with it; nesting overhead remains even
after null-pair correction. This confirms that the new children are live and
that residual boundary attribution is concentrated in warm retention rather
than owner reset. There is no matched pre-change boundary-child sample, so it
does not quantify saved product CPU time. Fused pin acquisition is now charged
to canonical materialization (about 1.614 corrected ms/P), not to the earlier
sparse-copy phase; those historical phase names cannot be compared as if their
work ownership were unchanged. Prepare and PendingDelta settlement remain
about 1.027 and 0.576 corrected ms/P, respectively. These are investigation
signals, not an additive FPS budget.
