---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Qualified compact settlement work removal"
type: evidence
status: native-verified-runtime-pending
updated: 2026-09-08
source: experiments/output/pe-settlement-refinement-20260908/analysis
related: present-pacing-pe-constant-copy.257.md; present-pacing-inline-constant-current.258.md; ../../../specs/d3d9/recorder/spec.md
---

# Qualified compact settlement work removal

## Scope and reference

At `97b0465e`, H257's current PE samples still locate compact settlement,
but its source-level pointer-array hypothesis does not survive inspection:
neither six-entry array exists in either linked DLL. The remaining OFF work
is six presence checks before validation and another six before clear. Empty
ranges already skip the actual dirty comparisons and `memset`; this change
must not claim to remove previously executed clears.

R-CORE-REC-7.61 adds an immutable producer-selected `hasConstantRanges` bit
in existing `SparseStatePlan::SourceFacts` padding. Reset clears it and
successful preparation publishes it once. Qualified const views consume it
only to gate the six-family validation and clear loops. Represented ranges
still require current dirty-extent validation; PendingDelta generation alone
does not prove constant freshness. Generic layout helpers remain range-based.

The private settlement interval has no PendingDelta writer or invalidating
callback. Remove its late duplicate current-ticket check and the recorder
caller's duplicate check; keep fresh source/domain, durable append, transaction
identity and current-ticket qualification before effects. No wire, transport,
queue, resource-lifetime, shader, default or Metal policy changes.

## Bounded verification

The actual build/finalize/owner/accept helpers cover all 64 six-family presence
masks with inline OFF/ON (128 cases), exact canonical bytes against the direct
builder, dirty membership, PendingDelta and scalar token settlement. Additional
traces cover ON/OFF/ON plan reuse, six families times changed/cleared dirty
ranges with retries, wrong transaction generation and token rejection/retry.
Existing owner tests retain wrong recorder/epoch, stale PendingDelta, partial
preparation, non-durable append and rollback coverage. These are bounded
production-bound native refinements, not exhaustive arbitrary D3D9 programs.

Two deliberate production mutations fail the native target: forcing false
constant presence and disabling the initial current-ticket comparison.
After restoration, full canonical builds pass, native focused 9/9 and x86_64
focused 4/4 pass. Logs retain both negative controls and final positive runs.
No new TLA transition or thread policy is introduced; unchanged recorder
models own ordering/lifetime, and native state/byte checks own this local
refinement. No separate shader/GPU oracle is claimed.

## Linked evidence

SourceFacts remains 240/304 bytes and the whole plan 288/368 bytes in PE32/64.
The new bit is at offset 42 before the unchanged PendingDelta offset 48;
native layout matches PE64. Existing enclosing recorder/device pins pass.
Exports, vtable/RTTI owners, options and 12 manifest hot-entry metrics match
H257. The manifest excludes the outlined functions audited separately below.

Both OFF paths now use one byte-load/branch per gated interval; ON preserves
all six validations. No pointer arrays, new allocation or external call appear.
`acceptSparseStatePlan` symbol extents shrink 0x980 to 0x900 (PE32) and 0x950
to 0x8f0 (PE64); these extents include alignment padding. Local stack changes
0x50 to 0x48 and 0x78 to 0x88 respectively, so PE64 has a 16-byte stack cost.
`buildSparseStatePlan` keeps its extent/frame, adding one reset byte store;
the ON preparation helper grows 16 bytes per width. The recorder settlement
caller also shrinks without adding calls. These are code-shape facts, not
measured CPU savings.

Final frozen PE32 SHA-256:
`16d0ad610afd6e9842ca4bccf676eb9050bdfa01d5d64c3df0709709745211dd`.
PE64: `e2d9ae0b3cd086cfc5c46a87e3d51e787b787a1b68abe4589393c4955c287a01`.
DLL sizes remain 1,905,664/1,771,520 bytes. Unix and both bridge DLLs are
byte-identical to H257. Rebuilding after the native-only negative controls
changed only timestamp bytes in the PE images; first-link images and final
images are both retained, and the final code gates were inspected again.

## Runtime gate

Fresh Wine correctness, sampler-OFF 05/06 GT2 CPU/FPS spots and separate full
PE self-PC profiles remain pending. H258's same-binary experiment and the
requested default-path repeat are not measurements of this implementation.
README recovery, repeatable gain and reduced process CPU are not established.
