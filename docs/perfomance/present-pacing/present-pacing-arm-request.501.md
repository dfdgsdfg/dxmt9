---
domain: present-pacing
workload: STALKER CoP capture selection control, no game timing run
title: "A one-shot identity-qualified PE capture request replaces skip guessing for the next tail input"
type: evidence
status: controlled-selector-tested-scene-capture-pending
updated: 2026-10-11
source: traces/stkcop-arm-request-h501/{focused-tests.txt,tla-tests.txt,build-x86.txt,build-x64.txt,build-unix.txt,built-images.sha256}; traces/stkcop-arm-request-h501/{wine-x86,wine-x64}/result.json; src/d3d9/d3d9_pe_render_tape_arm_request.hpp; src/d3d9/d3d9_pe_render_tape_arm_request_win32.hpp; tests/native/bridge/render_tape_arm_request_spec.cpp; scripts/check/test_render_tape_arm_request.py
related: present-pacing-tail-input-scope.500.md; present-pacing-normal-history-input.481.md
---

# Select the intended interval before pricing another local optimization

H498–H500 were committed as `a50ba6fd`. H500 excludes the existing original
input from the intended later relative window. H501 implements a bounded cold
control seam; it does not launch STALKER, capture new GPU work or measure FPS.

The capture-only selector reads a new 64-byte request at an existing successful
idle Present boundary. It checks exact PID and process-creation FILETIME,
128-bit nonce, schema, monotonic issuance and bounded expiry. The first device
owns the process-wide request; consumed/rejected admission is never restored
by capture abort or device reset. Bootstrap and identity-arm completion check
expiry again. Request/token/epoch/queue/publication receipts expose the join.

The Win32 file transport was chosen instead of a naked event because the
request carries process incarnation, nonce and deadline. It denies write/delete
sharing while reading; the writer uses CREATE_NEW and exclusive publication.
This is trusted local experiment control, not a hostile-filesystem protocol.
Missing/busy requests wait only through future eligible Presents, bounded by a
600-second gate; an external watchdog remains necessary if Presents stop.
The request lifetime is at most 30 seconds. No new PE/Unix ABI or Metal code is
introduced. Capture OFF retains no request state/I/O; unset selectors retain
the old skip/retry capture path. Explicit malformed/conflicting selectors reject.

## Verified layers

| Layer | Result |
|---|---|
| Production-gate exhaustive native refinement | 117,649 length-six histories; at most one admission, terminal rejection/consumption |
| Packet/clock counterexamples | Schema, PID/creation, nonce, issue/expiry, wrap-sized TTL, backward clock and setup expiry reject |
| Focused x86_64 regression | 4/4: arm request, capture, snapshot-budget transfer, install-name audit |
| Builds | x86/x64 PE and full x86_64 Unix build pass |
| Sikarugir x86 and x64 transport | Missing, exclusive writer busy, exact read, stale initial file, duplicate consumption, no overwrite and short file checked |
| Same-prefix cross-process writer/reader | Wrong creation time rejects before file creation; exact target request is accepted on both architectures |
| Existing TLA regression | Pass, 209.69 seconds; this is regression evidence, not the new selector's model/code proof |

The native test owns the new selector's sequential refinement. The controlled
Wine executable loads no D3D9 device and stages no runtime DLLs. Its self-test
removes only its own CREATE_NEW files; cross-process request bytes are retained.
There is no visual, original-readback, FPS or tail-membership result here.
The existing `g_ntAllocScanHint` unused-variable PE warning remains unrelated.

## Remaining integration gate

The runtime does not read game frame/camera memory. The supervisor still needs
to wait for ready, observe an explicit scene envelope, issue a unique request,
and retain coherent before/after camera/frame/process brackets. Join the actual
armed and publication receipts to authenticated Tape source identities and
original image hashes; reject missing/out-of-window/reset/ambiguous evidence.
Do not infer this join from skip count, filename timestamp or mere request success.

Normal capture's drains, getter-cache exclusion and first-read content-authority
gap remain. Even a successfully selected late input does not resolve mixed
Primary ancestry or establish default-tail CPU prevalence/removable cost.
Next is one supervised scene-qualified capture and original-image replay, not
another H481 micro-cache trial or an FPS claim from the control-path tests.
