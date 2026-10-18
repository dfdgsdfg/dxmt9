---
type: "Spec Detail"
title: "PE QPC Point Calibration"
description: "Directly observed PE endpoint clock bounds without extrapolation."
tags: [specs, experiments, harness, tracing, clocks]
---

# PE QPC Point Calibration

Owns the PE implementation detail for R-HARN-TRACE-3.1, 4.3, and 9.6.
The trace profile's 1000 ns qualification limit remains unchanged.

## Failure being corrected

The earlier batch sampled QPC around the entire transport call and reused that
call's calibration for the next chunk. Its measured error was 3383–8292 ns on
x86 and 2708–5725 ns on x64. More fundamentally, all six chunk endpoints in
each capture lay outside those previous-call validity ranges. Lowering a
reported uncertainty alone could not qualify them. The point protocol below
removes that uncovered-range dependency and shortens the observed bracket.

## Observation protocol

With PE tracing explicitly enabled, each chunk endpoint observes:

1. A minimal unix bridge probe reads the native monotonic clock and returns.
2. PE reads the original `QueryPerformanceCounter` value.
3. A second unix probe reads the same native monotonic clock.

Program order encloses that QPC observation between the two native samples.
Both probes and batch admission must agree on the session generation and
native thread identity. The unix owner assigns the normal session thread
track; a native TID is metadata, not a trace UUID.

The bounded selection rule tries at most eight candidates per endpoint,
stopping on the first candidate whose conservative uncertainty is at most
1000 ns. If all valid candidates are slower, it retains the smallest observed
bracket with its actual larger uncertainty. A failed calibration does not
license extrapolation, a fabricated timestamp, or a complete trace claim.

The selected QPC read defines the observed endpoint. Sampling can displace the
endpoint relative to the underlying chunk call; its overhead is not subtracted
from elapsed scope time. The opt-in budget is at most 32 clock-probe bridge
calls and one two-event batch per chunk. There are no per-draw bridge probes.
The disabled path retains the existing cached boolean gate.

## Representation and error

Batch ABI v2 carries each endpoint's original QPC tick, native lower/upper
bounds, host resolution, and common generation/thread identities. It replaces
the previous-call calibration that could not cover the next chunk's endpoints.
PE and unix binaries must be rebuilt together through the generated bridge.

The mapped event uses the integer midpoint of its native bracket. Its declared
uncertainty is at least:

```text
ceil((host_after_ns - host_before_ns) / 2)
  + ceil(1_000_000_000 / qpc_frequency)
  + host_resolution_ns
```

The `point_bracket` calibration applies only to its exact original QPC tick:
`source_before == source_after == valid_begin == valid_end == source_ticks`.
Every begin/end owns a distinct calibration ID. The retained affine coefficients
describe that point only; they do not authorize interpolation between events.
No equal-rate or drift assumption is needed to bound the observed native time.

Raw and native Perfetto annotations retain QPC ticks, source API/unit,
calibration identity, native bracket, resolution and uncertainty. Normalization
checks the midpoint, point validity, declared frequency, minimum error,
generation and event/metadata agreement. It rejects a different source tick,
underclaimed uncertainty, missing mapping or an error above the frozen policy.
High-error observations remain available for diagnosis.

The earlier x86/x64 captures documented in [evidence](evidence.md) remain failed
historical evidence. New captures must independently pass the native Trace
Processor, source identity, coverage, loss and clock gates.

## Qualification

Apple M1 / macOS 27.0.1, Sikarugir `sikarugir-cx-24.0.7`, the self-checking
`wine_triangle.cpp` fixture, and a 500 ms `timeline --trace-include-pe` window:

| Lane | Repetitions | PE endpoints | Declared error | Result |
|---|---:|---:|---:|---|
| Wine x64 | 3 | 18 | 247–810 ns | All mapped; zero loss; pinned TP qualified. |
| Wine x86 / WoW64 | 3 | 18 | 476–956 ns | All mapped; zero loss; pinned TP qualified. |

Every capture has 43 events including six PE endpoints, each with its own
point calibration. First and last endpoints require no preceding/following
batch. The native consumer validates original source tick/API/unit, midpoint,
bounds, uncertainty, native track and guest identity annotations. The frozen
clock-error policy remains 1000 ns. An additional first x64 capture qualified
at 247–560 ns. No out-of-range observations were removed to obtain these results.

Three matched baseline/timeline pairs per architecture (12 runs) all pass and
produce `actual.bmp` SHA-256
`e1a0b87d3e0ad47602a8dbc2983e5105aeeb9ccca3501ad1974c678545b97567`.
They qualify output and capture correctness. The wrapper's 1.5 s dwell and
catalogue wall time do not measure application throughput or probe overhead.
The bounded probes can perturb chunk duration and select a later endpoint;
these results do not establish negligible observer cost or arbitrary guest
function durations.

The existing prefix failed before tracing/device initialization with
`CreateWindowExA` / `The explorer process failed to start.` on both baseline
and requested tracing. Those failed attempts are retained. A fresh isolated
`experiments/prefixs/tracing-pe-clock-v2` prefix using the same accepted runtime
passed all runs above; the old prefix was not rebuilt or deleted.

Deterministic validation includes 62 Python tests; native tracing and install-name
checks pass SDK-on 19/19 and SDK-off 18/18. The SDK is restored on afterward.
The native producer round-trip preserves 52 events including two synthetic
24 MHz QPC endpoints and passes pinned Perfetto v58.2 Trace Processor queries.
All four canonical directories build, and generated bridge headers match
byte-for-byte across native, x86_64 unix, x64 PE and x86 PE. The PE compile-command
audit finds no SDK sources/includes. Existing concurrent lifecycle tests and
model/code binding checks pass; this repair adds no session storage owner or
lifetime state and does not claim a new full TLC run.

Negative fixtures cover malformed wire bounds/version/identity, arithmetic
and calibration-ID overflow, underreported uncertainty, non-midpoint values,
wrong source ticks/API/unit, frequency/session mismatch, and 1000 ns policy
failure. High-error points remain valid partial observations but cannot qualify
complete attribution. Fractional QPC frequency and large-offset serialization
fixtures prevent false rejection from rounded affine coefficients.

Local ignored evidence root: `experiments/output/tracing-pe-clock-fix/`.
`wine-evidence.json` links every attempt and raw endpoint; `matched-results.json`
records all 12 controls; `provenance-verification.json` verifies every matched
manifest artifact digest and the five binaries actually staged for each run.
`abi-lockstep.json`, `python-final-tests.log`, `sdk-on-final-tests.log`,
`sdk-off-tests.log`, and `dxmt9-tracing-runtime-3451/{normalized,consumer}.json`
retain the remaining checks. The six matched trace IDs are:

- x64: `c3bbfda604094d1da6693438c5b7eeec`,
  `659fbde105824fe18c7a05a2a3752603`, `411ddc79c7044bd6b07f5c448d6186b8`.
- x86: `004ee0b44f7b4cd59d7fbfbab566f594`,
  `df5cfe9bd0774cfd81040e7a4c76aff4`, `8f47350c1e174789b0e56ab49c39a8fe`.

The reproducible catalogue command (use the x86 wrapper for WoW64) is:

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py run conf-d3d9-triangle \
  --wine-id sikarugir-cx-24.0.7 \
  --wine-root "$PWD/experiments/wine/sikarugir-cx-24.0.7" \
  --prefix "$PWD/experiments/prefixs/tracing-pe-clock-v2" \
  --binary "$PWD/experiments/output/tracing-acceptance/wine-triangle-x64.exe" \
  --timeout 60 --trace-profile timeline --trace-include-pe \
  --trace-duration-ms 500 --output-suffix pe-clock-v2-review
```

The repair artifact inventory contains 212 files. Its SHA-256 is
`5175f1f974f333ebf32a0f6d9d5960fb04c9dcfb0232a988c1f6195cba79f7ca`
(`experiments/output/tracing-pe-clock-fix/artifact-inventory.json`).
