#!/usr/bin/env python3
"""Compare two experiment screenshots and emit a small correctness-gate report."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image


@dataclass
class ImageComparison:
    area: str
    width: int
    height: int
    compared_pixels: int
    before_active_pixels: int
    after_active_pixels: int
    before_active_pct: float
    after_active_pct: float
    changed_pixels: int
    changed_pct: float
    max_delta: int
    mean_abs_delta: float
    rms_delta: float
    ssim: float


POLICY_PRESETS: dict[str, dict[str, float | int]] = {
    "exact": {
        "max_changed_pct": 0.0,
        "min_ssim": 1.0,
    },
    "lsb1": {
        "max_changed_pct": 0.1,
        "min_ssim": 0.999999,
        "max_delta": 1,
        "max_mean_abs_delta": 0.001,
        "max_rms_delta": 0.02,
    },
}


def load_rgb(path: Path) -> Image.Image:
    return Image.open(path).convert("RGB")


def crop_bottom(image: Image.Image, pixels: int) -> Image.Image:
    if pixels <= 0:
        return image
    if pixels >= image.height:
        raise ValueError(
            f"cannot crop {pixels} bottom pixels from {image.width}x{image.height} image"
        )
    return image.crop((0, 0, image.width, image.height - pixels))


def compute_ssim_rgb(before: np.ndarray, after: np.ndarray) -> float:
    before_luma = (
        0.2126 * before[:, :, 0]
        + 0.7152 * before[:, :, 1]
        + 0.0722 * before[:, :, 2]
    ).astype(np.float64)
    after_luma = (
        0.2126 * after[:, :, 0]
        + 0.7152 * after[:, :, 1]
        + 0.0722 * after[:, :, 2]
    ).astype(np.float64)
    mu_x = float(np.mean(before_luma))
    mu_y = float(np.mean(after_luma))
    sigma_x = float(np.var(before_luma))
    sigma_y = float(np.var(after_luma))
    sigma_xy = float(np.mean((before_luma - mu_x) * (after_luma - mu_y)))
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    numerator = (2.0 * mu_x * mu_y + c1) * (2.0 * sigma_xy + c2)
    denominator = (mu_x * mu_x + mu_y * mu_y + c1) * (sigma_x + sigma_y + c2)
    if denominator == 0.0:
        return 1.0
    return float(numerator / denominator)


def compare_arrays(
    area: str,
    before: np.ndarray,
    after: np.ndarray,
    active_threshold: int,
) -> ImageComparison:
    if before.shape != after.shape:
        raise ValueError(f"image size mismatch: {before.shape} vs {after.shape}")
    delta = np.abs(after.astype(np.int16) - before.astype(np.int16))
    delta_float = delta.astype(np.float64)
    per_pixel_delta = np.max(delta, axis=2)
    before_active = np.max(before, axis=2) > active_threshold
    after_active = np.max(after, axis=2) > active_threshold
    changed_pixels = int(np.count_nonzero(per_pixel_delta))
    compared_pixels = int(per_pixel_delta.size)
    before_active_pixels = int(np.count_nonzero(before_active))
    after_active_pixels = int(np.count_nonzero(after_active))
    return ImageComparison(
        area=area,
        width=int(before.shape[1]),
        height=int(before.shape[0]),
        compared_pixels=compared_pixels,
        before_active_pixels=before_active_pixels,
        after_active_pixels=after_active_pixels,
        before_active_pct=(
            before_active_pixels / compared_pixels * 100.0
            if compared_pixels else 0.0
        ),
        after_active_pct=(
            after_active_pixels / compared_pixels * 100.0
            if compared_pixels else 0.0
        ),
        changed_pixels=changed_pixels,
        changed_pct=(changed_pixels / compared_pixels) * 100.0 if compared_pixels else 0.0,
        max_delta=int(np.max(delta)) if delta.size else 0,
        mean_abs_delta=float(np.mean(delta_float)) if delta.size else 0.0,
        rms_delta=float(math.sqrt(float(np.mean(np.square(delta_float))))) if delta.size else 0.0,
        ssim=compute_ssim_rgb(before, after),
    )


def compare_images(
    before_path: Path,
    after_path: Path,
    crop_bottom_pixels: int = 0,
    active_threshold: int = 0,
) -> list[ImageComparison]:
    before_image = load_rgb(before_path)
    after_image = load_rgb(after_path)
    if before_image.size != after_image.size:
        raise ValueError(
            f"image size mismatch: {before_path} is {before_image.size}, "
            f"{after_path} is {after_image.size}"
        )

    comparisons = [
        compare_arrays(
            "full",
            np.asarray(before_image, dtype=np.uint8),
            np.asarray(after_image, dtype=np.uint8),
            active_threshold,
        )
    ]
    if crop_bottom_pixels > 0:
        comparisons.append(
            compare_arrays(
                f"crop-bottom-{crop_bottom_pixels}",
                np.asarray(crop_bottom(before_image, crop_bottom_pixels), dtype=np.uint8),
                np.asarray(crop_bottom(after_image, crop_bottom_pixels), dtype=np.uint8),
                active_threshold,
            )
        )
    return comparisons


def write_diff_image(before_path: Path, after_path: Path, output_path: Path) -> None:
    before = load_rgb(before_path)
    after = load_rgb(after_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if before.size != after.size:
        canvas = Image.new(
            "RGB",
            (before.width + after.width, max(before.height, after.height)),
        )
        canvas.paste(before, (0, 0))
        canvas.paste(after, (before.width, 0))
        canvas.save(output_path)
        return

    before_arr = np.asarray(before, dtype=np.int16)
    after_arr = np.asarray(after, dtype=np.int16)
    delta = np.abs(after_arr - before_arr).astype(np.uint8)
    heat = np.zeros_like(delta)
    heat[:, :, 0] = np.max(delta, axis=2)
    heat[:, :, 1] = np.max(delta, axis=2) // 4
    heat[:, :, 2] = np.max(delta, axis=2) // 4
    blended = np.clip(0.6 * after_arr + 0.8 * heat, 0, 255).astype(np.uint8)
    Image.fromarray(blended, mode="RGB").save(output_path)


def fmt_float(value: float) -> str:
    return f"{value:.6f}"


def requirement_failures(
    comparisons: list[ImageComparison],
    max_changed_pct: float,
    min_ssim: float,
    min_before_active_pct: float,
    min_after_active_pct: float,
    max_delta: int | None,
    max_mean_abs_delta: float | None,
    max_rms_delta: float | None,
) -> list[str]:
    failures: list[str] = []
    for item in comparisons:
        if item.changed_pct > max_changed_pct:
            failures.append(
                f"{item.area}: changed_pct {item.changed_pct:.6f} "
                f"> {max_changed_pct:.6f}"
            )
        if item.ssim < min_ssim:
            failures.append(
                f"{item.area}: ssim {item.ssim:.6f} < {min_ssim:.6f}"
            )
        if item.before_active_pct < min_before_active_pct:
            failures.append(
                f"{item.area}: before_active_pct {item.before_active_pct:.6f} "
                f"< {min_before_active_pct:.6f}"
            )
        if item.after_active_pct < min_after_active_pct:
            failures.append(
                f"{item.area}: after_active_pct {item.after_active_pct:.6f} "
                f"< {min_after_active_pct:.6f}"
            )
        if max_delta is not None and item.max_delta > max_delta:
            failures.append(
                f"{item.area}: max_delta {item.max_delta} > {max_delta}"
            )
        if (
            max_mean_abs_delta is not None
            and item.mean_abs_delta > max_mean_abs_delta
        ):
            failures.append(
                f"{item.area}: mean_abs_delta {item.mean_abs_delta:.6f} "
                f"> {max_mean_abs_delta:.6f}"
            )
        if max_rms_delta is not None and item.rms_delta > max_rms_delta:
            failures.append(
                f"{item.area}: rms_delta {item.rms_delta:.6f} "
                f"> {max_rms_delta:.6f}"
            )
    return failures


def policy_value(
    policy: str | None,
    key: str,
    explicit: float | int | None,
    fallback: float | int,
) -> float | int:
    if explicit is not None:
        return explicit
    if policy is not None and key in POLICY_PRESETS[policy]:
        return POLICY_PRESETS[policy][key]
    return fallback


def write_summary_csv(path: Path, comparisons: list[ImageComparison]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "area",
        "width",
        "height",
        "compared_pixels",
        "changed_pixels",
        "changed_pct",
        "before_active_pixels",
        "before_active_pct",
        "after_active_pixels",
        "after_active_pct",
        "max_delta",
        "mean_abs_delta",
        "rms_delta",
        "ssim",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for item in comparisons:
            writer.writerow({
                "area": item.area,
                "width": item.width,
                "height": item.height,
                "compared_pixels": item.compared_pixels,
                "changed_pixels": item.changed_pixels,
                "changed_pct": fmt_float(item.changed_pct),
                "before_active_pixels": item.before_active_pixels,
                "before_active_pct": fmt_float(item.before_active_pct),
                "after_active_pixels": item.after_active_pixels,
                "after_active_pct": fmt_float(item.after_active_pct),
                "max_delta": item.max_delta,
                "mean_abs_delta": fmt_float(item.mean_abs_delta),
                "rms_delta": fmt_float(item.rms_delta),
                "ssim": fmt_float(item.ssim),
            })


def write_report(
    path: Path,
    before_path: Path,
    after_path: Path,
    label_before: str,
    label_after: str,
    comparisons: list[ImageComparison],
    failures: list[str],
    gates_requested: bool,
    policy: str | None,
    thresholds: dict[str, float | int | None],
    diff_output: Path | None,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = [
        "# Experiment Image Comparison",
        "",
        f"- Before: `{label_before}` (`{before_path}`)",
        f"- After: `{label_after}` (`{after_path}`)",
    ]
    if diff_output is not None:
        lines.append(f"- Diff image: `{diff_output}`")
    if policy is not None:
        lines.append(f"- Policy: `{policy}`")
    if gates_requested:
        lines.append(
            "- Gates: "
            f"`max_changed_pct={thresholds['max_changed_pct']}`, "
            f"`min_ssim={thresholds['min_ssim']}`, "
            f"`max_delta={thresholds['max_delta']}`, "
            f"`max_mean_abs_delta={thresholds['max_mean_abs_delta']}`, "
            f"`max_rms_delta={thresholds['max_rms_delta']}`, "
            f"`min_before_active_pct={thresholds['min_before_active_pct']}`, "
            f"`min_after_active_pct={thresholds['min_after_active_pct']}`"
        )
    lines.extend([
        "",
        "## Metrics",
        "",
        "| Area | Size | Changed pixels | Changed % | Before active % | After active % | Max delta | Mean abs delta | RMS delta | SSIM |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for item in comparisons:
        lines.append(
            f"| `{item.area}` | `{item.width}x{item.height}` | "
            f"`{item.changed_pixels:,}/{item.compared_pixels:,}` | "
            f"`{item.changed_pct:.6f}%` | "
            f"`{item.before_active_pct:.6f}%` | "
            f"`{item.after_active_pct:.6f}%` | `{item.max_delta}` | "
            f"`{item.mean_abs_delta:.6f}` | `{item.rms_delta:.6f}` | "
            f"`{item.ssim:.6f}` |"
        )
    lines.append("")
    lines.append("## Requirement Status")
    lines.append("")
    if not gates_requested:
        lines.append("- Not requested: no image similarity thresholds were enforced.")
    elif failures:
        lines.append(f"- Failed: `{len(failures)}` image gate(s) did not pass.")
        lines.append("")
        lines.append("## Requirement Failures")
        lines.append("")
        for failure in failures:
            lines.append(f"- {failure}")
    else:
        lines.append("- Passed: all requested image gates were satisfied.")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--label-before", default="before")
    parser.add_argument("--label-after", default="after")
    parser.add_argument("--crop-bottom", type=int, default=0)
    parser.add_argument(
        "--active-threshold",
        type=int,
        default=0,
        help="RGB max value above which a pixel counts as active/non-black",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--summary-output", type=Path)
    parser.add_argument("--diff-output", type=Path)
    parser.add_argument(
        "--policy",
        choices=sorted(POLICY_PRESETS),
        help=(
            "named similarity policy preset. 'exact' requires no changed pixels; "
            "'lsb1' allows bounded one-LSB blend-order differences"
        ),
    )
    parser.add_argument("--require-similar", action="store_true")
    parser.add_argument("--max-changed-pct", type=float)
    parser.add_argument("--min-ssim", type=float)
    parser.add_argument("--min-before-active-pct", type=float)
    parser.add_argument("--min-after-active-pct", type=float)
    parser.add_argument(
        "--max-delta",
        type=int,
        help="maximum allowed per-channel absolute delta",
    )
    parser.add_argument(
        "--max-mean-abs-delta",
        type=float,
        help="maximum allowed mean absolute per-channel delta",
    )
    parser.add_argument(
        "--max-rms-delta",
        type=float,
        help="maximum allowed RMS per-channel delta",
    )
    args = parser.parse_args()

    if not args.before.is_file():
        parser.error(f"missing before image: {args.before}")
    if not args.after.is_file():
        parser.error(f"missing after image: {args.after}")

    try:
        comparisons = compare_images(
            args.before,
            args.after,
            args.crop_bottom,
            args.active_threshold,
        )
    except ValueError as exc:
        parser.error(str(exc))

    if args.diff_output:
        write_diff_image(args.before, args.after, args.diff_output)

    max_changed_pct = float(policy_value(
        args.policy,
        "max_changed_pct",
        args.max_changed_pct,
        0.0,
    ))
    min_ssim = float(policy_value(args.policy, "min_ssim", args.min_ssim, 1.0))
    min_before_active_pct = float(policy_value(
        args.policy,
        "min_before_active_pct",
        args.min_before_active_pct,
        0.0,
    ))
    min_after_active_pct = float(policy_value(
        args.policy,
        "min_after_active_pct",
        args.min_after_active_pct,
        0.0,
    ))
    max_delta = policy_value(args.policy, "max_delta", args.max_delta, None)
    max_mean_abs_delta = policy_value(
        args.policy,
        "max_mean_abs_delta",
        args.max_mean_abs_delta,
        None,
    )
    max_rms_delta = policy_value(
        args.policy,
        "max_rms_delta",
        args.max_rms_delta,
        None,
    )
    gates_requested = (
        args.policy is not None
        or args.require_similar
        or min_before_active_pct > 0.0
        or min_after_active_pct > 0.0
        or max_delta is not None
        or max_mean_abs_delta is not None
        or max_rms_delta is not None
    )
    failures = (
        requirement_failures(
            comparisons,
            max_changed_pct,
            min_ssim,
            min_before_active_pct,
            min_after_active_pct,
            None if max_delta is None else int(max_delta),
            None if max_mean_abs_delta is None else float(max_mean_abs_delta),
            None if max_rms_delta is None else float(max_rms_delta),
        )
        if gates_requested
        else []
    )
    write_report(
        args.output,
        args.before,
        args.after,
        args.label_before,
        args.label_after,
        comparisons,
        failures,
        gates_requested,
        args.policy,
        {
            "max_changed_pct": max_changed_pct,
            "min_ssim": min_ssim,
            "max_delta": max_delta,
            "max_mean_abs_delta": max_mean_abs_delta,
            "max_rms_delta": max_rms_delta,
            "min_before_active_pct": min_before_active_pct,
            "min_after_active_pct": min_after_active_pct,
        },
        args.diff_output,
    )
    if args.summary_output:
        write_summary_csv(args.summary_output, comparisons)

    if failures:
        for failure in failures:
            print(f"requirement failed: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
