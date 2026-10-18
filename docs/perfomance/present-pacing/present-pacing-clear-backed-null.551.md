---
domain: present-pacing
workload: STALKER CoP pending Clear fusion feasibility
title: "Backed NULL RT projection enables all 65 captured Clear fusions with original image equality"
type: evidence
status: private-original-oracles-passed
updated: 2026-10-15
source: traces/stkcop-clear-backed-null-h551/{contract.md,build.py,build.json,build.log,caller_spec.cpp,build_test.py,test-build.log,run_test.py,serial-test.json,fused-test.json,serial-test.log,fused-test.log,run_capture.py,capture-serial-receipt.json,capture-fused-receipt.json,capture-serial.log,capture-fused.log,dxmt9_draw_encoder_chunk.mm,dxmt9_pending_clear_projection.hpp,dxmt9_pending_clear_fusion.hpp,native-test.log}; tests/native/backend/pending_clear_projection_spec.cpp
related: present-pacing-clear-caller.549.md; present-pacing-clear-feasibility.546.md; ../../../specs/backend/gap.md
---

# Refine physical participation without changing the serial renderer

H550 showed the initial blanket NullRt exclusion rejected all 63 depth-only
Clear candidates. H551's adapter follows the existing serial descriptor:
non-null texture means a participating physical attachment, even for logical
`Format::NullRt`. Actual texture/descriptor extent agreement remains required.
Directly allocated storage-free NullRt surfaces remain rejected, not silently
represented as backed or absent attachments. The pure admission predicate and
H549 private encoder caller are otherwise unchanged.

This is an adapter correction, not a change to NullRt allocation, D3D9 format
support or serial attachment inclusion. Matching binding identities reuse one
physical projection. All source/order, sampling, alias, query, conversion and
following-load guards remain.

## Evidence

The native projection fixture now creates both actual `Pool::createSurface`
storage-free NullRt and `createTexture`/`createSurfaceForTexture` backed alias
forms. The former rejects; the latter admits the depth-only case. Both native
predicate/projection tests pass.

The private actual-caller fixture runs:

- Seven ordinary-color aspect masks inherited from H549 in exact-tape mode.
- Seven backed-NullRt masks under ordinary stores and another seven under
  exact-tape stores; all 21 cases pass separately in serial and candidate.
- Four extended-branch null-encoder failures for each of those three fixture
  groups: 12 failures retain pending source/command identity, return no
  successful submission and leave seeded attachment contents unchanged.
- Candidate witnesses prove 12 successful extended-branch openings. Other
  masks intentionally exercise the unchanged equality branch.

Color, depth and stencil are read independently against fixed expected values.
The fixture skips actual Draw emission to isolate load/store; it does not
prove shader correctness or every future store-proof suffix. No new Metal
validation failures are observed. Native fixtures are not timing runs.

The original H502/r5 capture then runs in two fresh processes with real draws:
serial and candidate both match the original source/output hash
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
All seven source mappings, nine Issues, five polls, four results and balanced
670 resource wrappers pass. Candidate witness now records **65 fusions**:
63 depth-only and two color-only, all in local sequence 8/captured source 5.
Loaded private image paths/hashes are pinned. Hardware query visibility is
not covered by the production-counter transcript; capture content authority
and live timing qualification remain unchanged.

## What this permits next

All 65 intended local calls are now eligible with actual-caller and original
pixel evidence, so a fixed observer-thin local CPU comparison is meaningful.
It does not yet prove a CPU saving, live-tail prevalence or the largest
default single-thread owner. Ordinary live query/terminal interactions and
broader format/store suffixes remain promotion gates. Canonical encoder
sources, Wine staging and default runtime policy are unchanged.

The subsequent [H552 finite comparison](present-pacing-clear-economy.552.md)
establishes a local CPU saving; it does not close those live promotion gates.
