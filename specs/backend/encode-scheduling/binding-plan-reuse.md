---
type: spec
title: Command-local binding plan reuse
description: Frozen sampler projection with per-draw stream and raster derivation
tags: [encode, refinement, ownership]
---

# Command-local binding plan reuse

The reference is `makeDrawBindingPacketPlan` for every draw. Under
R-VERIF-1.5–1.8, a candidate must produce an equal packet at every draw, not
merely at the first draw of an encoder.

The serial DrawRun call owns a non-copyable, synchronous binding-plan owner.
It borrows the command's immutable hot state and pixel-shader metadata; neither
may change until the owner is destroyed. The owner must not escape the command,
cross a source boundary, or be shared with a parallel child. Address equality
is valid only inside this lifetime, never as a persistent resource identity.

Fragment and vertex texture/sampler projections may be prepared once. They
include handle generations, texture LOD, sampler hash and complete sampler
state, and shader sampler-use metadata. Stream projection (including UP,
instancing and startVertex) and raster projection (target extent, pretransformed
layout, debug overrides, viewport and scissor) must be recomputed per draw.
An ordinary preparation must match the owner's borrowed hot/shader identity.
The command-local `applyDrawBindingOverride` transition also preserves sampler
inputs and pixel shader: it changes only stream/index fields and vertex
declaration streams. Its result may use explicit stream-override preparation;
the native differential test must invoke that production transition. Any future
override-schema extension must re-audit this permission and extend the
counterexample domain before adding sampler or pixel-shader mutations.
Uniform-only and alpha-only changes do not invalidate sampler projection.
Buffer backing snapshots are deliberately not cached: their
per-draw binding consumer still resolves the original snapshot. Their presence
alone does not invalidate this value-only plan (no native resource pointers).

Candidate output is consumed synchronously before the next draw updates its
dynamic fields. No queue, lifetime, submission, or GPU-completion policy changes.
Missing capability or mismatched borrowed state must fall back before binding.

Verification: bounded A/B/A sequences compare the production owner against the
reference including foreign state, sampler/LOD changes, UP, offsets and raster
changes. This is the sequential refinement layer; it does not prove Metal
binding or pixels. Meaningful GPU exact comparison and supervised wild evidence
are required before default promotion.

Diagnostics must not resolve textures or clock cache stages when their owning
observer is disabled. Source planning diagnostics use bounded supply-trace
events qualified by backend, sequence and thread. Child phases partition
observation, graph construction, lookahead, natural-frontier planning, optimizer,
and replay selection; they must not be interpreted as FPS or GPU cost.
