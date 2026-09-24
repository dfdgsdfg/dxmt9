---
domain: present-pacing
workload: 3DMark05 GT2 fixed capture
title: "H360: Default prepared serial reads; retire OFF policy"
type: evidence
status: partial-attribution
updated: 2026-09-24
source: experiments/output/prepared-default-h360/summary.json; experiments/output/prepared-default-h360/{default0,default1,timer_off}/{result.json,provider.stderr}; experiments/output/prepared-default-h360/{dxmt9-h360-native.log,dxmt9-h360-x86.log,dxmt9-h360-unix.log,dxmt9-h360-tla.log}
related: present-pacing-prepared-fused.359.md
---

# H360: Explicit default promotion, not a performance verdict

Base `bd08a920` plus this policy change. The user explicitly requested default
prepared serial reads and removal of the legacy OFF policy after H359's exact
GT2 qualification. Eligible ordinary Slots now always build fused serial read
metadata. `DXMT9_PREPARED_SERIAL_READS` is no longer read by runtime code;
setting it to `0` cannot restore reference-only execution.

The unused full-plan serial consumer is removed from the production encoder
header and retained only as a test-local differential oracle. The general
canonical plan still serves the separate scheduling foundation and diagnostic
shadow; it is not a selectable old serial implementation.

Necessary failure/unsupported-input handling is not removed: allocation and
capacity failures, stale identity, missing rows, Arena payloads and non-identity
execution continue to use the serial raw read-hazard calculation before draw
effects. Removing this safety path would require new coverage or would reject
previously supported work. Pins, stamps, parallel admission and source lifetime
are unchanged.

`measure_prepared_serial_cost.py` now measures two default-path blocks and a
build-timer-OFF control. It no longer presents two identical default paths as an
OFF/ON experiment. `check_prepared_serial_pair.py` explicitly validates archived
pre-promotion receipts and still requires zero OFF consumption, so current
receipts cannot masquerade as historical reference runs.

H359's unresolved throughput result remains unresolved. This is a user-selected
policy simplification with bounded correctness evidence, not a new FPS win or
proof of all-workload performance neutrality. Broader wild qualification,
Arena/parallel metadata use and full Replay-emission fusion remain open.

## Final default-binary verification

- Native focused tests: 5/5, including a retired-selector `0` rejection test
  that requires actual prepared ownership after queue publication.
- x86_64 focused, lifecycle, join, shadow and install-name tests: 9/9.
- Full Unix build: pass. PE ABI is unchanged.
- Existing TLA regression: pass, 195.54 seconds.
- Counter schema/table/callsite and changed-document source audits: pass.

The final provider runs the same GT2 input and authenticated source boundaries
without the retired selector. Three blocks (two default repeats, one build-timer
OFF) each execute two warmups and sixteen measured iterations. All 54 iterations
pass and consume 646 prepared rows each. Attached-row fallbacks and GPU errors
are zero. All source/output readbacks match the preserved H359/H358 oracle
byte-for-byte with SHA-256
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
The runner verifies equal loaded-image and input fingerprints across blocks;
receipts confirm the retired selector is absent.

Prepare-slot elapsed medians are 0.879/0.875/0.851ms and Encode medians are
40.353/40.108/40.400ms. There is no OFF implementation in this binary, so these
are repeatability observations, not a policy speedup comparison or a live FPS
qualification. Tests and builds completed before the replay campaign.
