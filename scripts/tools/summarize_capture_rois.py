#!/usr/bin/env python3
"""Summarize screenshot/capture images into ROI warm/white/bright tables."""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from PIL import Image, ImageDraw


IMAGE_SUFFIXES = {".bmp", ".jpg", ".jpeg", ".png", ".tif", ".tiff"}


@dataclass(frozen=True)
class Roi:
    name: str
    left: int
    top: int
    right: int
    bottom: int


@dataclass(frozen=True)
class Component:
    component_id: int
    search_roi: str
    left: int
    top: int
    right: int
    bottom: int
    bbox_area: int
    bbox_fill_pct: float
    bbox_aspect_ratio: float
    area: int
    warm_pixels: int
    white_pixels: int
    bright_pixels: int
    max_rgb: tuple[int, int, int]
    hot: tuple[int, int, int, int, int]
    warm_hot: tuple[int, int, int, int, int]


def parse_roi(value: str) -> Roi:
    parts = value.split(":")
    coords = parts[0].split(",")
    if len(coords) != 4 or len(parts) > 2:
        raise argparse.ArgumentTypeError("ROI must be L,T,R,B or L,T,R,B:name")
    try:
        left, top, right, bottom = (int(part) for part in coords)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("ROI coordinates must be integers") from exc
    if left < 0 or top < 0 or right <= left or bottom <= top:
        raise argparse.ArgumentTypeError("ROI must have non-negative L/T and R>L, B>T")
    name = parts[1] if len(parts) == 2 and parts[1] else f"{left},{top},{right},{bottom}"
    return Roi(name=name, left=left, top=top, right=right, bottom=bottom)


def iter_images(
    paths: Sequence[Path],
    include_name_regex: str | None = None,
    exclude_name_regex: str | None = None,
) -> list[Path]:
    include_re = re.compile(include_name_regex) if include_name_regex else None
    exclude_re = re.compile(exclude_name_regex) if exclude_name_regex else None

    def accepts(path: Path) -> bool:
        if path.suffix.lower() not in IMAGE_SUFFIXES:
            return False
        if include_re is not None and include_re.search(path.name) is None:
            return False
        if exclude_re is not None and exclude_re.search(path.name) is not None:
            return False
        return True

    result: list[Path] = []
    for path in paths:
        if path.is_dir():
            result.extend(
                candidate
                for candidate in sorted(path.iterdir())
                if candidate.is_file() and accepts(candidate)
            )
        elif path.is_file() and accepts(path):
            result.append(path)
        elif path.is_file():
            continue
        else:
            raise FileNotFoundError(f"not an image path or directory: {path}")
    return sorted(dict.fromkeys(result))


def clamp_roi(roi: Roi, width: int, height: int) -> Roi:
    return Roi(
        name=roi.name,
        left=min(roi.left, width),
        top=min(roi.top, height),
        right=min(roi.right, width),
        bottom=min(roi.bottom, height),
    )


def extract_frame(path: Path) -> str:
    match = re.search(r"frame0*([0-9]+)", path.stem, flags=re.IGNORECASE)
    if match:
        return match.group(1)
    match = re.search(r"([0-9]+)", path.stem)
    return match.group(1) if match else ""


def summarize_region(
    image: Image.Image,
    roi: Roi,
    bright_threshold: int,
    white_threshold: int,
    warm_red_threshold: int,
    warm_green_threshold: int,
    warm_blue_margin: int,
) -> dict[str, object]:
    clipped = clamp_roi(roi, image.width, image.height)
    width = max(0, clipped.right - clipped.left)
    height = max(0, clipped.bottom - clipped.top)
    pixels = width * height
    sums = [0, 0, 0]
    max_rgb = [0, 0, 0]
    bright = 0
    white = 0
    warm = 0
    hot = [0, 0, 0, 0, 0, 0]
    warm_hot = [0, 0, 0, 0, 0, 0]
    source = image.load()
    for y in range(clipped.top, clipped.bottom):
        for x in range(clipped.left, clipped.right):
            r, g, b = source[x, y]
            sums[0] += r
            sums[1] += g
            sums[2] += b
            max_rgb[0] = max(max_rgb[0], r)
            max_rgb[1] = max(max_rgb[1], g)
            max_rgb[2] = max(max_rgb[2], b)
            channel_max = max(r, g, b)
            if channel_max > bright_threshold:
                bright += 1
            if r > white_threshold and g > white_threshold and b > white_threshold:
                white += 1
            if (
                r >= warm_red_threshold
                and g >= warm_green_threshold
                and b <= r + warm_blue_margin
            ):
                warm += 1
                warm_score = r + g + b
                if warm_score > warm_hot[5]:
                    warm_hot = [x, y, r, g, b, warm_score]
            if channel_max > hot[5]:
                hot = [x, y, r, g, b, channel_max]
    avg = [0.0, 0.0, 0.0]
    if pixels:
        avg = [value / pixels for value in sums]
    return {
        "roi": clipped,
        "pixels": pixels,
        "avg": avg,
        "max": max_rgb,
        "bright": bright,
        "white": white,
        "warm": warm,
        "hot": hot,
        "warm_hot": warm_hot,
    }


def summarize_image(
    image_path: Path,
    rois: Sequence[Roi],
    bright_threshold: int,
    white_threshold: int,
    warm_red_threshold: int,
    warm_green_threshold: int,
    warm_blue_margin: int,
) -> list[dict[str, object]]:
    image = Image.open(image_path).convert("RGB")
    rows: list[dict[str, object]] = []
    frame = extract_frame(image_path)
    for roi in rois:
        region = summarize_region(
            image,
            roi,
            bright_threshold,
            white_threshold,
            warm_red_threshold,
            warm_green_threshold,
            warm_blue_margin,
        )
        clipped: Roi = region["roi"]  # type: ignore[assignment]
        avg = region["avg"]  # type: ignore[assignment]
        max_rgb = region["max"]  # type: ignore[assignment]
        hot = region["hot"]  # type: ignore[assignment]
        warm_hot = region["warm_hot"]  # type: ignore[assignment]
        pixels = int(region["pixels"])
        warm_pixels = int(region["warm"])
        rows.append(
            {
                "file": str(image_path),
                "image": image_path.name,
                "frame": frame,
                "width": image.width,
                "height": image.height,
                "roi": clipped.name,
                "roi_rect": f"{clipped.left},{clipped.top},{clipped.right},{clipped.bottom}",
                "pixels": pixels,
                "avg_r": f"{avg[0]:.3f}",
                "avg_g": f"{avg[1]:.3f}",
                "avg_b": f"{avg[2]:.3f}",
                "max_r": max_rgb[0],
                "max_g": max_rgb[1],
                "max_b": max_rgb[2],
                "bright_pixels": int(region["bright"]),
                "white_pixels": int(region["white"]),
                "warm_pixels": warm_pixels,
                "warm_density_pct": f"{(warm_pixels / pixels * 100.0) if pixels else 0.0:.6f}",
                "hot_x": hot[0],
                "hot_y": hot[1],
                "hot_r": hot[2],
                "hot_g": hot[3],
                "hot_b": hot[4],
                "warm_hot_x": warm_hot[0],
                "warm_hot_y": warm_hot[1],
                "warm_hot_r": warm_hot[2],
                "warm_hot_g": warm_hot[3],
                "warm_hot_b": warm_hot[4],
            }
        )
    return rows


def is_warm_pixel(
    r: int,
    g: int,
    b: int,
    warm_red_threshold: int,
    warm_green_threshold: int,
    warm_blue_margin: int,
) -> bool:
    return (
        r >= warm_red_threshold
        and g >= warm_green_threshold
        and b <= r + warm_blue_margin
    )


def detect_warm_components(
    image: Image.Image,
    search_rois: Sequence[Roi],
    bright_threshold: int,
    white_threshold: int,
    warm_red_threshold: int,
    warm_green_threshold: int,
    warm_blue_margin: int,
    min_area: int,
    max_area: int,
    min_white: int,
    max_width: int,
    max_height: int,
    min_fill_pct: float,
    max_aspect_ratio: float,
) -> list[Component]:
    source = image.load()
    components: list[Component] = []
    next_component_id = 1
    for raw_roi in search_rois:
        roi = clamp_roi(raw_roi, image.width, image.height)
        if roi.right <= roi.left or roi.bottom <= roi.top:
            continue
        width = roi.right - roi.left
        height = roi.bottom - roi.top
        visited = bytearray(width * height)

        def local_index(x: int, y: int) -> int:
            return (y - roi.top) * width + (x - roi.left)

        for start_y in range(roi.top, roi.bottom):
            for start_x in range(roi.left, roi.right):
                start_index = local_index(start_x, start_y)
                if visited[start_index]:
                    continue
                visited[start_index] = 1
                r, g, b = source[start_x, start_y]
                if not is_warm_pixel(
                    r,
                    g,
                    b,
                    warm_red_threshold,
                    warm_green_threshold,
                    warm_blue_margin,
                ):
                    continue

                stack = [(start_x, start_y)]
                left = right = start_x
                top = bottom = start_y
                area = 0
                warm_pixels = 0
                white_pixels = 0
                bright_pixels = 0
                max_rgb = [0, 0, 0]
                hot = [0, 0, 0, 0, 0, 0]
                warm_hot = [0, 0, 0, 0, 0, 0]
                while stack:
                    x, y = stack.pop()
                    pr, pg, pb = source[x, y]
                    area += 1
                    warm_pixels += 1
                    left = min(left, x)
                    right = max(right, x)
                    top = min(top, y)
                    bottom = max(bottom, y)
                    max_rgb[0] = max(max_rgb[0], pr)
                    max_rgb[1] = max(max_rgb[1], pg)
                    max_rgb[2] = max(max_rgb[2], pb)
                    channel_max = max(pr, pg, pb)
                    if channel_max > bright_threshold:
                        bright_pixels += 1
                    if pr > white_threshold and pg > white_threshold and pb > white_threshold:
                        white_pixels += 1
                    if channel_max > hot[5]:
                        hot = [x, y, pr, pg, pb, channel_max]
                    warm_score = pr + pg + pb
                    if warm_score > warm_hot[5]:
                        warm_hot = [x, y, pr, pg, pb, warm_score]

                    for neighbor_y in (y - 1, y, y + 1):
                        if neighbor_y < roi.top or neighbor_y >= roi.bottom:
                            continue
                        for neighbor_x in (x - 1, x, x + 1):
                            if neighbor_x == x and neighbor_y == y:
                                continue
                            if neighbor_x < roi.left or neighbor_x >= roi.right:
                                continue
                            idx = local_index(neighbor_x, neighbor_y)
                            if visited[idx]:
                                continue
                            visited[idx] = 1
                            nr, ng, nb = source[neighbor_x, neighbor_y]
                            if is_warm_pixel(
                                nr,
                                ng,
                                nb,
                                warm_red_threshold,
                                warm_green_threshold,
                                warm_blue_margin,
                            ):
                                stack.append((neighbor_x, neighbor_y))

                component_width = right - left + 1
                component_height = bottom - top + 1
                bbox_area = component_width * component_height
                fill_pct = (area / bbox_area * 100.0) if bbox_area else 0.0
                aspect_ratio = (
                    max(component_width, component_height) / max(1, min(component_width, component_height))
                )
                if area < min_area:
                    continue
                if max_area > 0 and area > max_area:
                    continue
                if white_pixels < min_white:
                    continue
                if max_width > 0 and component_width > max_width:
                    continue
                if max_height > 0 and component_height > max_height:
                    continue
                if min_fill_pct > 0.0 and fill_pct < min_fill_pct:
                    continue
                if max_aspect_ratio > 0.0 and aspect_ratio > max_aspect_ratio:
                    continue
                components.append(
                    Component(
                        component_id=next_component_id,
                        search_roi=roi.name,
                        left=left,
                        top=top,
                        right=right + 1,
                        bottom=bottom + 1,
                        bbox_area=bbox_area,
                        bbox_fill_pct=fill_pct,
                        bbox_aspect_ratio=aspect_ratio,
                        area=area,
                        warm_pixels=warm_pixels,
                        white_pixels=white_pixels,
                        bright_pixels=bright_pixels,
                        max_rgb=(max_rgb[0], max_rgb[1], max_rgb[2]),
                        hot=(hot[0], hot[1], hot[2], hot[3], hot[4]),
                        warm_hot=(warm_hot[0], warm_hot[1], warm_hot[2], warm_hot[3], warm_hot[4]),
                    )
                )
                next_component_id += 1
    components.sort(
        key=lambda component: (
            -component.white_pixels,
            -component.warm_pixels,
            -component.bright_pixels,
            component.area,
            component.top,
            component.left,
        )
    )
    return components


def summarize_components_for_image(
    image_path: Path,
    search_rois: Sequence[Roi],
    bright_threshold: int,
    white_threshold: int,
    warm_red_threshold: int,
    warm_green_threshold: int,
    warm_blue_margin: int,
    min_area: int,
    max_area: int,
    min_white: int,
    max_width: int,
    max_height: int,
    min_fill_pct: float,
    max_aspect_ratio: float,
) -> list[dict[str, object]]:
    image = Image.open(image_path).convert("RGB")
    frame = extract_frame(image_path)
    components = detect_warm_components(
        image,
        search_rois,
        bright_threshold=bright_threshold,
        white_threshold=white_threshold,
        warm_red_threshold=warm_red_threshold,
        warm_green_threshold=warm_green_threshold,
        warm_blue_margin=warm_blue_margin,
        min_area=min_area,
        max_area=max_area,
        min_white=min_white,
        max_width=max_width,
        max_height=max_height,
        min_fill_pct=min_fill_pct,
        max_aspect_ratio=max_aspect_ratio,
    )
    rows: list[dict[str, object]] = []
    for component in components:
        rows.append(
            {
                "file": str(image_path),
                "image": image_path.name,
                "frame": frame,
                "component_id": component.component_id,
                "search_roi": component.search_roi,
                "bbox": f"{component.left},{component.top},{component.right},{component.bottom}",
                "bbox_width": component.right - component.left,
                "bbox_height": component.bottom - component.top,
                "bbox_area": component.bbox_area,
                "bbox_fill_pct": f"{component.bbox_fill_pct:.6f}",
                "bbox_aspect_ratio": f"{component.bbox_aspect_ratio:.6f}",
                "area": component.area,
                "warm_pixels": component.warm_pixels,
                "white_pixels": component.white_pixels,
                "bright_pixels": component.bright_pixels,
                "max_r": component.max_rgb[0],
                "max_g": component.max_rgb[1],
                "max_b": component.max_rgb[2],
                "hot_x": component.hot[0],
                "hot_y": component.hot[1],
                "hot_r": component.hot[2],
                "hot_g": component.hot[3],
                "hot_b": component.hot[4],
                "warm_hot_x": component.warm_hot[0],
                "warm_hot_y": component.warm_hot[1],
                "warm_hot_r": component.warm_hot[2],
                "warm_hot_g": component.warm_hot[3],
                "warm_hot_b": component.warm_hot[4],
            }
        )
    return rows


CSV_FIELDS = [
    "file",
    "image",
    "frame",
    "width",
    "height",
    "roi",
    "roi_rect",
    "pixels",
    "avg_r",
    "avg_g",
    "avg_b",
    "max_r",
    "max_g",
    "max_b",
    "bright_pixels",
    "white_pixels",
    "warm_pixels",
    "warm_density_pct",
    "hot_x",
    "hot_y",
    "hot_r",
    "hot_g",
    "hot_b",
    "warm_hot_x",
    "warm_hot_y",
    "warm_hot_r",
    "warm_hot_g",
    "warm_hot_b",
]


COMPONENT_FIELDS = [
    "file",
    "image",
    "frame",
    "component_id",
    "search_roi",
    "bbox",
    "bbox_width",
    "bbox_height",
    "bbox_area",
    "bbox_fill_pct",
    "bbox_aspect_ratio",
    "area",
    "warm_pixels",
    "white_pixels",
    "bright_pixels",
    "max_r",
    "max_g",
    "max_b",
    "hot_x",
    "hot_y",
    "hot_r",
    "hot_g",
    "hot_b",
    "warm_hot_x",
    "warm_hot_y",
    "warm_hot_r",
    "warm_hot_g",
    "warm_hot_b",
]


FRAME_SCORE_FIELDS = [
    "file",
    "image",
    "frame",
    "best_candidate_roi",
    "candidate_warm_pixels",
    "candidate_white_pixels",
    "candidate_bright_pixels",
    "best_control_roi",
    "control_warm_pixels",
    "control_white_pixels",
    "control_bright_pixels",
    "warm_delta",
    "warm_ratio",
    "verdict",
]


def int_sort_key(value: object) -> tuple[int, int | str]:
    text = str(value)
    if not text:
        return (1, "")
    try:
        return (0, int(text, 0))
    except ValueError:
        return (1, text)


def row_sort_key(row: dict[str, object]) -> tuple[object, ...]:
    return (
        int_sort_key(row["frame"]),
        str(row["image"]),
        str(row["roi"]),
    )


def signal_sort_key(row: dict[str, object]) -> tuple[object, ...]:
    return (
        -int(row["warm_pixels"]),
        -int(row["white_pixels"]),
        -int(row["bright_pixels"]),
        row_sort_key(row),
    )


def best_row(rows: Sequence[dict[str, object]]) -> dict[str, object] | None:
    if not rows:
        return None
    return max(
        rows,
        key=lambda row: (
            int(row["warm_pixels"]),
            int(row["white_pixels"]),
            int(row["bright_pixels"]),
        ),
    )


def build_frame_scores(
    rows: Sequence[dict[str, object]],
    candidate_rois: set[str],
    control_rois: set[str],
    min_candidate_warm: int,
    min_warm_ratio: float,
) -> list[dict[str, object]]:
    grouped: dict[tuple[str, str], list[dict[str, object]]] = {}
    for row in rows:
        grouped.setdefault((str(row["image"]), str(row["frame"])), []).append(row)

    scores: list[dict[str, object]] = []
    for (image, frame), frame_rows in grouped.items():
        file_path = str(frame_rows[0]["file"]) if frame_rows else ""
        candidates = [row for row in frame_rows if str(row["roi"]) in candidate_rois]
        controls = [row for row in frame_rows if str(row["roi"]) in control_rois]
        candidate = best_row(candidates)
        control = best_row(controls)
        candidate_warm = int(candidate["warm_pixels"]) if candidate else 0
        candidate_white = int(candidate["white_pixels"]) if candidate else 0
        candidate_bright = int(candidate["bright_pixels"]) if candidate else 0
        control_warm = int(control["warm_pixels"]) if control else 0
        control_white = int(control["white_pixels"]) if control else 0
        control_bright = int(control["bright_pixels"]) if control else 0
        warm_ratio = candidate_warm / max(1, control_warm)
        if candidate_warm <= 0 and control_warm <= 0:
            verdict = "no-warm-signal"
        elif candidate_warm >= min_candidate_warm and warm_ratio >= min_warm_ratio:
            verdict = "candidate-dominates-control"
        elif control_warm >= candidate_warm:
            verdict = "control-dominates"
        else:
            verdict = "ambiguous-candidate"
        scores.append(
            {
                "image": image,
                "file": file_path,
                "frame": frame,
                "best_candidate_roi": candidate["roi"] if candidate else "",
                "candidate_warm_pixels": candidate_warm,
                "candidate_white_pixels": candidate_white,
                "candidate_bright_pixels": candidate_bright,
                "best_control_roi": control["roi"] if control else "",
                "control_warm_pixels": control_warm,
                "control_white_pixels": control_white,
                "control_bright_pixels": control_bright,
                "warm_delta": candidate_warm - control_warm,
                "warm_ratio": f"{warm_ratio:.6f}",
                "verdict": verdict,
            }
        )
    scores.sort(
        key=lambda row: (
            str(row["verdict"]) != "candidate-dominates-control",
            -int(row["candidate_warm_pixels"]),
            -float(row["warm_ratio"]),
            int_sort_key(row["frame"]),
            str(row["image"]),
        )
    )
    return scores


def resize_to_fit(image: Image.Image, max_width: int, max_height: int) -> Image.Image:
    scale = min(max_width / image.width, max_height / image.height, 1.0)
    width = max(1, int(round(image.width * scale)))
    height = max(1, int(round(image.height * scale)))
    if width == image.width and height == image.height:
        return image.copy()
    return image.resize((width, height), Image.Resampling.LANCZOS)


def draw_scaled_roi(
    draw: ImageDraw.ImageDraw,
    roi: Roi,
    source_size: tuple[int, int],
    target_offset: tuple[int, int],
    target_size: tuple[int, int],
    color: tuple[int, int, int],
) -> None:
    source_width, source_height = source_size
    target_left, target_top = target_offset
    target_width, target_height = target_size
    x_scale = target_width / source_width
    y_scale = target_height / source_height
    draw.rectangle(
        (
            target_left + int(round(roi.left * x_scale)),
            target_top + int(round(roi.top * y_scale)),
            target_left + int(round(roi.right * x_scale)),
            target_top + int(round(roi.bottom * y_scale)),
        ),
        outline=color,
        width=2,
    )


def crop_roi_image(image: Image.Image, roi: Roi) -> Image.Image:
    clipped = clamp_roi(roi, image.width, image.height)
    if clipped.right <= clipped.left or clipped.bottom <= clipped.top:
        return Image.new("RGB", (1, 1), (0, 0, 0))
    return image.crop((clipped.left, clipped.top, clipped.right, clipped.bottom))


def write_frame_score_montage(
    frame_scores: Sequence[dict[str, object]],
    roi_map: dict[str, Roi],
    path: Path,
    limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    shown = frame_scores[:limit] if limit > 0 else frame_scores
    if not shown:
        Image.new("RGB", (320, 80), (20, 20, 20)).save(path)
        return

    margin = 8
    label_height = 38
    full_box = (240, 180)
    crop_box = (190, 150)
    row_width = margin * 4 + full_box[0] + crop_box[0] * 2
    row_height = margin * 2 + label_height + max(full_box[1], crop_box[1])
    canvas = Image.new("RGB", (row_width, row_height * len(shown)), (18, 18, 18))
    draw = ImageDraw.Draw(canvas)

    for index, score in enumerate(shown):
        top = index * row_height
        image = Image.open(Path(str(score["file"]))).convert("RGB")
        candidate_name = str(score["best_candidate_roi"])
        control_name = str(score["best_control_roi"])
        candidate_roi = roi_map.get(candidate_name)
        control_roi = roi_map.get(control_name)
        full = resize_to_fit(image, *full_box)
        full_left = margin
        image_top = top + margin + label_height
        canvas.paste(full, (full_left, image_top))
        if candidate_roi is not None:
            draw_scaled_roi(
                draw,
                candidate_roi,
                image.size,
                (full_left, image_top),
                full.size,
                (255, 128, 64),
            )
        if control_roi is not None:
            draw_scaled_roi(
                draw,
                control_roi,
                image.size,
                (full_left, image_top),
                full.size,
                (64, 192, 255),
            )

        candidate_crop = (
            resize_to_fit(crop_roi_image(image, candidate_roi), *crop_box)
            if candidate_roi is not None
            else Image.new("RGB", (1, 1), (0, 0, 0))
        )
        control_crop = (
            resize_to_fit(crop_roi_image(image, control_roi), *crop_box)
            if control_roi is not None
            else Image.new("RGB", (1, 1), (0, 0, 0))
        )
        candidate_left = margin * 2 + full_box[0]
        control_left = margin * 3 + full_box[0] + crop_box[0]
        canvas.paste(candidate_crop, (candidate_left, image_top))
        canvas.paste(control_crop, (control_left, image_top))
        draw.rectangle(
            (
                candidate_left,
                image_top,
                candidate_left + candidate_crop.width - 1,
                image_top + candidate_crop.height - 1,
            ),
            outline=(255, 128, 64),
            width=2,
        )
        draw.rectangle(
            (
                control_left,
                image_top,
                control_left + control_crop.width - 1,
                image_top + control_crop.height - 1,
            ),
            outline=(64, 192, 255),
            width=2,
        )

        title = (
            f"frame {score['frame']} {score['verdict']} "
            f"ratio {score['warm_ratio']}"
        )
        detail = (
            f"cand {candidate_name} {score['candidate_warm_pixels']}/"
            f"{score['candidate_white_pixels']}/"
            f"{score['candidate_bright_pixels']}  "
            f"ctrl {control_name} {score['control_warm_pixels']}/"
            f"{score['control_white_pixels']}/"
            f"{score['control_bright_pixels']}"
        )
        draw.text((margin, top + margin), title, fill=(235, 235, 235))
        draw.text((margin, top + margin + 16), detail, fill=(200, 200, 200))
        draw.text((candidate_left, top + margin + 20), "candidate", fill=(255, 180, 130))
        draw.text((control_left, top + margin + 20), "control", fill=(120, 210, 255))

    canvas.save(path)


def parse_bbox(value: object) -> tuple[int, int, int, int]:
    left, top, right, bottom = (int(part) for part in str(value).split(","))
    return left, top, right, bottom


def write_component_csv(rows: Sequence[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COMPONENT_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_component_markdown(
    rows: Sequence[dict[str, object]],
    path: Path,
    limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    shown = rows[:limit] if limit > 0 else rows
    lines = [
        "# Capture Warm Component Summary",
        "",
        f"- Rows: `{len(rows)}`",
        "",
        "| Image | Frame | Component | ROI | BBox | Aspect | Fill | Area | Warm | White | Bright | Max RGB | Hot | Warm Hot |",
        "|---|---:|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|",
    ]
    for row in shown:
        lines.append(
            "| "
            + " | ".join(
                [
                    str(row["image"]),
                    str(row["frame"]),
                    str(row["component_id"]),
                    str(row["search_roi"]),
                    str(row["bbox"]),
                    str(row["bbox_aspect_ratio"]),
                    f"{row['bbox_fill_pct']}%",
                    str(row["area"]),
                    str(row["warm_pixels"]),
                    str(row["white_pixels"]),
                    str(row["bright_pixels"]),
                    f"[{row['max_r']},{row['max_g']},{row['max_b']}]",
                    f"({row['hot_x']},{row['hot_y']}) [{row['hot_r']},{row['hot_g']},{row['hot_b']}]",
                    f"({row['warm_hot_x']},{row['warm_hot_y']}) [{row['warm_hot_r']},{row['warm_hot_g']},{row['warm_hot_b']}]",
                ]
            )
            + " |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_component_montage(
    rows: Sequence[dict[str, object]],
    path: Path,
    limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    shown = rows[:limit] if limit > 0 else rows
    if not shown:
        Image.new("RGB", (320, 80), (20, 20, 20)).save(path)
        return

    margin = 8
    label_height = 38
    full_box = (240, 180)
    crop_box = (220, 150)
    row_width = margin * 3 + full_box[0] + crop_box[0]
    row_height = margin * 2 + label_height + max(full_box[1], crop_box[1])
    canvas = Image.new("RGB", (row_width, row_height * len(shown)), (18, 18, 18))
    draw = ImageDraw.Draw(canvas)

    for index, row in enumerate(shown):
        top = index * row_height
        image = Image.open(Path(str(row["file"]))).convert("RGB")
        left, bbox_top, right, bottom = parse_bbox(row["bbox"])
        bbox_roi = Roi("component", left, bbox_top, right, bottom)
        full = resize_to_fit(image, *full_box)
        full_left = margin
        image_top = top + margin + label_height
        canvas.paste(full, (full_left, image_top))
        draw_scaled_roi(
            draw,
            bbox_roi,
            image.size,
            (full_left, image_top),
            full.size,
            (255, 128, 64),
        )
        crop = resize_to_fit(crop_roi_image(image, bbox_roi), *crop_box)
        crop_left = margin * 2 + full_box[0]
        canvas.paste(crop, (crop_left, image_top))
        draw.rectangle(
            (
                crop_left,
                image_top,
                crop_left + crop.width - 1,
                image_top + crop.height - 1,
            ),
            outline=(255, 128, 64),
            width=2,
        )
        title = (
            f"frame {row['frame']} component {row['component_id']} "
            f"bbox {row['bbox']}"
        )
        detail = (
            f"area {row['area']} warm/white/bright "
            f"{row['warm_pixels']}/{row['white_pixels']}/{row['bright_pixels']} "
            f"aspect {row['bbox_aspect_ratio']} fill {row['bbox_fill_pct']}%"
        )
        draw.text((margin, top + margin), title, fill=(235, 235, 235))
        draw.text((margin, top + margin + 16), detail, fill=(200, 200, 200))

    canvas.save(path)


def write_csv(rows: Sequence[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_frame_score_csv(rows: Sequence[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FRAME_SCORE_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(rows: Sequence[dict[str, object]], path: Path, limit: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    shown = rows[:limit] if limit > 0 else rows
    lines = [
        "# Capture ROI Summary",
        "",
        f"- Rows: `{len(rows)}`",
        "",
        "| Image | Frame | ROI | Max RGB | Bright | White | Warm | Warm Density | Hot | Warm Hot |",
        "|---|---:|---|---:|---:|---:|---:|---:|---|---|",
    ]
    for row in shown:
        lines.append(
            "| "
            + " | ".join(
                [
                    str(row["image"]),
                    str(row["frame"]),
                    f"{row['roi']} `{row['roi_rect']}`",
                    f"[{row['max_r']},{row['max_g']},{row['max_b']}]",
                    str(row["bright_pixels"]),
                    str(row["white_pixels"]),
                    str(row["warm_pixels"]),
                    f"{row['warm_density_pct']}%",
                    f"({row['hot_x']},{row['hot_y']}) [{row['hot_r']},{row['hot_g']},{row['hot_b']}]",
                    f"({row['warm_hot_x']},{row['warm_hot_y']}) [{row['warm_hot_r']},{row['warm_hot_g']},{row['warm_hot_b']}]",
                ]
            )
            + " |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_frame_score_markdown(
    rows: Sequence[dict[str, object]],
    path: Path,
    limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    shown = rows[:limit] if limit > 0 else rows
    lines = [
        "# Capture Frame Score Summary",
        "",
        f"- Rows: `{len(rows)}`",
        "",
        "| Image | Frame | Candidate ROI | Candidate Warm/White/Bright | Control ROI | Control Warm/White/Bright | Warm Ratio | Verdict |",
        "|---|---:|---|---:|---|---:|---:|---|",
    ]
    for row in shown:
        lines.append(
            "| "
            + " | ".join(
                [
                    str(row["image"]),
                    str(row["frame"]),
                    str(row["best_candidate_roi"]),
                    f"{row['candidate_warm_pixels']}/{row['candidate_white_pixels']}/{row['candidate_bright_pixels']}",
                    str(row["best_control_roi"]),
                    f"{row['control_warm_pixels']}/{row['control_white_pixels']}/{row['control_bright_pixels']}",
                    str(row["warm_ratio"]),
                    str(row["verdict"]),
                ]
            )
            + " |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, help="Image files or directories")
    parser.add_argument("--roi", action="append", type=parse_roi, default=[], help="ROI L,T,R,B[:name]")
    parser.add_argument("--bright-threshold", type=int, default=220)
    parser.add_argument("--white-threshold", type=int, default=240)
    parser.add_argument("--warm-red-threshold", type=int, default=180)
    parser.add_argument("--warm-green-threshold", type=int, default=110)
    parser.add_argument("--warm-blue-margin", type=int, default=32)
    parser.add_argument(
        "--sort",
        choices=("frame", "signal"),
        default="frame",
        help="Sort by frame/ROI order or warm/white/bright signal strength",
    )
    parser.add_argument(
        "--include-name-regex",
        help="Only include image file names matching this regular expression",
    )
    parser.add_argument(
        "--exclude-name-regex",
        help="Exclude image file names matching this regular expression",
    )
    parser.add_argument("--output", type=Path, required=True, help="Markdown output")
    parser.add_argument("--csv-output", type=Path, help="CSV output")
    parser.add_argument("--limit", type=int, default=80, help="Markdown row limit; 0 means all")
    parser.add_argument("--candidate-roi", action="append", default=[], help="ROI name to use as a candidate in frame-score output")
    parser.add_argument("--control-roi", action="append", default=[], help="ROI name to use as a control/glare ROI in frame-score output")
    parser.add_argument("--min-candidate-warm", type=int, default=100)
    parser.add_argument("--min-warm-ratio", type=float, default=2.0)
    parser.add_argument("--frame-score-output", type=Path, help="Optional frame-score Markdown output")
    parser.add_argument("--frame-score-csv-output", type=Path, help="Optional frame-score CSV output")
    parser.add_argument("--frame-score-montage-output", type=Path, help="Optional frame-score candidate/control crop montage")
    parser.add_argument("--montage-limit", type=int, default=12, help="Frame-score montage row limit; 0 means all")
    parser.add_argument("--component-search-roi", action="append", type=parse_roi, default=[], help="ROI L,T,R,B[:name] to search for warm components; default is full image")
    parser.add_argument("--component-min-area", type=int, default=8)
    parser.add_argument("--component-max-area", type=int, default=2000, help="0 disables max area filtering")
    parser.add_argument("--component-min-white", type=int, default=1)
    parser.add_argument("--component-max-width", type=int, default=220, help="0 disables max width filtering")
    parser.add_argument("--component-max-height", type=int, default=180, help="0 disables max height filtering")
    parser.add_argument("--component-min-fill-pct", type=float, default=0.0, help="0 disables bbox fill filtering")
    parser.add_argument("--component-max-aspect-ratio", type=float, default=0.0, help="0 disables aspect-ratio filtering")
    parser.add_argument("--component-output", type=Path, help="Optional warm-component Markdown output")
    parser.add_argument("--component-csv-output", type=Path, help="Optional warm-component CSV output")
    parser.add_argument("--component-montage-output", type=Path, help="Optional warm-component crop montage output")
    parser.add_argument("--component-limit", type=int, default=80, help="Component Markdown row limit; 0 means all")
    parser.add_argument("--component-montage-limit", type=int, default=24, help="Component montage row limit; 0 means all")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    rois = args.roi or [Roi("full", 0, 0, 1 << 30, 1 << 30)]
    image_paths = iter_images(
        args.paths,
        include_name_regex=args.include_name_regex,
        exclude_name_regex=args.exclude_name_regex,
    )
    rows: list[dict[str, object]] = []
    for image_path in image_paths:
        rows.extend(
            summarize_image(
                image_path,
                rois,
                bright_threshold=args.bright_threshold,
                white_threshold=args.white_threshold,
                warm_red_threshold=args.warm_red_threshold,
                warm_green_threshold=args.warm_green_threshold,
                warm_blue_margin=args.warm_blue_margin,
            )
        )
    rows.sort(key=signal_sort_key if args.sort == "signal" else row_sort_key)
    write_markdown(rows, args.output, args.limit)
    if args.csv_output:
        write_csv(rows, args.csv_output)
    if args.frame_score_output or args.frame_score_csv_output or args.frame_score_montage_output:
        if not args.candidate_roi or not args.control_roi:
            raise SystemExit("--frame-score-output requires --candidate-roi and --control-roi")
        roi_map = {roi.name: roi for roi in rois}
        frame_scores = build_frame_scores(
            rows,
            set(args.candidate_roi),
            set(args.control_roi),
            min_candidate_warm=args.min_candidate_warm,
            min_warm_ratio=args.min_warm_ratio,
        )
        if args.frame_score_output:
            write_frame_score_markdown(frame_scores, args.frame_score_output, args.limit)
        if args.frame_score_csv_output:
            write_frame_score_csv(frame_scores, args.frame_score_csv_output)
        if args.frame_score_montage_output:
            write_frame_score_montage(
                frame_scores,
                roi_map,
                args.frame_score_montage_output,
                args.montage_limit,
            )
    if args.component_output or args.component_csv_output or args.component_montage_output:
        component_rois = args.component_search_roi or [Roi("full", 0, 0, 1 << 30, 1 << 30)]
        component_rows: list[dict[str, object]] = []
        for image_path in image_paths:
            component_rows.extend(
                summarize_components_for_image(
                    image_path,
                    component_rois,
                    bright_threshold=args.bright_threshold,
                    white_threshold=args.white_threshold,
                    warm_red_threshold=args.warm_red_threshold,
                    warm_green_threshold=args.warm_green_threshold,
                    warm_blue_margin=args.warm_blue_margin,
                    min_area=args.component_min_area,
                    max_area=args.component_max_area,
                    min_white=args.component_min_white,
                    max_width=args.component_max_width,
                    max_height=args.component_max_height,
                    min_fill_pct=args.component_min_fill_pct,
                    max_aspect_ratio=args.component_max_aspect_ratio,
                )
            )
        component_rows.sort(
            key=lambda row: (
                -int(row["white_pixels"]),
                -int(row["warm_pixels"]),
                -int(row["bright_pixels"]),
                int_sort_key(row["frame"]),
                str(row["image"]),
                int(row["component_id"]),
            )
        )
        if args.component_output:
            write_component_markdown(component_rows, args.component_output, args.component_limit)
        if args.component_csv_output:
            write_component_csv(component_rows, args.component_csv_output)
        if args.component_montage_output:
            write_component_montage(
                component_rows,
                args.component_montage_output,
                args.component_montage_limit,
            )
    print(args.output)
    if args.csv_output:
        print(args.csv_output)
    if args.frame_score_output:
        print(args.frame_score_output)
    if args.frame_score_csv_output:
        print(args.frame_score_csv_output)
    if args.frame_score_montage_output:
        print(args.frame_score_montage_output)
    if args.component_output:
        print(args.component_output)
    if args.component_csv_output:
        print(args.component_csv_output)
    if args.component_montage_output:
        print(args.component_montage_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
