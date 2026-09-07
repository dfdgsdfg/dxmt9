---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "External native commit-thread CPU attribution"
type: evidence
status: measured-attribution-no-claimed-gain
updated: 2026-09-08
source: experiments/output/pe-native-thread-cpu-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-native-thread-cpu-ad2e58c4-gt2-off-threadcpu-r1-20260908
related: present-pacing-pe-constant-loop.267.md; present-pacing-pe-compact-binding-validation.262.md
---

# External native commit-thread CPU attribution

## Question and boundary

H267 reduces linked constant-loop work but does not establish CPU savings.
Its two-second process observer combines game, PE, Wine, replay, encode and
other threads. The new external observer separates the native thread entering
the PE command-chunk bridge from the whole process without suspending it or
adding a PE hot-path probe. It changes no runtime binary, wire or provider.

`dxmt9c_device_commit_chunk_impl` in `src/d3d9/device_c_chunk_replay.cpp`
emits `unix_commit_chunk_entry` before offload admission. Its existing Info
marker uses `pthread_threadid_np`; the asynchronous `ReplayOffloadWorker::run`
calls `replayRawChunk` instead and does not enter this marker. A synchronous
readback/inline replay still consumes the selected caller's CPU.

This is **commit-entry-thread CPU**, including game/Wine/synchronous Unix,
not PE-exclusive CPU or all PE callers. The runtime does not log recorder
lock/BehaviorFlags here, so the observer cannot prove that every PE setter
uses this thread. A per-thread marker latch also hides another device on the
same thread: require exactly one successful canonical device negotiation as
well as one device/native-TID marker pair in a fresh complete run log.

## Public Darwin API and calibration

`tools/dxmt9_thread_cpu.cpp` builds as the host-native
`build/tools/dxmt9-thread-cpu` without renderer dependencies. A snapshot queries
`PROC_PIDTBSDINFO`, then `PROC_PIDTHREADID64INFO`, then the process identity
again. It records PID/start seconds/microseconds, the 64-bit native ID,
user/system nanoseconds and a `CLOCK_MONOTONIC` query bracket. Short/error
queries preserve flavor, returned/expected bytes and errno, and exit nonzero.

No task-port acquisition, target suspension, thread enumeration or privileged
fallback is used. In particular `PROC_PIDLISTTHREADS` does not enumerate this
native-ID namespace. Apple's [thread-info implementation](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/bsd_kern.c)
converts Mach basic-info seconds/microseconds to nanoseconds for these fields;
its [proc-info dispatch](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/proc_info.c)
selects the unique-ID lookup for `PROC_PIDTHREADID64INFO`. No Apple
implementation source is copied into the project.

The local SDK declarations alone do not document the time units. The tool's
`--self-check` compares controlled busy/idle CPU with `THREAD_BASIC_INFO`.
A separate live same-user peer runs 200 ms of measured thread CPU, remains
alive for external queries, and verifies nonexistent-TID and reaped-PID
failures. This proves this host's API/units and peer access, not permission on
every macOS process. Real Wine access is a separate runtime gate.

## Collection and acceptance

```sh
meson compile -C build dxmt9-thread-cpu
meson test -C build dxmt9-thread-cpu-calibration dxmt9-thread-cpu-peer \
  dxmt9-observe-darwin-thread-cpu
scripts/run_python.sh scripts/tools/observe_darwin_thread_cpu.py \
  --pid <actual-benchmark-pid> --log <fresh-run>/dxmt9.log \
  --output <fresh-run>/native-thread-cpu.jsonl --samples 120 --interval 2
```

The benchmark still launches through `run_experiment.py`. Info logging is
required for the existing marker, so these are diagnostic runs, not replacements
for Warn-profile FPS. The stopped-PC sampler and detailed PE timing remain off.
The collector records helper identity; the supervised recipe also preserves
five runtime images, helper source/script identities and calibration output.

`observe_darwin_thread_cpu.py` bounds incremental reads and rejects log
rotation/truncation, ambiguous devices/IDs, non-contiguous transport,
process-start changes, malformed snapshots and excessive query duration.
It retains selector and file metadata and rejects logs last modified before
the target process started. These checks do not authenticate causal log
ownership: standalone callers must still supply the correct fresh run log.
It uses the same explicit monotonic
clock as the helper. The reducer only joins adjacent successful samples whose
before/after log-frame endpoints are stable and inside the useful scene.
Loading, static result windows, errors and ambiguous endpoints do not become
zero CPU or bridge an unobserved gap. CPU/time regressions and mixed identities
fail closed. Empty coverage is `unmeasured`, not zero cost.

Frame endpoints remain approximate log progress, not exact Present timestamps;
query round-trip latency is retained and queries above 100 ms are excluded.
Host scheduling and observer subprocess overhead remain real. These receipts
alone cannot prove a product CPU reduction, critical-path latency or FPS gain.

## Verified runtime receipts

Runtime HEAD `ad2e58c4` has the same five PE/bridge/Unix images as H267's
`f928ba0c` candidate; only the new external observer is under development.
Recipes freeze its executable/source/script identities. Native calibration,
same-user peer/CLI tests and 15 deterministic log/receipt/delta cases pass
through the three focused Meson targets. Full canonical PE32, PE64 and
x86_64 provider builds finish before either wild run. The observer itself
is a native arm64 executable, not ARM64 code injected into the x86 PE.

Post-run review tightens standalone transport/freshness checks and serializes
selector/file metadata; live peer tests exercise these gates. The measured
collector is preserved byte-for-byte as `analysis/observe_darwin_thread_cpu.measured.py`
matching the recipe SHA-256. The supervised recipes already enforced a fresh
output directory and unique contiguous selector, so the recorded populations
remain valid; post-review reducer revalidation exactly reproduces their sums
and accepted intervals.
Standalone CLI output is raw JSONL; the supervised recipe calls the reducer
and separately qualifies rendering, images and selector provenance.

Both runs use Sikarugir cx-24.0.7, named GT2, frame sampling and Info logging,
offload ON, inline constants/segmented transport/Tape/index reorder OFF.
Resolutions remain 1024x768 (05) and 1280x720 (06). No build, test or other
Wine benchmark overlaps the scenes. Five-image checks pass against both the
frozen recipe and the actual Wine root. Each fresh log has exactly one
canonical contiguous negotiation and one native-ID marker pair, and the
live target API resolves that ID inside the recorded PID/start lifetime.

`qualify_thread_runs.py` separately pairs the same polling iterations of
process and thread data; `thread-runs-qualified.json` retains the arithmetic.

| Product | Scene / CPU-joined frames | Joined CPU coverage | Commit-thread CPU ms/frame | Average occupied cores | Same-interval process CPU ms/frame | Thread share of process CPU |
|---|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 1,920 / 1,255 | 65.4% | 33.826 | 0.944 | 76.263 | 44.4% |
| 06 GT2 | 1,155 / 1,049 | 90.8% | 52.550 | 0.965 | 95.434 | 55.1% |

05's joined thread counters contain 42.198056 s user + 0.253539 s system
over 44.954956 s; 06 contains 54.954025 s user + 0.171281 s system over
57.136463 s. Whole-process snapshots precede thread queries in each polling
iteration and are not atomic with them. Partial populations differ by scene;
do not use the full-scene FPS reciprocal to reinterpret the joined CPU/frame.
The helper round-trip medians/maxima are 3.428/4.509 ms (05) and 3.804/4.448 ms
(06), not helper CPU measurements. All queries stay below the 100-ms bound.
05 has 3 pending-selector rows and 4 excluded endpoint rows; 06 has 12 pending
rows and 78 excluded endpoints, mostly loading/result-window observations.
Neither run has a query error, mixed selector or decreasing counter.

Both render runs pass with eight reported failure counters zero. 05 exits
normally; 06 times out in the completed GUI under the catalogue's policy.
06's final error snapshot at Present 2,100 does not cover the unreported tail
to frame 2,151. Scene populations are positive-wall frames 1-1,920 for 05 and
997-2,151 with more than 100 draws for 06. The diagnostic scene averages are
29.702/18.019 FPS over 64.642/64.098 s. They must not replace H267's Warn
29.989/18.270 values or become an A/B claim; README and provider defaults stay
unchanged. These runs add no whole-scene pixel-equivalence proof.

## Decision and next gate

The selected commit-entry thread consumes nearly one core in both retained
scene populations. Its measured cost is predominantly user time, so removing
CPU waits alone is not the obvious explanation for this pressure. This is
direct evidence that useful CPU-side work remains to be localized, not proof
that the whole measured time belongs to dxmt9 PE or that the GPU is irrelevant.

H267's PE self-PC buckets and these CPU counters come from different runs and
populations. Do not multiply those bucket percentages by these times to claim
PE-exclusive milliseconds. The next candidate must retain a matched thread-CPU
population, current-image symbol ownership and separate Warn FPS evidence;
constant entry/materialization and first-occurrence admission/retention remain
measured PE candidates. Game/Wine/PE domain attribution and observer economy
remain open. This observer improves candidate pricing; it is not itself a
production CPU optimization or a completed README recovery gate.
