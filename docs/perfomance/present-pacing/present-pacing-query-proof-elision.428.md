---
domain: present-pacing
workload: STALKER CoP Day
title: "H428: Omit unconsumed store proof under query preservation"
type: implementation-validation
status: native-validated-wild-performance-open
updated: 2026-10-01
source: traces/query-ranges-h428/{build.log,build-final.log,tests-final.log,testlog.txt,tla.log,binaries.sha256}; src/dxmt9/dxmt9_render_pass_encoder.mm; src/dxmt9/dxmt9_render_pass_store_policy.hpp; tests/native/backend/query_preserved_pass_spec.cpp
related: present-pacing-pass-proof-audit.427.md; present-pacing-drawrun-preparation.426.md
---

# H428: Query-preserved passes need no unobserved live-out proof

Base `45a33028` plus implementation/tests. The production pure selector
`renderPassStoreProofRequired(queryPreserved, observeProof)` permits omission
only for query preservation with perf OFF. `prepareRenderPassWithStoreProofLookahead`
uses it at the color and depth/stencil scans. No new knob, cache, diagnostic
payload, resource ownership or scheduling change is introduced.

Query preservation already makes DontCare and provisional Unknown ineligible.
The omitted result therefore cannot change a store action. Resolve selection,
load/clear decisions, included attachments, late-store ledger construction and
query/completion ordering remain unchanged. Perf ON executes the original scans
and emits real proof/cause counters; the omitted path emits no synthetic proof.
Tape-only exact preservation and non-query paths retain their original scans.

This is pure value-computation elision, not a change to load/store actions or
interleavings. Native predicate/output checks are the specific refinement layer;
the unchanged TLA suite is regression coverage, not a new theorem about this
implementation or Metal behavior.

## Evidence and limits

- Canonical x86_64 Unix build completes, including install-name fixup.
- The unchanged TLA suite completes all 227 configurations with exit status 0,
  including the expected-counterexample checks. Runtime/test binary SHA-256
  receipts are retained in `binaries.sha256`; no Wine staging occurred.
- Six focused tests pass: render-pass actions, both new prepared-output modes,
  range OFF/ON segmented offloaded GPU query/pixel oracles and install-name audit.
- Each prepared-output mode covers 192 combinations using actual Metal surface
  allocations: MRT, NULL RT0, D16, D24S8, stencil-only, single-sample/2x MSAA,
  clear/no-clear and all future-source/invalid/truncated-lookahead flag combinations.
- Each mode compares actual prepared descriptors, action summaries and ledger
  fields against an empty-suffix conservative preparation in the same process.
  Perf ON retains proof evaluation; perf OFF exercises omission. These are not
  byte comparisons of different process handles or of an archived old binary.
- Explicit expected Store/StoreAndMultisampleResolve checks and no-Unknown checks
  accompany descriptor equivalence. Existing action tests retain aliasing,
  replay-order and late-store counterexamples; the new fixture is not an
  exhaustive cross-product of every resource format or aliasing configuration.

No new wild run or observer-OFF performance comparison has been performed.
H426's 22.466 ms includes observed proof and other work; it is not the saving
from this change. Because perf ON intentionally preserves the original work,
rerunning that diagnostic lane is not an overhead-free test of this improvement.

## Next gate

Use a pinned before/after observer-OFF Day comparison, qualify loaded binaries
and visual/query behavior, and only then assign a CPU/FPS benefit. A dedicated
same-input preparation benchmark can isolate the eliminated walks separately
from pass creation. StartRenderPass lookahead-view construction and perf-only
summary re-traversal remain present; this change does not remove those costs.
Query range policy remains default OFF.
