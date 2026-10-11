---
domain: present-pacing
workload: STALKER CoP H481 original Normal input, x86 release finite A/A and A/B
title: "Invocation-local uniform validation reuse does not clear the CPU noise gate"
type: evidence
status: rejected-noise-gate-runtime-restored
updated: 2026-10-11
source: traces/stkcop-validation-reuse-h492/{contract.md,run.py,build_runner.py,candidate-full.patch,candidate-build.json,matrix-images.json,rows.jsonl,summary.json,tests.log,build-candidate.log,build-restored.log,restored.json,tests-restored.log}; traces/stkcop-validation-reuse-h492/{image-A,image-B}.stderr
related: present-pacing-slot-validation.491.md; present-pacing-provider-stack.490.md; present-pacing-root-game.477.md; present-pacing-default-unwind-feasibility.467.md
---

# A concrete removal candidate fails the frozen noise gates

H491's repeated full-Slot scan is real, but a cheaper uniform-lookup loop is
not yet a qualified saving. The candidate reuses the last positive handle
lookup within one const DrawRun traversal. It changes no command/payload range
check and retains no cache across calls. After 30 same-input executions, both
predeclared CPU gates fail. The runtime change is archived and reverted;
the exhaustive predicate tests remain useful for future validation changes.

## Why this experiment, and what it does not reopen

The original objective is the largest default-tail dxmt9 single-thread owner
and a correctness-qualified removable amount. H477's all-call clock observer
failed workload transfer; H467's external CPU profiler failed Primary ancestry
recovery under Rosetta. Neither closed experiment was rerun here. H491 alone
does not repair either gap. A broad generation-qualified prefix cache would
introduce additional invalidation/rollback obligations for a measured envelope
of only a few milliseconds in this input. This experiment first tests a smaller
pure work-removal mechanism, without claiming that it solves global ownership.

## Predicate refinement and binary qualification

`src/dxmt9/dxmt9_backend_types.hpp`, `ChunkSlot::commandPayloadsInRange`,
already validates the DrawRun's uniform handle. The candidate remembers it in
a local 16-byte value, skips an equal valid parameter handle lookup, and updates
the local value after a different valid handle succeeds. Invalid handles retain
the original semantics. Equality includes index, generation and hash. Nothing
survives the const invocation, and all parameter payload ranges remain checked.

This is a pure predicate transformation on an unchanged input, not mutable
state caching or an ownership/interleaving change; a TLA lifecycle model is
not applicable. The native test compares against every-handle lookup for all
16,807 combinations of seven handle classes across one run and four parameters.
Classes cover null, two valid identities, stale generation, stale hash,
out-of-range index, and generation-zero tokens with nonzero other fields.
It additionally corrupts a validated handle between calls and exercises every
payload range in every parameter. Existing production routing, source-payload,
provider V4 and install-name tests also pass (six focused Meson tests total).

Both arms use the same x86 release configuration and H486 runner source and
compiler/link command. Separate runners are necessary because provider static
objects compile the inline validator. The baseline runner and two images match
H491's hashes. Each arm's OFF run confirms its actual archived image paths;
every execution rechecks image hashes. Candidate images and the cumulative
header/test patch are preserved in the artifact tree. No Wine image is staged
and no game configuration or default is changed.

## Frozen finite result

The matrix contains two OFF image/oracle runs, two diagnostic warmups per arm,
four A/A pairs and eight A/B pairs with alternating AB/BA order. All 30 runs
use a fresh device; the warmups do not establish warm per-device PSO caches.
All 30 runs
pass the original source/output pixel oracle, unchanged Normal policy,
16-surface/12-alias restoration, balanced 779 wrappers and three source cuts.
Every diagnostic has 92 raw calls, 208 accepted validator calls and 84,587
successful command visits. No counterexample or outlier is discarded.

The acceptance rule, written before candidate execution, requires the paired
median saving to exceed the maximum absolute A/A difference, with at least
seven of eight pairs positive. These are empirical experiment gates, not
confidence intervals or claims that effects below the threshold are zero.

| CPU scope, entire original interval | Paired median A−B | Maximum A/A difference | Positive pairs | Gate |
|---|---:|---:|---:|---|
| Full-Slot validator | 0.312 ms | 0.737 ms | 8 / 8 | Fail |
| Planned replay, including V4 | 0.397 ms | 1.463 ms | 5 / 8 | Fail |

One baseline A/B member has 5.451 ms validator CPU and 59.085 ms planned CPU.
It remains in the report; there is no evidence assigning its increase to user
activity, scheduling, or a particular background process. No synthetic observer
tax is subtracted. No favorable extra runs or adjusted threshold are added.
The Metal skill's doctor/readback workflow is used (Xcode 27, Apple M1 Metal 4),
without gputrace or concurrent builds during measurement.

## Result and remaining objective

The original validator is restored and rebuilt; `restored.json` records the
post-restore image comparison. The runtime has no new cache or policy switch.
This closes invocation-local last-handle reuse as an accepted optimization in
this experiment. It does not disprove a tiny benefit or establish that every
possible prefix-validation improvement is useless.

The measured cost envelope must not be promoted to a 3 ms removal budget:
the actual tested removal did not clear either noise gate. No live-tail FPS,
global owner ordering or qualified removable amount follows. All-resource
content authority remains unknown and timing qualification false in the
original replay oracle. The objective remains incomplete.

Before another small replay optimization, separate the substantial V4
qualification work identified in H490 from ordinary replay work. In particular,
the repeated record scans in identity binding and physical-append verification
are observer costs, not product savings. Improving that measurement path must
preserve rollback and exact identity evidence. Independently, the mixed live
Primary thread still needs default-transfer-qualified attribution; a faster
capture harness alone cannot close that ownership requirement.
