---
domain: present-pacing
workload: 3DMark05 GT2 fixed capture
title: "H363: Unconditional serial binding-plan reuse"
type: evidence
status: partial-attribution
updated: 2026-09-25
source: experiments/output/binding-plan-default-h363/comparison.json; experiments/output/binding-plan-default-h363/{default,retired-zero}/result.json
related: present-pacing-binding-plan.362.md
---

# H363: Explicit promotion and OFF-policy removal

Following the user's explicit request, serial DrawRun now always constructs
its command-local binding-plan owner. The environment lookup and optional-owner
OFF branch are removed. `DXMT9_COMMAND_BINDING_PLAN_REUSE=0` no longer disables
reuse; no runtime code reads that selector.

Canonical preparation remains necessary for parallel children, standalone
draws without the capability and mismatched borrowed state. These are safety
and coverage fallbacks, not an OFF policy. Source lifetime, concrete backing
snapshots, dynamic stream/raster preparation and submission order are unchanged.

The historical OFF/ON checker remains useful for archived H362 receipts.
Its `--default` mode instead requires nonzero builds/reuses in both an unset
selector run and a retired-selector=`0` run, together with identical binaries,
inputs, controlled environment and exact nondegenerate source/output pixels.
Native tests reject reintroduction of the runtime selector and exercise both
checker modes.

This is a user-selected default policy, not a new FPS-benefit claim. H362's
same-large-source spike attribution and longer observer-OFF performance
qualification remain open.

## Final-binary verification

Native and x86_64 Unix full builds pass; native focused tests 3/3 and x86
binding/install-name tests 2/2 pass. No ABI, resource lifecycle or scheduling
transition changed in this promotion; H362's 210.83-second TLA regression is
the preceding model evidence, not a newly rerun check here.

The same GT2 input and captured source plan run twice: selector unset, then
retired selector `0`, each with two warmups and four measured iterations.
All 12 iterations pass GPU exact, with zero GPU errors. Each block reports
3,876 plan builds and 7,320 reuses. Both source and output readbacks retain
H362's SHA-256 `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
The checker verifies identical images/inputs and that `0` cannot restore OFF.
No new wild or FPS comparison is claimed by this policy-only verification.
