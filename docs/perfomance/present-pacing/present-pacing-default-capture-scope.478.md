---
domain: present-pacing
workload: Current STALKER default-tail capture and replay capability audit
title: "Existing Render Tape qualification is not a default-action workload snapshot"
type: evidence
status: default-capture-authority-missing-no-new-workload-run
updated: 2026-10-08
source: traces/stkcop-default-capture-scope-h478/{audit.py,receipt.json}; src/dxmt9/dxmt9_command_queue.cpp; src/dxmt9/dxmt9_render_tape_action_profile.hpp; src/d3d9/d3d9_pe_device_tape.cpp; scripts/check/run_render_tape_cpu_v4_semantics.py; scripts/check/run_render_tape_v4_capture_diagnostic.py; specs/experiments/harness/replay/requirements.md; specs/experiments/harness/replay/spec.md
related: present-pacing-root-game.477.md; present-pacing-v4-final-grouping.443.md; present-pacing-run-construction-removal.451.md; ../../../specs/experiments/gap.md
---

# Do not recapture a different policy and call it default evidence

After H477 rejects live all-call-clock transfer, an identity-matched captured
tail is the next proposed way to measure actual work removal. The current
capture/replay contract does **not** supply an untouched default-action input.
This is a documented capability boundary, not a newly found renderer bug.
No additional STALKER run, capture, renderer build or policy matrix is launched
for this audit.

## Verified contract boundary

| Surface | Current behavior | Consequence for the objective |
|---|---|---|
| `CommandQueue::CommandQueue` / `initializeRenderTapeActionProfile` | `DXMT9_RENDER_TAPE_CAPTURE` selects exact attachment preservation at queue construction | Even a late arm has capture-policy prehistory; capture is not a passive default-action observation |
| `RenderTapeActionProfileHistory` / `renderTapeActionProfileStableExact` | Distinguishes normal/exact construction and current policy; stability requires exact construction and unchanged revision zero | Disabling preservation after construction or relabeling endpoints cannot manufacture accepted default input |
| `armRenderTapeCaptureAtPresentBoundaryInterval` | Selects by successful idle Present skip count | It does not identify an engine completed frame, camera pose or a whole eight-second tail window |
| Controlled CPU semantic admission | Requires authenticated `action_profile` with `stable_exact=true` | A normal-profile bundle is outside the qualified CPU comparison domain |
| GPU semantic-mapping admission | Requires the same stable-exact witness before launching replay | Relaxing only the Python check would discard an authority requirement, not implement normal-policy replay |

R-HARN-REPLAY-7.18 explicitly requires the same tape-exact attachment policy
from capture queue construction through the interval and during replay.
R-HARN-REPLAY-9.14 requires an observed authenticated action-history witness;
environment strings, missing evidence and matching endpoint booleans are not
substitutes. This intentional contract permits deterministic comparison in
the declared capture domain, not automatic transfer to normal policy.

Two existing focused tests pass on the current tree, with commands, return
codes, output and source hashes retained in the audit receipt:

- `test_input_authentication_and_action_witness_reject_before_child` checks
  digest tampering and missing action evidence in CPU semantics admission.
- `test_semantic_legacy_unknown_or_changed_action_profile_never_spawns` checks
  absent, normal, ABA and manifest-only GPU semantic-mapping inputs. All remain
  rejected before the mocked child launch.

These are launcher rejection tests, not GPU execution or a new physical
attachment-preservation proof. They confirm that the current rejection is
deliberate and enforced; no gate is waived.

## More than one boolean separates capture from the default path

The documented PE capture path also disables the texture-level surface-handle
cache to preserve capture object/alias identity. Present mirror reservation
joins replay and flushes the preceding ordinary writer under the explicit
`pe-present-reservation-v1` producer contract. Consequently, merely splitting
the capture flag from attachment preservation would not establish unchanged
default PE cost, publication topology or resource history.

Nor does a canonical Render Tape reproduce every original public PE call:
its input is the recorded semantic stream. Even a new default-action capture
authority would not by itself resolve H477's Primary game/Wine/PE split or
prove the globally largest owner. Both clauses of the original objective must
remain separate from any successful local Replay/Encode experiment.

## Required capability before the proposed default-tail removal experiment

The missing capability is a **default-policy capture and replay contract**, not
another choice of Present skip count. Its bounded design would have to provide:

1. An authenticated join from the selected game completed frame/camera envelope
   to capture token, epoch, original event/source identities and actual loaded
   images. Retain selected source cuts; do not substitute capture29 or infer
   scene identity from wall time or file creation time.
2. Truthful normal-policy provenance, with a defined treatment of capture-only
   drains, object-cache behavior, lookahead and attachment first-use/live-out
   contents. Missing or undefined bytes must fail closed, not be initialized
   to fabricated reference values.
3. A normal-policy reference execution and candidate admission contract, followed
   by model/native binding and meaningful original GPU/query readback checks.
   Keep existing stable-exact qualification intact as a separate domain.
4. Only then, identical-input measurements of a concrete work removal. A local
   body delta does not establish whole-tail prevalence, default Primary
   attribution or a largest-owner claim without those additional joins.

This is a harness/verification implementation extension beyond rerunning the
existing measurement commands. No requirements are weakened and no production
semantics, public execution grammar or runtime default is changed by this audit.
The present tools can still perform explicitly capture-profile conditional
experiments, but those must not be substituted for the active default-tail
objective.

The smallest feasibility stage need not inherently change the public V4 binary
ABI: the existing action-profile value already represents Normal as zero and
TapeExact as one. An explicitly separate diagnostic could publish a truthful
Normal witness with `stable_exact=false`. That would not authorize current
semantic/performance qualification, nor make snapshots and capture drains
passive. Normal replay and its initial-history sufficiency remain a separate
implementation/verification decision. Obtain scope confirmation for that
extension rather than silently rewriting the existing TapeExact contract.
