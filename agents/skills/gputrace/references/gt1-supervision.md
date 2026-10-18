# 3DMark probe supervision

Read this before starting a 3DMark probe. Commands run from the repository root.

Before starting a new 3DMark05 `.gputrace`, keep at least 2GiB free on the
repository volume. The standard wrapper enforces this with
`DXMT_3DMARK05_MIN_TRACE_FREE_MB` / `--min-free-mb`; do not bypass it unless
the run deliberately disables gputrace or the output volume has already been
cleared. Use `--dry-run` first; if free space is below the guard, dry-run and
guard failures print `space usage hints` plus `large trace/output files` to
choose cleanup targets. Do not delete raw trace artifacts automatically.

3DMark05 may hang on the final frame after the useful perf/capture data has
already been emitted. Do not wait for natural process exit during perf work.
Every 3DMark05 perf or `.gputrace` run must go through a positive runner
timeout so the run can timeout-finalize `result.json`, perf logs, and trace
paths consistently. The standard wrapper defaults to `120s` for no-gputrace
scouts and `420s` for `.gputrace`/Xcode replay candidates, and passes that as
an explicit `run_experiment.py --timeout`. It also wraps the entire
`caffeinate run_experiment.py ...` command in a top-level watchdog at
`timeout + effective capture delay + DXMT_3DMARK05_PROBE_TIMEOUT_SLACK`
(default capture delay `70s`, default slack `45s`) so a detached
Wine/final-frame hang still gets terminated after `run_experiment.py` has had
time to timeout-finalize `result.json`. Override wrapper `--timeout` only when
the experiment needs a documented budget. A
timeout-finalized run with the
expected artifacts is acceptable input for finalizer comparison; do not use the
Wine process lifetime or a manual kill as a performance metric. If a run had
to be killed manually because it was stuck at the final frame, treat that as a
timeout-policy failure and verify the wrapper watchdog rather than repeating an
unsupervised launch. For GT2 (`-gt2`) A/B runs, never use `result.json`
presents-at-kill as the FPS metric: GT2 is a fixed ~68s animation timeline
that finishes in-window and then often hangs post-scene until the timeout
kill, and SIGKILL loses the final counter flush, so `result.json` reports the
last periodic emission (~8% presents undercount observed, H231). Enable
`DXMT9_PERF_FRAME_SAMPLING=1` and compute scene-fps from the per-frame
`wall_ms` samples in the run log instead — and anchor the extraction to the
`[dxmt9-perf-frame ...]` line prefix. A loose `wall_ms=` regex also matches the
cumulative `parallel_pass_worker_wall_ms` counter in periodic `[dxmt9-perf]`
emissions; in a parallel-mode run those cumulative samples injected 11.5 fake
seconds into ~61 real seconds and fabricated a parallel-only harmonic-fps
collapse, while identity runs were immune because their zero values were
filtered — the artifact reads exactly like a mode-specific hitch tail
(retracted 2026-08-18, see `specs/backend/encode-scheduling/gap.md`); total process elapsed and
timed-out-vs-natural-exit only distinguish whether the post-scene hang
occurred, not render speed. If the desktop may be locked when a probe is scheduled,
pass `run_3dmark05_perf_probe.sh --wait-unlocked-sec N`; the wrapper polls
before Wine launch and does not create probe artifacts when the wait expires.
For no-gputrace A/B runs whose FPS or present count is used as evidence, add
`--keep-frontmost` when there is any risk that 3DMark05 will lose focus; this
keeps the visible `3DMark05.exe` process frontmost during the supervised run and
prevents app focus/scene-progress drift from masquerading as a renderer change.
For routine
3DMark05 smoke/image runs, use the catalogue runner's default
`run_timeout_sec=120` / `allow_timeout=true` / `require_positive_timeout=true`
or pass an explicit positive `run_experiment.py --timeout N`; `--timeout 0` is
rejected for this app. Do not run
`experiments/launchers/app-d3d9-3dmark05.sh` or the
`DXMT_3DMARK05_DIRECT=1` launcher path unsupervised from a shell. If the
launcher is started directly and no `run_experiment.py` supervision is present,
it applies a fallback positive timeout (`DXMT_3DMARK05_LAUNCHER_TIMEOUT`,
default `120s`) and kills the child process group; set
`DXMT_3DMARK05_ALLOW_UNSUPERVISED=1` only when another documented supervisor is
active. For direct verify-prefix runs, prefer
`scripts/run_apps/run_app-d3d9-3dmark05-verify_direct.sh`; it supervises the
launcher with `DXMT_3DMARK05_DIRECT_TIMEOUT=120` by default and kills the
process group on timeout. Use `DXMT_3DMARK05_DIRECT_DRY_RUN=1` first when
checking the resolved timeout/command, and record any non-default timeout
budget in the run notes. The 3DMark05 direct launcher also traps `TERM`/`INT`
and runs `wineserver -k` for the app prefix on exit by default
(`DXMT_3DMARK05_KILL_SERVER_ON_EXIT=1`), so a timeout must not leave a
detached final-frame 3DMark05 process running.

After Xcode encoder counters are exported, run
`scripts/tools/finalize_3dmark05_perf_probe.sh`. Besides the Xcode/dxmt joined
summary, the finalizer now emits
`analysis/frame<N>-indexed-state-class-xcode-proxy.{md,csv}` whenever indexed
probe draw telemetry exists. Read this proxy queue before opening another
`.gputrace`: `gate_status=covered-production-path` means the residual class is
already covered by the accepted opaque proof family, `explicit-tolerance-only`
means screen-blend needs an exact/`lsb1` semantic policy, and
`blocked-final-color-oracle` means the next useful work is a final-color/
final-writer predicate or a non-reorder backend mechanism.
