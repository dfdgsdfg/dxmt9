---
domain: present-pacing
workload: STALKER CoP Normal replay diagnostic
title: "Retained Normal setup reproduces the original H481 source and output"
type: evidence
status: bounded-original-readback-match-not-timing-qualified
updated: 2026-10-11
source: traces/stkcop-normal-provider-h485/{normal_probe.cpp,probe-r1.txt,probe-r2.txt,probe-r2.stderr,probe-r3.txt,probe-r3.stderr,probe-sha256.txt,commands.txt,native-rebuild.log,x86-rebuild.log,native-final-tests.log,x86-final-tests.log,tla-recheck.log}; traces/stkcop-normal-finish-h481/tape/frame-10699344711800-1/manifest.json
related: present-pacing-normal-produced-setup.484.md; present-pacing-normal-setup-closure.483.md; ../../../specs/experiments/gap.md
---

# Normal attachment setup reaches original GPU readback equality

The private V4 provider now replays the original H481 bundle without switching
the queue to Exact attachment policy. Two fresh native Metal processes complete
the original source boundaries and reproduce both original readbacks exactly.
This removes the immediate Normal setup/restoration integration obstacle. It
does not identify the largest default-tail CPU owner or a removable CPU budget.

## Boundary implemented

`prepareRenderTapeIdentityV4` authenticates action/history companions and owns
the resulting metadata. It derives the missing produced DF24 allocation without
moving its original write. The runtime prepares and retains late surface
wrappers before arm, completes local setup, verifies parent/level bindings and
restores the original touched/discard history using the H482 primitive.
ObjectDefine transfers the retained wrapper rather than reacquiring from the
texture's weak cache. Prefix backbuffer seeds use actual checkpoint completion;
post-prefix backbuffer mutations reject before device effects. No Exact toggle
or seed-derived touch override is used.

`RenderTapeNormalSetup.tla` covers the composed setup/restore/arm/activation and
reference-ownership sequence, with negatives for temporary alias release, late
seed-touch overwrite and early arm. The first verification run found an invalid
TLC initial-state expression (`~seeded` without assignment); explicit Boolean
initialization fixes it. The retained recheck passes the complete inventory:
68 safety, six progress and 163 expected counterexamples, 237 configurations.
The model excludes physical contents and production GPU retirement internals.

## Original-input diagnostic

The runner verifies the H481 manifest SHA-256
`ab9b808f08c071cef1f4dd64d498ec87333109f9cec9a236e84536c0854f3e85`,
component digests and blob hashes. It uses the actual prepared V4 API with
ProductionSlot, serial partition mode and a fresh device. Shader archive access
and prewarm are disabled in-process; no Wine installation is staged or changed.
The native toolchain doctor succeeded on Apple M1, Xcode 27 / Metal 4.

| Evidence | r2 | r3 |
|---|---:|---:|
| Required surfaces restored | 16 | 16 |
| Late aliases prepared / activated | 12 / 12 | 12 / 12 |
| Wrappers created / released | 779 / 779 | 779 / 779 |
| Original source cuts mapped | 3 | 3 |
| Normal policy unchanged through finish | yes | yes |
| Source bytes equal to original | 3,686,400 | 3,686,400 |
| Output bytes equal to original | 3,686,400 | 3,686,400 |
| GPU command-buffer errors | 0 | 0 |

Both source/output SHA-256 values are
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
Format 22 is BGRX despite the original `.rgba` extension; comparisons are raw
bytes, with no channel conversion, masking or tolerance.

The first attempt omitted the existing `PePresentReservationV1` producer
contract. It restored history and balanced all wrappers but correctly failed
`v4-exact-source-publication` at event 6051, before Present. The production PE
capture adapter performs reservation before that boundary; r2/r3 explicitly
attest that existing contract. No publication comparison or completion check
was weakened. The final runner source is the corrected r2/r3 version; r1 logs
remain as failure evidence.

## Verification and remaining qualification

Full native and x86_64 Unix builds succeed. Focused native tests pass 5/5;
x86_64 passes 5/5 including the Unix install-name audit. Provider fixtures cover
companion binding, wrong digest, partial input, policy ABA, missing history,
foreign capture epoch, null-device non-authority and every counted synchronous
allocation failure in immutable preparation. H482's actual queue tests and
H484's allocation-only planner tests remain active.

This is a bounded original-input execution/readback proof, not transparent
capture timing or all-resource content validity. Read-first resource authority
is still unknown; a final image match does not prove every intermediate byte.
The runner is native arm64, not a Wine/Rosetta parity result. It measures no
CPU owner, removable amount or FPS, and the input is not a whole-tail sample.
The stable-Exact launcher gate remains closed to Normal inputs. Next is bounded
first-read content closure and representative input selection before repeated
same-input CPU policy experiments; no global Normal promotion follows here.
