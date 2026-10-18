---
domain: present-pacing
workload: STALKER CoP pending Clear fusion feasibility
title: "Shared Clear-fusion admission and bounded value refinement; runtime binding remains open"
type: evidence
status: candidate-predicate-model-passed-runtime-gpu-economy-open
updated: 2026-10-15
source: src/dxmt9/dxmt9_pending_clear_fusion.hpp; tests/native/backend/pending_clear_fusion_spec.cpp; traces/stkcop-clear-model-h547/verify.py; traces/stkcop-clear-model-h547/qualified/{verification.json,dxmt9_pending_clear_fusion.hpp,pending_clear_fusion_spec.cpp,arm64-build.log,arm64-test.log,x86_64-build.log,x86_64-test.log}; traces/stkcop-clear-model-h547/native-initial-sdk-failure.log; build/meson-logs/pending-clear-fusion-h547-qualified.txt
related: present-pacing-clear-feasibility.546.md; ../../../specs/backend/render-pass-actions/requirements.md; ../../../specs/backend/gap.md
---

# Move from attribution to a shared admission contract

H546 directly priced standalone pending Clear execution and observed 65
necessary-condition candidates. H547 adds `R-BACK-15.19`, a shared pure
candidate predicate and a native bounded value-refinement test. **There is no
production caller, runtime selector or changed rendering policy yet.** The
header does not establish a CPU saving or authorize promotion.

`pending_clear::classify` consumes flat call-local facts and returns a small
typed decision. It performs no allocation, handle resolution, Metal operation
or resource-retention change. Facts include queue owner, source index and
generation, storage page/generation identity, sequence and adjacent command
indices; attachment identities, bound/view levels, slices, dimensions, samples
and resolve/alias facts; requested aspects and serial following load actions.
It rejects unknown/stale identities, incompatible geometry, active/query
scopes, sampled hazards and unsupported conversions before any visible effect.

## What the bounded model checks

The test uses the same C++ classifier intended for the runtime adapter, not
an unrelated test-only admission rule. It compares independent serial
Clear/Store/Load and fused load-action constructions before a deterministic
draw, then checks independent aspect contents after the draw and final stores.

| Check | Final tested population |
|---|---:|
| Complete bounded guard truth table | 16,384 combinations |
| Aspect/value/load-choice/draw states | 311,296 |
| Same final-store-mask comparisons | 2,490,368 |

Color, depth and stencil have independent binary values at two pixels. All
nonempty aspect combinations, independent draw coverage/write masks and
following color Load/Discard modes are considered. For unrequested color
DontCare, both pixel outcomes are independently enumerated and coupled between
reference and candidate; unknown pixels are not collapsed into one fixed value.
Final discarded aspects are excluded through a shared unknown marker. This
is bounded value refinement, not a proof of arbitrary shaders or a GPU driver.

Focused counterexamples reject stale source/storage/attachment generations,
wrong levels/slices, mismatched extents, zero extents, samples/resolve, sampled
aliases and unsupported aspect metadata. Distinguishing memory examples show
under-clear from dropping the extent guard, corruption of unrequested stencil
from combining aspect masks, and loss of exact-reference authority when a
serial following color load discards the Clear.

Two important boundaries are explicit:

- A requested color followed by first-use DontCare is not admitted as an exact
  serial refinement. An unrequested color's existing DontCare remains allowed
  and is modeled independently.
- Pending backbuffer discard is rejected because `clear.has_value()` changes
  the existing discard-after-Present branch. Bare attachment-subset equality
  does not justify crossing that branch.

H546 did not record all of these new facts. Its **65 necessary candidates must
not be relabeled as 65 admitted candidates** under this stricter predicate.
Actual following-load, source/storage and pending-discard projection remains
to be exercised against the retained source.

## Verification and exclusions

The final Meson target `dxmt9-pending-clear-fusion-spec` passes. Independent
`-O2 -Wall -Wextra -Werror` arm64 and x86_64 builds/executions pass with the
same counts; final source/header/binary hashes and snapshots are retained in
`qualified/`. Frozen H537 runtime images remain unchanged. Earlier model
iterations are retained outside that directory and are not the final evidence.

Native build discovery initially did not know the new target; regeneration
then failed because Xcode SDKROOT was absent (`libc++`/`stdio.h` not found).
Reconfiguring the existing native build with the installed SDK path repaired
the build without changing runtime policy or staging Wine files. The failed
configure log is preserved. No Metal or wild run is claimed in this turn.

The equivalent exhaustive checker supplies the bounded semantic layer allowed
by `R-VERIF-1.5`–`1.8`; no new TLA liveness claim is made. Real Pool fact
extraction, format conversion, Metal encoder failure/settlement and pixels
are not proven. A speculative model of successful settlement would not prove
the actual adapter, so that obligation remains open rather than being claimed
through a test-only transaction.

## Next gate

Construct the encode-thread adapter from the real pending command and Draw,
use this classifier unchanged, and test rejection plus failed-open settlement
through the real caller seam. Then compare independent color/depth/stencil
GPU readbacks and the original STALKER capture before an observer-thin finite
A/A and alternating A/B CPU comparison. The full objective—largest default
live single-thread owner and correctness-qualified removable CPU—remains open;
the H546 2.07–2.24 ms observed scope is not yet a saved amount.
