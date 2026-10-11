---
domain: present-pacing
workload: STALKER CoP Normal replay preparation
title: "Explicit produced-parent allocation planning closes the H481 DF24 metadata gap"
type: evidence
status: implementation-tested-runtime-unconnected
updated: 2026-10-11
source: traces/stkcop-normal-restore-h482/{selected_setup_probe.cpp,selected-setup-probe.txt,selected-setup-probe.stderr,selected-setup-probe-sha256.txt,selected-native-build.log,selected-x86-build.log,selected-native-tests.txt,selected-x86-tests.txt,selected-cpu-regression.txt}
related: present-pacing-normal-setup-closure.483.md; present-pacing-normal-restore.482.md; ../../../specs/experiments/gap.md
---

# Allocate the produced parent without moving its defining write

H483 identified one missing pre-arm parent: DF24 texture `4294967318/g1`.
`buildRenderTapeInitialSetupV4` now accepts an explicit bounded list of exact
late produced parent generations. It authenticates the entire source first,
rejects duplicate/stale/missing/seeded requests, and supports single-level,
single-sample DEFAULT 2D A8R8G8B8/X8R8G8B8 or DF24 allocation only.

The output uses existing `AllocationOnly` ownership: zero seed count, zero
promised content and `NoIndex` for the first seed. The original clear, logical
definition, semantic actions and publication cuts are not edited. Allocation
bytes use an independent bound; wider FP backing is rejected rather than
underpriced as four bytes per pixel. Failures leave the existing destination
plan intact. An empty request retains the historical behavior.

## Same-input production preflight

The retained native probe reruns actual General/V4/initial-setup builders on
H481, once without requests and once with only the missing DF24 identity.
It does not create a device or invoke replay.

| Metadata | Default | Explicit DF24 request |
|---|---:|---:|
| Hidden parent owners | 646 | 647 |
| Related late surface aliases | 11 | 12 |
| Seed operations | 5,044 | 5,044 |
| Seed bytes | 427,768,410 | 427,768,410 |
| Allocation-only bytes | 16,777,216 | 33,554,432 |
| Setup-prefix events | 11 | 11 |

Thus the required additional parent and its alias can be planned without
inventing a seed or changing the command stream. This is metadata coverage,
not actual allocation, local completion, retained-wrapper or GPU evidence.

## Verification and limits

Full native and x86_64 Unix builds complete. Four focused native targets pass;
x86_64 passes the same four plus the Unix install-name audit. The initial-setup
native fixture includes genuine produced color and DF24 clear streams, exact
and one-over capacity, absent/stale/wrong-kind/duplicate/conflicting-generation
requests, a seeded-owner downgrade negative, whole-input tampering and a wider
FP rejection. Existing default-path and ownership reducer tests remain active.

This change is a pure bounded metadata transform. It does not add a runtime
ownership transition, change the queue restoration implementation or publish
work; native fixtures bind the actual planner. No additional interleaving model
is claimed. H482's bounded restore proof still excludes GPU contents and caller
retention. Any later runtime precreation/activation needs its own integrated
ownership and completion verification.

No production provider caller supplies the new request list yet. Required
wrapper retention, authenticated Normal preparation, backbuffer seed settlement,
pre-arm history restoration and original-vs-replay GPU comparison are still
open. The stable-Exact qualification gates are unchanged. No timing, FPS,
largest default-tail CPU owner or removable amount is established here.
