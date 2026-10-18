# Wine tracing integration fixture

`wine_triangle.cpp` wraps the repository's self-checking triangle fixture and
keeps its process alive for 1.5 seconds after the fixture returns. The app
window can close before that dwell ends. The runner still needs the process to
survive long enough to close its trace window, wait for admitted events, and
publish the final receipt. This is capture-lifecycle coverage, not a workload
throughput or observer-overhead benchmark.

Build the x64 wrapper with the same Windows import libraries as the catalogue
triangle fixture. The normal build prerequisites, including `llvm-mingw`, are
in [`docs/build.md`](../../../docs/build.md).

```sh
mkdir -p experiments/output/tracing-acceptance
x86_64-w64-mingw32-clang++ \
  -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter \
  -Iexperiments/apps/common \
  -o experiments/output/tracing-acceptance/wine-triangle-x64.exe \
  tests/integration/tracing/wine_triangle.cpp \
  -ld3d9 -ld3dx9_43 -ldxguid -lgdi32 -luser32 -lshell32 -lole32 -luuid
```

Build the dxmt9 unix provider with `-Ddxmt9_perfetto=true` and stage matching
x64 PE binaries as described in `docs/build.md`. Then capture a 500 ms timeline
using the manifest-selected Sikarugir runtime:

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py run conf-d3d9-triangle \
  --binary experiments/output/tracing-acceptance/wine-triangle-x64.exe \
  --wine-id sikarugir-cx-24.0.7 --trace-profile timeline \
  --trace-duration-ms 500 --timeout 60 --output-suffix tracing-window-x64
```

The 500 ms window fits inside the wrapper's post-work dwell. Longer windows
require changing and rebuilding the wrapper; the ordinary short-lived catalogue
binary has no such dwell and can exit before asynchronous trace finalization.
The qualified x64 result is retained under
`experiments/output/conf-d3d9-triangle-tracing-window-x64-trace-df36a79cffdd404da85d30de1f06b912/`.
It qualifies CPU engine tracing and exact native Trace Processor comparison.
Final x64 GPU runs also qualify both GPU timing modes with the same wrapper and
runtime:

| Capture | Artifact directory | Qualified evidence |
|---|---|---|
| Encoder stage | `experiments/output/conf-d3d9-triangle-tracing-stage-final-x64-trace-d962bb867fed4406849ff7a68068e186/` | 43 total events, including 6 GPU events; zero loss; four complete pinned-TP flows. |
| Command buffer + detail | `experiments/output/conf-d3d9-triangle-tracing-cb-detail-final-x64-trace-dadab305ece94d46b89c4300c3271c64/` | 42 total events, including 4 GPU events; zero loss; one observed detail event; four complete pinned-TP flows. |

These runs qualify this x64 Wine/runtime/workload lane; they do not establish
performance overhead. Separate x86 and x64 PE runs each emitted 8 PE events
among 45 total with zero loss, but strict trace qualification rejected both
because QPC calibration was missing/uncovered and uncertainty exceeded the
frozen 1000 ns policy. The observed records remain evidence of collection, not
qualified guest clock attribution. Three matched baseline/timeline pairs were also run for each
wrapper architecture (12 runs total); all passed and produced identical
`actual.bmp` SHA-256
`e1a0b87d3e0ad47602a8dbc2983e5105aeeb9ccca3501ad1974c678545b97567`. The
matched results use screenshot and catalogue-launch timing, not throughput, and
support no speedup or negligible-overhead claim. Their summary artifacts are
`experiments/output/tracing-acceptance/matched-wine-evidence.json` and
`matched-wine-results.json`. Final native tests pass SDK-off (17/17) and SDK-on
(20/20); x86/x64 PE and unix builds complete. Live Linux and macOS external
probe adapters remain unavailable; portable adapter fixtures do not replace
those platform runs.


## PE endpoint repair

The ABI v2 [PE point-bracket protocol](../../../specs/experiments/harness/tracing/pe-clock.md)
now qualifies both x86 and x64. Add `--trace-include-pe` to the timeline request.
Three baseline/timeline pairs per architecture pass on the isolated
`experiments/prefixs/tracing-pe-clock-v2` prefix, with the same image hash above.
All six traces retain six PE endpoints, zero loss, exact native TP agreement,
and unchanged 1000 ns clock policy: x64 247–810 ns, x86 476–956 ns. The earlier
failed PE traces remain historical evidence. New summaries and test logs live
under `experiments/output/tracing-pe-clock-fix/`; this smoke does not establish
throughput or negligible probe cost.
