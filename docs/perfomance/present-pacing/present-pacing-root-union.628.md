---
domain: present-pacing
workload: Sikarugir PE32/PE64 diagnostic root-bound qualification
title: "H628 — union overlapping clock margins without reducing endpoint uncertainty"
type: experiment
status: qualified-preflight-live-owner-open
updated: 2026-10-16
source: traces/stkcop-root-union-h628/{contract.md,cover.hpp,root_wall.hpp,prepare.py,derivation.json,native-tests.json,build.json,bounds.py,fixture-run.json,verification.json}; traces/stkcop-root-union-h628/{i686,x86_64}-fixture/stdout.txt
related: present-pacing-override-projection.627.md; present-pacing-owner-composition.621.md; present-pacing-owner-domains.563.md; present-pacing-clock-bridge.562.md
---

# H628 — tighten a timing bound, not the workload's CPU

H627 identified about 3.1ms/frame of endpoint allowance in H621's Primary
renderer upper bound. H628 retains the original ±100ns per endpoint and
removes only overlap between adjacent expanded root intervals. Pure native
refinement and actual PE32/PE64 COM/activation fixtures pass. This does not
yet tighten the live STALKER bound or establish a largest default owner.

## Bound and checkpoint contract

For observed disjoint ordered roots `[start,end]`, the original upper sum is
`sum(end-start+200ns)`. Each true root is enclosed by
`[max(0,start-100ns),end+100ns]` under the existing clock assumption. The union
of these expanded intervals therefore encloses all renderer-root CPU without
counting shared margins twice. Waits and callbacks remain inside the bound;
this is not an off-CPU subtraction or a smaller assumed clock error.

A thread-confined `Cover` reducer maintains the previous expanded end and
cumulative union length. Checked arithmetic rejects reversed/overflowing
inputs. `RootWall` retains identity/epoch, activation, nesting, original root
count, elapsed sum and original upper sum, adding the union total. No extra
per-root clock read or native bridge call is introduced; extra integer work
still has an observation cost that has not been priced live.

At a checkpoint the cumulative union may include part of a cluster that
continues into later roots. A window subtraction is qualified only when the
first checkpoint's mapped `after_tick` upper is before the camera window and
the last checkpoint's root-end lower is after it. The first accumulated cover
then ends before the selected interval. Subtracting it cannot remove coverage
inside that interval. Reuse H563's enclosing checkpoint selection and explicit
continuous/absolute mapping; do not use arbitrary cumulative sample pairs.

The map gets distinct H628 magic. Previously reserved slot/row fields carry
union totals, while the original elapsed/sum-upper fields remain unchanged.
Old and new formats cannot be silently mixed. The parser requires
`elapsed <= union <= original_upper`, monotonic qualified deltas, existing
identity/clock/nesting/publication conservation and empty retired domains.

## Native and actual-PE evidence

ARM64 and x86_64 each pass:

- 20 original root-reducer controls, including disabled reads, nested roots,
  stale identity, activation, monotonicity, arithmetic and retained waits.
- 31 activation/epoch controls.
- 729 measured interval histories, independently reconstructed union lengths,
  1,681,362 admissible actual endpoint combinations and 2,187 checkpoint
  differences. Actual roots remain contained under the unchanged uncertainty.
- Three negative mutations: omitted end margin, incorrectly counted gaps and
  discarded overlap tails. All fail the checker. An overestimate mutant is
  rejected for wrong exact union arithmetic, not mislabeled unsafe containment.

Only private diagnostic PE entry objects, controller and fixtures are rebuilt.
The product PE baseline and H621 Unix provider remain authenticated. The real
PE32/PE64 fixtures pass external identity-qualified activation, wrong-birth
rejection, actual COM objects, all table/export coverage, nested callback,
reload/epoch behavior and 20,000 getter calls. The existing clock-domain
qualification is preserved. Xcode 27.0, Metal 32023.921 and Apple M1 are
rechecked before the runtime fixture.

The first launcher attempt rejected the new build receipt before any staging:
it expected an optional `generated` dictionary absent from the new receipt.
Using the recorded `inputs`/`images` and an empty optional dictionary fixes
that scaffold lookup. No runtime or timing population was replaced.

The verifier re-reads raw map tuples, checks original/union arithmetic and
matching parsed fields, source reversals, frozen inputs and runtime hashes.
Six checkpoint rows and 16 deliberately corrupted identity/schema/union cases
pass their acceptance/rejection gates. Five shared Wine images are restored
by hash, and three temporary clock-service files are recovered after fixture
process exit. No product source, default-policy, commit or push change occurs.

## Tight-loop result — not live STALKER performance

Milliseconds across the 20,000 getter roots in each actual fixture:

| PE architecture | Recorded elapsed sum | Original upper | Union upper | Overlapping margin removed |
|---|---:|---:|---:|---:|
| i686 | 4.257246 | 8.257246 | 5.254559 | 3.002687 |
| x86_64 | 1.167883 | 5.167883 | 1.607133 | 3.560750 |

These are bounds from the same measured calls, not before/after execution
times. Both original and union retain the same endpoint allowance. The
reduction is a proof-accounting improvement, not eliminated renderer work,
an FPS gain, or a precision guarantee beyond the original clock qualification.
Getter spacing differs from game call spacing, so do not scale these numbers
to H621's live call count.

## Next evidence toward the full objective

Run a prospectively fixed same-camera composition with H628 diagnostic PE and
the authenticated H621 Unix observer image. Record original and union bounds
in the same ON episodes, alongside Replay/Encode and auxiliary ownership;
retain OFF controls for observation burden. Require exact identity joins,
natural scene completion, image inspection and staging restoration.

Existing H621 data cannot be retroactively corrected: its aggregate receipt
does not retain every root endpoint needed to reconstruct overlap. Its prior
19.196/18.879ms/frame bounds remain authoritative for those episodes. Even if
the new live union separates Primary from Encode, default transfer, lifecycle
tails and a correctness-qualified removable CPU amount remain separate gates.
