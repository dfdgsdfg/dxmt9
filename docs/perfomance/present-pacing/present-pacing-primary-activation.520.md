---
domain: present-pacing
workload: Native and actual PE32/PE64 root-gap observer qualification
title: "Epoch-qualified activation survives cold D3D9 reloads"
type: evidence
status: observer-authority-qualified-no-performance-result
updated: 2026-10-14
source: traces/stkcop-primary-epoch-h520/{contract.md,epoch.hpp,control.cpp,spec.cpp,build.json,run.json,verify.py,verification.json,failures.md}; traces/stkcop-primary-epoch-h520/{i686-run,x86_64-run,attempt1}
related: present-pacing-primary-gap-epoch.519.md; present-pacing-primary-gap-live.521.md
---

# Activation authority, not first-image identity

H520 replaces H519's first-attachment assumption with an immutable activation
receipt. The independently checked process birth, current attachment epoch,
guest/native thread keys and slot count must remain unchanged around the sole
request `0 → 1` CAS. Historical slots must be empty. The terminal parser must
reject any subsequent attachment change, new binding or retired-slot activity.
H519 remains rejected under its original contract.

## Verification

- 28 native cases pass on each of ARM64 and Rosetta x86_64.
- Actual PE32 and PE64 object helpers perform four DLL load/unload epochs.
  Wrong process birth is rejected; activation in epoch 4 succeeds.
- Each map contains four cold binds, eight gap queries, four completed rows,
  and no pending interval. The request/kind pairs are `(1,1)`, `(7,1)`,
  `(8,2)`, `(9,3)`, preserving Device Present, PresentEx, SwapChain Present
  and nested-callback coverage from H518.
- `verification.json` checks original maps, receipts, image hashes and recovery.
  Five existing shared files are restored and three service files recovered.

The first runner attempt timed out because buffered text `readline()` hid a
ready line from `select()`. It is retained under `attempt1/`; recovery waited
for actual helper termination. The corrected runner uses unbuffered binary
reads. No identity or clock predicate was weakened.

These are observer tests, not a rendering optimization or CPU result. H518's
diagnostic images remain unchanged. Largest default renderer ownership and
correctness-qualified removable CPU are still unresolved; the live utility
test is [H521](present-pacing-primary-gap-live.521.md).
