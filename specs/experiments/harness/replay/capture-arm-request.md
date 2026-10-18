---
type: Spec
title: One-shot external Render Tape arm request
description: Cold PE request admission, bounded expiry and external scene provenance.
tags: [specs, experiments, replay, capture]
---

# One-shot external arm request

Implements R-HARN-REPLAY-9.24. This diagnostic selects a capture boundary, not
a renderer policy or an unperturbed timing sample. Existing capture semantics,
seed drains, first-read content gaps and original GPU oracles remain unchanged.

## Ownership and protocol

With capture enabled, `DXMT9_RENDER_TAPE_ARM_REQUEST_PATH` and
`DXMT9_RENDER_TAPE_ARM_REQUEST_ID` opt in together. The path must be a local
drive-qualified Windows path shorter than 260 bytes. The ID is exactly 32
lowercase hex digits, not all zero. Nonzero skip-Present configuration conflicts
and rejects; an invalid explicit selector must never fall back to skip mode.

The first eligible device owns the process-wide selector; ownership is never
returned on device destruction/reset. At its first successful idle Present,
the PE reader checks that the request path does not already exist and logs
`request_ready` with PID, process creation FILETIME, nonce and GetTickCount64.
The supervisor must wait for this receipt. No selector environment means the
historical skip/retry path; capture OFF creates no selector state or I/O.

The writer runs in the same Wine prefix, opens the exact target PID, checks its
creation FILETIME against the ready receipt and writes a new, exclusive file.
The file is precisely the 64-byte little-endian `RenderTapeArmRequest` schema:
magic/version/length, PID, zero reserved field, process creation FILETIME,
128-bit nonce, issued and expiry GetTickCount64 milliseconds. The writer uses
CREATE_NEW and denies sharing until the complete write is closed. It must not
overwrite, delete or refresh a request. This is a trusted local experiment
protocol, not authentication against hostile same-user filesystem writers.

The PE reader checks only successful eligible Present boundaries, never waits
on an event or GPU and never changes the PE/Unix ABI. Missing file or an active
exclusive writer means pending. All other I/O/size/schema failures reject.
At the first poll at or after 600 seconds since ready, pending terminates.
The external supervisor still needs a watchdog if no Present occurs.
A request is valid only for
the exact PID/creation/nonce, issued no earlier than ready and no later than
the current tick, with a positive lifetime of at most 30 seconds. Equality to
expiry rejects; backward clock movement rejects. The complete read denies
Windows write/delete sharing and holds no borrowed bytes after the call.

## Transition and receipt contract

`Waiting -> Consumed | Rejected` is terminal for admission. A valid request is
consumed **before** bootstrap effects. Bootstrap failure, capture abort, Reset,
publication failure, duplicate requests and subsequent Presents cannot arm a
different interval. Missing/busy requests never start bootstrap. A second
expiry check after seed preparation, before identity arm, aborts late setup.

Logs join accepted nonce/PID/creation/tick to actual successful token/epoch/queue,
then to publication success before identity cancellation. The ordinary identity
and topology companions remain the authority for source identities. A failed
request or bootstrap grants no capture identity. No manifest schema is changed.

## Scene and verification scope

The runtime does **not** read XRay frame/camera memory. A fresh nonce links the
supervisor's coherent before/request/after observations to the actual arm and
publication receipt. The supervisor must independently reject missing brackets,
process replacement, running-episode reset, out-of-envelope camera/frame work,
ambiguous requests, missing source joins or image hashes. The request TTL bounds
admission/setup; it is not proof of scene membership or completion by expiry.

The production pure gate is bound to exhaustive native action histories and
field/expiry counterexamples. PE builds check the real file adapter. Controlled
Wine transport tests and a scene-qualified original-image capture are separate
integration gates. No GPU shader/order/lifetime algorithm changes here; existing
capture regression tests still apply. Selecting an interval does not resolve
mixed Primary CPU ancestry, initial-content authority or default-tail transfer.

## Supervisor usage

Build `dxmt9-render-tape-arm-request.exe` through the ordinary x86 or x64 PE
Meson build. It resides in `tests/conformance/d3d9/` under that build directory
and loads no D3D9 device. Under the target's Wine prefix, invoke:

```text
dxmt9-render-tape-arm-request.exe PATH NONCE PID CREATION_FILETIME TTL_MS
```

Copy PID and decimal creation FILETIME from `request_ready ready=1`; use the
same unique nonce/path configured before starting the game. Use a new private
directory each run. Record coherent scene observations before and after this
command and retain `request_written`, `request_admission`, `request_receipt`
and the original request bytes beside the published bundle. The helper refuses
process replacement, exited targets, malformed input, overwrite and oversized
TTL. Failure is not permission to republish at the same path.

`--self-test NEW_PATH` exercises the Win32 file adapter without D3D9. It removes
only files it created with CREATE_NEW; failed/foreign paths are not removed.
This is transport evidence, not a scene-selected GPU capture.

The bounded runner `scripts/check/test_render_tape_arm_request.py` exercises
the self-test and cross-process writer/reader, including a wrong process-creation
counterexample. It requires an existing initialized prefix and verified Z: -> /
mapping, does not stage runtime DLLs and retains the exact request and image hash.
