---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "CPU attachment authority: production-equivalent Clear work and qualified A/A"
type: experiment
status: qualified-harness-correction
updated: 2026-09-16
source: experiments/output/gt2-attachment-authority-20260916-r621.noindex/campaign.json; experiments/output/gt2-attachment-authority-20260916-r621.noindex/audit.json; experiments/output/gt2-attachment-authority-20260916-r621.noindex/native-testlog.txt
related: docs/perfomance/mini-replay-bisection/cpu-shadow-slowmode.07.md; docs/perfomance/mini-replay-bisection/gt2-policy-matrix.06.md; specs/experiments/harness/replay/fixed-input-experiment.md; specs/verification/spec.md
---

# CPU attachment authority

## Verdict

The CPU-only attachment-Clear timing mismatch is corrected without claiming
GPU readback or removing Clear commands. Three fresh same-policy A/A pairs
now differ by only 0.27–0.63%, and their whole-iteration CPU medians are
70.7–73.4ms. All CPU semantics, source settlements and original GPU pixels
are independently reauthenticated. This is a **harness-fidelity correction**,
not an application FPS improvement or a renderer policy promotion.

The previous [226–319ms measurement brackets](cpu-shadow-slowmode.07.md) retain
their original meaning: they include software Clear work absent from the
normal GPU-backed provider. The new receipts require a different attachment
ownership contract and cannot be silently mixed with those older timings.

## Ownership and execution contract

`include/dxmt9/attachment_contents.hpp` defines a small value-only ownership
enum and shared pure shadow/read routing predicates. No per-draw allocation,
observer, bridge schema or policy knob is introduced.

- `DeviceImpl` declares backend-command attachment ownership and retains real
  GPU readback support. Successful production Clear behavior is unchanged.
- Existing recording fixtures retain CPU-shadow ownership. Only the admitted
  general CPU executor selects backend-command ownership before tape effects;
  its readback capability stays false.
- Core Clear and StretchRect use ownership, not readback availability, to
  select shadow work. Clear's command, snapshot, order and lifetime remain.
- Complete immutable seed writes still execute. The CPU resource oracle uses
  qualified identities, seed digests and ordered commands, never seed storage
  as a claim about post-render pixels. CPU read/control consumers are rejected
  by the existing restricted grammar before resource effects.
- `getRenderTargetData` rejects an unavailable or failed backend-owned read
  instead of falling back to a stale seed. Low-level seed locks are not a
  general post-render readback implementation.
- Both CLI and comparison gates require
  `attachment_contents=backend-command-owned-v1` and `gpu_readback=false`;
  missing/old/false-capability receipts reject.

## Evidence order

The bounded native refinement enumerates **373,248 six-step traces** over two
owners, attachment/nonattachment, readback available/unavailable, and six
seed/Clear/read actions. Every successful read equals the serial latest value;
unavailable reads expose nothing; a stale-fallback mutant has counterexamples.
Concrete core calls additionally pin color surface, texture alias and depth
shadow decisions, unchanged queued Clear identities, normal completion, and
unavailable/failed readback rejection. This is not an unbounded or concurrent
proof and does not establish physical pixels.

The full x86_64 release build succeeds. Fifteen focused native/CLI tests plus
`dxmt9-verify-tla` pass; the latter checks all 196 existing configurations.
The new ownership proof is native exhaustive refinement, not a new TLA module.
Three supplementary tests (core device coverage, Render Tape provider and
unixlib install-name audit) pass after the campaign; their separate logs are
retained in r621. The same-image
[forward/reverse policy matrix](gt2-qualified-policy-matrix.09.md) follows.

## Same-input A/A: r621

The unchanged r405 GT2 original has source-plan SHA-256
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
All CPU/GPU receipts authenticate original/staged input, policies, provider,
validator and actual loaded runtime images. The x86_64 release/O3 controller
runs through Rosetta. No concurrent build, test, sampler or GPU trace runs
during the measurement block. Host/process snapshots are retained; they are
not a guarantee of fixed core placement or frequency.

Each pair has two full CPU verification iterations and two cold-device GPU
exact iterations per side, then two warm-ups plus sixteen CPU measurements.
Total: **12 CPU verification, 12 warm-ups, 96 measured CPU and 12 GPU exact
iterations**. All 1,866 ordered draw observations and the final semantic digest
equal the pre-correction r620 verification. Every CPU iteration retains 185
submitted/completed/reclaimed sources and 656 source-summary commands.

| Same-policy pair | Reference CPU median (ms) | Candidate CPU median (ms) | Difference |
|---|---:|---:|---:|
| Compatibility exact | 72.799 | 72.595 | -0.28% |
| ProductionSlot exact | 70.870 | 70.678 | -0.27% |
| Compatibility exact repeat | 73.388 | 72.927 | -0.63% |

All original source/output RGBA hashes remain
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`, with zero
GPU command-buffer errors. Warm-ups and slow samples remain in the evidence.

## Remaining scope

Whole-iteration CPU time still includes fixture creation, admission, resource
setup/mutation, publication, controlled completion and teardown; it is not
isolated production replay service time. GPU encode, asynchronous supply,
real completion pacing and Wine arrival are excluded. The precise reason
the old software depth loop had process-dependent speeds remains unproven;
removing that unrepresentative work does not identify Rosetta/OS internals.
Policy ranking requires the separate forward/reverse matrix and must be
distinguished from capacity/locality evidence and any later wild FPS result.
