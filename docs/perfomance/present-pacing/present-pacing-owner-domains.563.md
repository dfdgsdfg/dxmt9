---
domain: present-pacing
workload: PE32/PE64 actual D3D9 object and root-observer fixtures
title: "Root ownership observer now retains explicit domains and process CPU"
type: evidence
status: root-integration-passed-process-endpoint-premise-rejected
updated: 2026-10-16
source: traces/stkcop-owner-domains-h563/{contract.md,build.py,build.json,marker.hpp,layout.hpp,epoch-build.json,fixture-run.json,fixture-verification.json,bounds.py,verify_fixture.py}; traces/stkcop-owner-domains-h563/{i686,x86_64}-fixture/{stdout.txt,stderr.txt}; traces/stkcop-clock-bridge-h562/verification.json
related: present-pacing-clock-bridge.562.md; present-pacing-process-accounting.561.md; present-pacing-clear-live.559.md
---

# Private root/process observer integration passes

[H564/H565](present-pacing-owner-live.564.md) later admit the live self/root
results but reject process/residual upper bounds: own-process getrusage has
delayed CPU updates not covered by quantization. Do not reuse
`bounds.py::process_bound` as a qualified live upper-bound provider.

H563 relinks only private PE diagnostic entry objects against the frozen
baseline archives. Canonical renderer code and build images are unchanged.
The diagnostic consumes H562's explicit-domain service at cold binding and
successful top-level Present checkpoints, not per draw/root.

Its distinct map magic and 304-byte row preserve raw RDTSC root endpoints,
cumulative root elapsed/upper values, native self CPU and the full H562 packet,
including process CPU and mapping brackets. Per-attachment mapping intersections
fail closed on changed identity, backward observations or disjoint offsets.
The old 100 ns root endpoint allowance remains unchanged.

## Verified gates

- H524 activation reducer cases pass natively on ARM64 and x86_64 with the
  new map layout.
- Actual PE32 and PE64 fixtures reject wrong native birth, then activate the
  correct epoch 4 through the external controller.
- Each fixture records **20,073 roots / three top-level Present checkpoints**,
  covers all 16 interface tables and 12 exports, and preserves nested-callback
  accounting plus the 20,000 cheap-getter test.
- The terminal decoder verifies publication, row/slot conservation, CPU
  monotonicity, native identity and explicit continuous/absolute joins.
- **17 negative cases** reject mutated identity/schema/publication, changed
  clock mapping, inconsistent process CPU, duplicate known-owner threads,
  negative residual and missing enclosing process endpoints.
- All five staged renderer files are hash-restored after natural Wine exit;
  three temporary services are recovered into evidence storage.

The observed mapping intersection is 10,024,639,924,834–10,024,639,924,916 ns
for PE32 and 10,024,639,924,875 ns for PE64. Bounds are kept explicit; the
decoder does not silently relabel continuous ticks as absolute time.

## Live analysis now supported, not yet measured

`bounds.py::envelope` converts root endpoints conservatively before joining
them with absolute-domain spatial observations. It keeps waits and callbacks
inside the renderer-root upper envelope rather than claiming exclusive CPU.

`bounds.py::process_bound` selects enclosing whole-process CPU checkpoints,
adds the declared 4 us endpoint quantization allowance and subtracts lower
bounds for three distinct threads: Primary, Replay and Encode. It never applies
the single-thread CPU/wall slope to the process counter. The remaining upper
bound includes all game, driver and other workers; it may still be too broad
to order dxmt9 owners.

The next gate is one new contracted late-window ownership episode using the
H559 A Unix image and this diagnostic PE. H560 stays rejected. H559's finite
Encode reduction remains separate evidence; no new CPU/FPS saving, global
owner verdict, common-observer transfer or default promotion is claimed here.
