---
domain: present-pacing
workload: STALKER CoP H481 original Normal input, command-local target lookup A/A and A/B
title: "Command-local raster-target reuse passes original oracles but fails CPU saving gates"
type: evidence
status: rejected-local-cpu-gate-runtime-restored
updated: 2026-10-11
source: traces/stkcop-command-raster-h497/{contract.md,candidate.patch,build.log,build-fixed.log,build-final.log,tests.log,tla.log,build_runner.py,candidate-build.json,run.py,matrix-images.json,rows.jsonl,summary.json,matrix.log,build-restored.log,tests-restored.log,verify_restored.py,restoration.json}; traces/stkcop-command-raster-h497/{image-A,image-B}.stderr
related: present-pacing-encode-callsite.496.md; present-pacing-binding-counter-gate.495.md; present-pacing-root-game.477.md
---

# A tested lookup-removal candidate, not a tail-gap explanation

The H496 call-site audit found a real resource lookup/shared-lock acquisition
in draw encoding. H497 implements and tests a value-only command-local raster
extent cache. All 30 original-input GPU/source oracles pass, but the frozen
CPU saving gate fails. The candidate and its candidate-specific tests are
archived in `candidate.patch`; runtime source and the binding-plan spec are
restored to HEAD `221d6f60`. No new knob, default policy or global cache remains.
The frozen candidate images and every result, including slow pairs, are kept.

## Semantic and bounded proof

Reference `encodeDraw` resolves attachment zero for each binding packet. A
live textured surface supplies width/height; missing or textureless surfaces
supply 1×1 and suppress viewport/scissor emission at the later base-bind site.
The first build catches an omitted presence consumer; its log is retained.
The corrected candidate explicitly distinguishes a real 1×1 target from that
fallback, rather than treating dimensions as a presence test.

`CommandBindingPlan::rasterExtent` stores only width, height and presence after
successful lookup of the command's full attachment handle. Foreign generations
and slots resolve independently without replacing the owner value. Missing or
textureless lookups do not grant reuse. The command's retained surface descriptor
and backing are immutable in this scope; existing mark/destroy operations only
update sequence/destruction metadata. No resource pointer, lease or ownership
transition is added. New commands have new owners, and standalone/parallel paths
without that owner keep the reference lookup. Dynamic stream/raster preparation
and actual Metal emission remain unchanged.

The production-helper differential test exhausts 46,080 reads over five-read
histories and fresh A/B/A owners: owner, foreign generation, foreign slot, null,
missing-then-success, permanently missing and real 1×1 targets. It checks both
reference values and resolver call counts. Existing production stream-override
and per-draw raster reducer tests remain. Focused x86 tests pass 5/5; the full
x86 build passes and existing TLA regression passes in 207.46 seconds. The TLA
suite is regression evidence, not a model of this new cache; bounded native
refinement is its explicit sequential model/code layer.

## Frozen finite experiment

Baseline is the authenticated H495 candidate, corresponding to current runtime
source before H497. Candidate uses the same H486 runner source and compiler/link
recipe; the runner binary itself has the same SHA-256 in both arms. Runtime
images are frozen and rehashed per run. Observer-OFF dyld logs verify both native
and unix-provider image paths. No compilation or TLA run overlaps measurement.
No Wine staging, UI operation, Time Profiler or new capture occurs.

The predeclared matrix has two OFF image/oracle proofs, two coarse-trace warmups
per arm, four A/A pairs, and eight alternating AB/BA pairs. Every execution uses
a fresh device/cold per-device PSO cache. The primary metric is command-body
thread CPU of original source index 1 (556 commands), not cold prefetch.
Success requires median paired A−B greater than the maximum absolute A/A delta
and at least seven positive pairs. No exclusions or favorable extra runs follow.

| CPU scope | Baseline median | Candidate median | Paired A−B median | Max A/A delta | Positive pairs |
|---|---:|---:|---:|---:|---:|
| Large-source command body | 18.427 ms | 18.5365 ms | −0.177 ms | 0.787 ms | 3/8 |
| Complete planned replay, unchanged-work control | 43.053 ms | 43.727 ms | −0.221 ms | 1.601 ms | 4/8 |

The removal does not demonstrate a CPU saving. Its central slowdown is inside
the observed A/A envelope, so this is not a proven general regression either.
Two candidate pairs are substantially slower and remain in the evidence.
Independent arm medians differ from the paired statistic. Concurrent CPU scopes
are not added, and no number is transferred to default game frame time or FPS.

## Original oracle and decision

All 30 executions preserve original source/output SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`,
unchanged Normal policy, 16 surfaces, 12 aliases, 779 balanced wrappers and
three original source boundaries. All 28 trace executions retain 92 raws,
208 accepted validators and 84,587 command-header visits. Xcode 27, Metal
compiler and Apple M1 / Metal 4 doctor checks passed before the original
readback workflow. Original all-resource content authority remains unknown
and `timing_qualified=0`; GPU equality does not remove those capture limits.

The explicit retention rule rejects a new cache when the time-saving gate
fails, unlike H495's stateless diagnostic simplification. Runtime and test
source are restored, not hidden behind an OFF branch. There is no supervised
wild promotion because the local benefit gate already failed. This closes
this particular cache experiment, not every resource-lock optimization and
not the possibility of contention in the live game.

Restoration is independently verified: the touched runtime/test/spec paths
have no HEAD diff; both rebuilt native and unix-provider image SHA-256 values
are byte-identical to the frozen H495 baseline. The restored focused tests
pass 5/5. The ARM build and Wine installation are untouched.

The next investigation should change scale: price or remove a complete
representation step, rather than another isolated lookup. Current code expands
compact stage constants into zero-filled full arrays, copies the fixed payload,
then builds upload prefixes and copies into transient storage. Existing command
scratch/parameter reuse must be accounted for first. A borrowed compact-stage
consumer must preserve implicit zeros, boolean canonicalization, upload layout,
hash/identity and scratch lifetime; skipping arbitrary payload initialization
is not an equivalent candidate. H496 also leaves driver work under vertex-byte
binding and pass creation unpriced. Neither branch is yet the largest default
CPU owner or a qualified removable budget; mixed Primary remains unresolved.
