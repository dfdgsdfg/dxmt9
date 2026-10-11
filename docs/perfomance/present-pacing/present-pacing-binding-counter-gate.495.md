---
domain: present-pacing
workload: STALKER CoP H481 original Normal input, x86 release finite counter-gate comparison
title: "Binding counter gates consolidate; captured-body CPU saving remains below noise"
type: evidence
status: implemented-redundant-work-removal-no-qualified-time-saving
updated: 2026-10-11
source: traces/stkcop-binding-counter-gate-h495/{contract.md,candidate.patch,candidate-test.patch,build_runner.py,candidate-build.json,bind-instructions.txt,run.py,matrix-images.json,rows.jsonl,summary.json,tests.log,tests-final.log}; traces/stkcop-encode-cpu-stack-h494/{bind-counter-instructions.txt,build-binding-candidate.log,build-binding-candidate-r2.log}; traces/stkcop-binding-counter-gate-h495/{image-A,image-B}.stderr
related: present-pacing-encode-cpu-stack.494.md; present-pacing-v4-pending-lookup.493.md; present-pacing-validation-reuse.492.md; present-pacing-root-game.477.md
---

# Remove redundant diagnostics without claiming a measured tail saving

H494 identified real default-path work inside disabled binding counters.
`countBaseStateBind`, `countBaseStateBindSkip` and
`countBaseStateBindSkipExtended` now make one immutable enable decision per
call instead of 10, 2 and 7 decisions. Enabled calls use the existing
`addEnabledNonZero` helper, preserving nonzero relaxed increments without
zero-valued read-modify-write operations. No rendering, queue, resource
lifetime, ABI or policy knob changes.

Release instructions confirm the main OFF path changes from ten calls to
`perf::enabled()` to one. That is verified removed work, not proof of a useful
whole-source CPU saving. The original-input paired median is only **0.272 ms**,
below the **1.483 ms** maximum A/A difference, with six of eight pairs positive.
The predeclared time-saving gate fails. Do not report 0.272 ms as an established
removal budget or infer a game FPS gain.

## Semantics and verification

`enabled()` reads a process-cached immutable setting; one decision is equivalent
to repeated decisions within the batch. These counters are diagnostic relaxed
accumulators, not synchronization or event-publication tokens. Removing a zero
increment preserves their values and supplies no new ordering guarantee.
No queue/lifetime/interleaving protocol changes, so no separate TLA model is
claimed for this pure counter transformation.

`perf_binding_counters_spec.cpp` exercises all 1,156 zero/nonzero masks across
the 10-, 2- and 7-field families, with distinct near-UINT32_MAX values to expose
field permutations and 64-bit accumulation. Each family also receives 2,000
calls from two concurrent writers. Separate processes test ON exact sums and
OFF unchanged values. The cold `perf::test::snapshotBindingCounters` seam
reads raw values; it is not called by the production hot path and adds no
argument or return payload to its interfaces.

The first test attempted direct access to the hidden counter store and failed
to link. Its build log is retained. The repaired test follows the existing
cold snapshot-test seam instead of exposing the hidden store. The full x86
build passes. Both new tests, allocation-counter compatibility, counter table
and callsite audits, and the install-name audit pass (six tests). The ARM build
is not refreshed by this experiment.

## Frozen original-input comparison

Baseline is H493's archived candidate, so both arms already contain its
capture lookup optimization. The new candidate uses the same H486 runner
source/compiler command and release configuration, with the scoped binding
counter patch and cold tests. Both OFF runs prove their actual loaded image
paths. Each execution verifies hashes, and no build, Wine workload or Time
Profiler runs concurrently with the matrix. H494's active signposts are not
treated as an observer tax to subtract.

Two OFF image/oracle proofs, two diagnostic warmups per arm, four A/A pairs
and eight alternating AB/BA pairs all complete: 30 executions total. Each uses
a fresh device; cold PSO prefetch is not the timing target. The primary scope
is command-body thread CPU for the unique original source at captured index 1
with 556 commands. The unchanged-work control is planned-replay CPU on a
different thread; the two scopes are never summed.

| Entire captured scope | Baseline median | Candidate median | Paired median A−B | Max A/A difference | Positive pairs | Saving gate |
|---|---:|---:|---:|---:|---:|---|
| Large source command body | 18.859 ms | 18.521 ms | 0.272 ms | 1.483 ms | 6 / 8 | Fail |
| Planned replay control | 42.894 ms | 43.329 ms | −0.514 ms | 4.867 ms | 2 / 8 | Fail |

The body paired-median candidate/baseline ratio is 0.9854. No outlier is removed
and no additional favorable run is added. These empirical gates are not
confidence intervals or proof that a smaller benefit is zero.

All 30 runs preserve Normal policy, the three source cuts, 16 restored surfaces,
12 aliases, 779 created/released wrappers and exact source/output SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
All 28 coarse traces retain 92 raws, 208 accepted Slot checks and 84,587 command
visits. The Metal skill's original readback workflow is used; no new capture or
live-game staging is needed.

## Decision and objective boundary

The predeclared contract separates code retention from a time-saving claim.
Retain this narrow diagnostic simplification: counter semantics and original
GPU oracles pass, release code proves redundant work removal, and there is no
observed greater-than-5% paired-median body regression. Unlike an additional
cache/policy lane, it introduces no alternate rendering path or switch to keep.
This is not statistical proof of no regression outside the tested input.

Close this specific counter family as a large-bottleneck explanation: it does
not establish a material saving in the current finite experiment. Do not turn
the retained cleanup into a default performance promotion claim. Read-first
content authority, whole-tail representativeness and mixed Primary attribution
remain open; the largest default dxmt9 CPU owner and qualified removable amount
are still unresolved. Further work should use H494's non-diagnostic call sites
and source semantics, not repeat this small-noise comparison or expand it into
an unmeasured blanket counter rewrite.

The next code audit must not assume every sampled sampler lookup is redundant:
the direct fragment path in `encodeDraw` already checks
`samplerBindShadowMatches` before calling `resolveFragmentSamplerState`.
Surface extent lookup before binding-plan preparation and transient upload
remain separate concrete call sites; source-qualified multiplicity and existing
identity/metadata reuse must be checked before proposing another cache.
