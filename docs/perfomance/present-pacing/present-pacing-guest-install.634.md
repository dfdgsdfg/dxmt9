---
domain: present-pacing
workload: STALKER guest observer cold-installation preparation
title: "H634 — private PE image installation qualified on synthetic inputs"
type: evidence
status: synthetic-cold-install-qualified-live-game-open
updated: 2026-10-16
source: traces/stkcop-guest-install-h634/{prepare_image.py,blob.S,probe.cpp,layout.hpp,observer.hpp,verify.py,verification.json}; traces/stkcop-guest-install-h634/cold-fixed/{run.json,build.json,image.json}; traces/stkcop-guest-install-h634/cold-fixed/{baseline,inert,armed}/machine.bin; traces/stkcop-guest-install-h634/cold-fixed/{inert,armed}/guest.bin; traces/stkcop-guest-install-h634/{cold-fixture,cold-loader-diagnosis}/run.json; traces/stkcop-guest-install-h634/engine/bin_test/image.json
related: present-pacing-guest-observer.633.md; present-pacing-guest-boundaries.630.md; present-pacing-owner-union-live.629.md
---

# H634 — private executable instrumentation, not decompiler recompilation

The cold-install mechanism passes synthetic Wine controls. A private copy of
the authenticated STALKER executable is generated and statically verified,
but **has not been launched**. Original game files and renderer binaries remain
unchanged. There is no new live CPU ranking, removable-cost result or FPS claim.

## Installation boundary

Decompilation identifies the four caller boundaries; recovered pseudocode is
not compiled into a replacement game implementation. The private PE32 copy adds
one section, an observer DLL import, and four jumps at the H630 Begin, dispatch,
End and continuation sites. A replacement entrypoint initializes the observer
before jumping to the original entrypoint. Existing imports retain their IAT
addresses. The added marker code has no COFF relocations and derives its image
base using call/pop.

Before-call markers recreate the original return address and jump to the
original target. This preserves the hidden dispatcher EAX input and the End
tail-call path. The continuation marker replays the displaced six-byte load.
H633's GPR/flags/LastError/x87/SSE preservation remains in use.

The private copy clears its certificate directory and checksum. It is an
unsigned, local diagnostic artifact, not a distributable game replacement.
The new section combines writable import data and executable markers; this is
not a production hardening design. No live executable pages are patched.

## Frozen synthetic population

| Check | Result |
|---|---|
| Original, inert-marker, armed-marker processes | All exit 0 |
| Inputs per arm | 512: normal, End tail, failed Begin, bypass × 128 flag patterns |
| Captured machine states per arm | 2,048; every captured byte equals original |
| Inert event rows | 0 |
| Armed event rows | 1,408, ordered and native-identity-qualified |
| Armed closure counts | 256 rendered paths, 128 failed Begin, 128 unpaired continuations |
| Shared service restoration | Three temporary files recovered; no remaining fixture processes |
| Protected images | Five renderer and two game images unchanged |

The mapped stream has a 128-byte header and 16,384 fixed 200-byte rows. Cold
initialization pre-touches storage. The hot callback does not format strings or
allocate per event; mapped writes can still incur memory-system cost. Request
1 arms at Begin; request 2 seals at an idle boundary or a completed attempt.
Rows are not an online concurrently readable stream. Readers must wait for
writer quiescence and validate terminal status, identity and complete grammar.
The synthetic test drives the request words itself; an external live controller
is still required.

## Preserved installation failure

Initial patched fixtures exited 5. Loader diagnosis found execution entering
COFF metadata: this local `llvm-objcopy -O binary --only-section=.text` invocation
had retained the object container. The corrected generator extracts `.text`
from the COFF section table, rejects relocations and checks marker prologues.
Both failed runs and their restoration evidence are retained; corrected
artifacts use a new directory rather than replacing failed inputs.

## Catalogue launcher correction

`run_experiment.py --binary` set `DXMT_EXPERIMENT_BINARY`, but the STALKER
launcher passed a hardcoded original executable to `exp_run_wine_binary`.
The launcher now forwards the catalogue-resolved variable. The default still
selects the original engine; explicit diagnostic paths no longer silently run
the baseline. App working directory, configuration and result collection stay
at the existing app root.

`tests/scripts/test_stkcop_audio_controls.py` executes the shell launcher against
a temporary fake app tree. It checks exact binary/arguments/workdir, an override
containing spaces, success/failure restoration and existing audio controls.
Four tests pass; three lane contracts and 18 profile tests also pass. These are
launcher checks, not actual private-game startup evidence.

## Remaining gates

1. Stage private neighboring dependencies and an identity-qualified native
   request controller; preserve the original app and shared-runtime restoration.
2. Verify actual loaded image, entry initialization, thread identity, scene,
   screen and complete stream closure through the catalogue runner.
3. Compare unmodified/inert/armed controls on fixed scene boundaries before
   transferring any observed CPU ranking to the default path.

The synthetic images all loaded at their preferred base; forced relocation is
not tested. Arbitrary SEH, AVX state, unexpected indirect entrances, logger
faults and live perturbation remain outside this result. Synthetic bridge-call
cost must not be subtracted as a universal live overhead correction.
