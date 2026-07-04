#!/usr/bin/env python3
"""Judge a paired 3DMark05 GT1 P4 deferred-boundary scout.

Design: docs/superpowers/specs/2026-07-04-gt1-p4-deferred-boundary-design.md
Gates (candidate vs baseline):
  fps        present_encoded above baseline beyond --noise-pct
  p4         completion_wait_without_enqueue_ms/present <= 50% of baseline
             AND completion_wait_with_enqueue_ms/present > baseline
  locality   command_buffers, sub_command_buffers, render_pass_begin,
             render_pass_tile_preservation_bytes per present all
             <= baseline * (1 + --locality-slack-pct/100)
  correct    status == pass (timeout-finalized ok), gpu_command_buffer_errors == 0,
             completion_dequeue_status_error == 0, mean_luma >= 10 (both runs)
  semantics  candidate present_boundary_applied > 0 and
             present_boundary_deferred > 0
Exit: 0 WIN, 1 LOSE, 2 REPEAT (FPS inside noise band, all other gates pass).
"""

import argparse
import json
import sys
from pathlib import Path

LOCALITY_KEYS = [
    "command_buffers",
    "sub_command_buffers",
    "render_pass_begin",
    "render_pass_tile_preservation_bytes",
]


def load_run(path):
    result = json.loads((Path(path) / "result.json").read_text())
    counters = result.get("dxmt9_perf_counters") or {}
    presents = float(counters.get("present_encoded") or 0.0)
    image = result.get("image_metrics") or {}
    return result, counters, presents, image


def per_present(counters, key, presents):
    value = counters.get(key)
    if value is None or presents <= 0:
        return None
    return float(value) / presents


def run_correct(result, counters, image, label, rows):
    ok = True
    status = result.get("status")
    errors = float(counters.get("gpu_command_buffer_errors") or 0.0)
    dequeue_err = float(counters.get("completion_dequeue_status_error") or 0.0)
    luma = image.get("mean_luma")
    luma_ok = luma is not None and float(luma) >= 10.0
    for name, cond in [
        (f"{label} status=pass", status == "pass"),
        (f"{label} gpu_command_buffer_errors=0", errors == 0.0),
        (f"{label} completion_dequeue_status_error=0", dequeue_err == 0.0),
        (f"{label} mean_luma>=10 (non-black)", luma_ok),
    ]:
        rows.append((name, "PASS" if cond else "FAIL", ""))
        ok = ok and cond
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--noise-pct", type=float, default=5.0)
    parser.add_argument("--locality-slack-pct", type=float, default=2.0)
    args = parser.parse_args()

    b_res, b_ctr, b_presents, b_img = load_run(args.baseline)
    c_res, c_ctr, c_presents, c_img = load_run(args.candidate)
    rows = []

    correct = run_correct(b_res, b_ctr, b_img, "baseline", rows)
    correct = run_correct(c_res, c_ctr, c_img, "candidate", rows) and correct

    fps_delta_pct = (
        (c_presents - b_presents) / b_presents * 100.0 if b_presents else -100.0
    )
    fps_win = fps_delta_pct > args.noise_pct
    fps_lose = fps_delta_pct < -args.noise_pct
    rows.append((
        "fps presents",
        f"{b_presents:.0f} -> {c_presents:.0f}",
        f"{fps_delta_pct:+.2f}% (noise ±{args.noise_pct}%)",
    ))

    b_without = per_present(b_ctr, "completion_wait_without_enqueue_ms", b_presents)
    c_without = per_present(c_ctr, "completion_wait_without_enqueue_ms", c_presents)
    b_with = per_present(b_ctr, "completion_wait_with_enqueue_ms", b_presents)
    c_with = per_present(c_ctr, "completion_wait_with_enqueue_ms", c_presents)
    p4 = (
        None not in (b_without, c_without, b_with, c_with)
        and c_without <= 0.5 * b_without
        and c_with > b_with
    )
    rows.append((
        "p4 without-enqueue ms/present (<=50% of baseline)",
        f"{b_without:.3f} -> {c_without:.3f}"
        if None not in (b_without, c_without) else "missing",
        "PASS" if p4 else "FAIL",
    ))
    rows.append((
        "p4 with-enqueue ms/present (must rise)",
        f"{b_with:.3f} -> {c_with:.3f}"
        if None not in (b_with, c_with) else "missing",
        "",
    ))

    locality = True
    slack = 1.0 + args.locality_slack_pct / 100.0
    for key in LOCALITY_KEYS:
        b_v = per_present(b_ctr, key, b_presents)
        c_v = per_present(c_ctr, key, c_presents)
        ok = b_v is not None and c_v is not None and c_v <= b_v * slack
        locality = locality and ok
        shown = (
            f"{b_v:.3f} -> {c_v:.3f}" if None not in (b_v, c_v) else "missing"
        )
        rows.append((f"locality {key}/present", shown, "PASS" if ok else "FAIL"))

    applied = float(c_ctr.get("present_boundary_applied") or 0.0)
    deferred = float(c_ctr.get("present_boundary_deferred") or 0.0)
    deferred_waits = float(c_ctr.get("present_boundary_deferred_waits") or 0.0)
    semantics = applied > 0.0 and deferred > 0.0
    rows.append((
        "semantics deferred gate engaged",
        f"applied={applied:.0f} deferred={deferred:.0f}"
        f" deferred_waits={deferred_waits:.0f}",
        "PASS" if semantics else "FAIL",
    ))

    width = max(len(r[0]) for r in rows)
    for name, value, note in rows:
        print(f"{name:<{width}}  {value}  {note}")

    hard_gates = correct and p4 and locality and semantics
    if not hard_gates:
        print("VERDICT: LOSE (non-FPS gate failed)")
        return 1
    if fps_win:
        print("VERDICT: WIN")
        return 0
    if fps_lose:
        print("VERDICT: LOSE (FPS regressed)")
        return 1
    print("VERDICT: REPEAT (FPS inside noise band)")
    return 2


if __name__ == "__main__":
    sys.exit(main())
