---
domain: present-pacing
workload: PE32 and PE64 recorder fault conformance
title: "Preserve the unique-retain fault result through semantic-owner rollback"
type: evidence
status: correctness-prerequisite
updated: 2026-09-08
source: experiments/output/pe-inline-constant-refinement-20260908/analysis
related: present-pacing-inline-constant-current.258.md; present-pacing-pe-compact-binding-validation.262.md; ../../../specs/d3d9/recorder/requirements.md
---

# Preserve the unique-retain fault result

This is a correctness prerequisite for revisiting inline constant batching,
not a new FPS optimization or a default change. R-CORE-REC-7.5 owns injected
failure propagation; R-BACK-2.52 still requires independent final replay-state,
pixel and repeatable performance evidence before inline-mode promotion.

## Reproduced failure and narrow fix

The public PE `d3d9_recorder_fault` fixture was extended to set and read back
all six VS/PS float/int/bool constant families around failed and retry draws.
With the H262 PE32 image, `retain_acquire=0` was reached and consumed but the
first Draw returned `0x8876086c` (`INVALIDCALL`) instead of `0x8007000e`
(`E_OUTOFMEMORY`). The unmodified, no-constant fixture was separately rebuilt
and reproduced the same failure. Both failed result/log/staged-image groups
remain in the analysis root; this is not evidence of an inline-only regression.

The typed retainer returned false after recording the injected HRESULT, but
the owner append adapter no longer consumed its one-shot result. The adapter
collapsed rejected admission to `INVALIDCALL`. It now retrieves the receipt
only on failed append exits, after rollback. A failed rollback or successful
admission with an invalid projection still poisons/fails stop; the diagnostic
HRESULT cannot mask either invariant failure. Empty rolled-back owners have
no emission frontier and return the injected rejection, while invalid
nonempty frontiers retain `INVALIDCALL`. Invariant exits consume/discard the
receipt rather than leaking it to another attempt.

No hot-path signature, persistent state field, callback, ABI, queue order,
constant representation or default is added. This changes observable injected
failure handling; it is not labelled a behavior-preserving refactor. Existing
recorder transaction models own ordering and rollback; they do not prove the
configured HRESULT or private PE adapter. Native one-shot tests and actual PE
failure/retry runs provide that narrower binding.

## Actual PE evidence

`run_pe_faults.py` runs fresh processes through the canonical conformance
runner with Sikarugir cx-24.0.7, replay offload ON, and both inline values.
All **12** cases pass: PE32/PE64 × inline OFF/ON × failure before the first
unique retain, failure after one unique retain, and custom `0x80004003`.
Every case has explicit reached/consumed fault receipts and a passing retry;
`pe-fixed-r1/pe-fault-qualified.json` verifies verdicts and all three staged
images for that architecture against frozen full hashes. The native fault
matrix independently tests one-shot retrieval after typed-pin rollback,
including the custom HRESULT.
Main's focused native 5/5 passes (`native-fault-final-5.log`); the existing
full `dxmt9-verify-tla` suite also passes (`unchanged-tla.log`). No model or
model-code transition was changed, so TLC remains protocol evidence rather
than proof of the private adapter's HRESULT branch.

These Wine checks prove the public HRESULT and six Live getter values around
failure/retry, **not** the final draw's constant bytes or pixels. They do not
complete the all-family Wine fault matrix, capture/bridge ambiguity coverage,
or arbitrary corrupt-frontier/rollback-failure injection into the private PE
adapter. Those scopes remain open.

Both canonical PE builds pass. `fault-main-hot-metrics.json` compares the
linked images against H262: all twelve representative public-body metrics
per width have zero byte/instruction/direct-call delta. The outlined failed
append branch changes; unchanged representative callers do not prove that
all linked hot blocks are identical or that CPU/FPS is neutral. No FPS gain
or CPU-time saving is claimed for this cold error-propagation fix.
