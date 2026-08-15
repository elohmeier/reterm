#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=2", "pillow>=11", "rawpy>=0.27", "tifffile>=2025.6"]
# ///

"""Measure an E1004 color target from an Apple ProRAW DNG."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
from pathlib import Path
from typing import Any

import numpy as np
import rawpy
import tifffile
from PIL import Image, ImageDraw, ImageFilter, ImageOps

REPO = Path(__file__).resolve().parents[1]
D65 = np.array((0.95047, 1.0, 1.08883), dtype=np.float64)
BRADFORD = np.array(
    ((0.8951, 0.2664, -0.1614),
     (-0.7502, 1.7135, 0.0367),
     (0.0389, -0.0685, 1.0296)),
    dtype=np.float64,
)
XYZ_TO_LINEAR_SRGB = np.array(
    ((3.2404542, -1.5371385, -0.4985314),
     (-0.9692660, 1.8760108, 0.0415560),
     (0.0556434, -0.2040259, 1.0572252)),
    dtype=np.float64,
)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def box_sum(integral: np.ndarray, radius: int) -> np.ndarray:
    width = radius * 2
    return (
        integral[width:, width:]
        - integral[:-width, width:]
        - integral[width:, :-width]
        + integral[:-width, :-width]
    )


def marker_score(gray: np.ndarray, radius: int) -> np.ndarray:
    """Return a black/white/black nested-square response at each valid center."""
    middle = max(2, round(radius * 0.69))
    inner = max(1, round(radius * 0.33))
    integral = np.pad(gray, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    outer_sum = box_sum(integral, radius)

    # Crop the smaller box maps so all arrays refer to the same centers.
    def centered_sum(smaller: int) -> np.ndarray:
        values = box_sum(integral, smaller)
        inset = radius - smaller
        return values[inset:inset + outer_sum.shape[0],
                      inset:inset + outer_sum.shape[1]]

    middle_sum = centered_sum(middle)
    inner_sum = centered_sum(inner)
    outer_area = (radius * 2) ** 2
    middle_area = (middle * 2) ** 2
    inner_area = (inner * 2) ** 2
    outer_ring = (outer_sum - middle_sum) / (outer_area - middle_area)
    white_ring = (middle_sum - inner_sum) / (middle_area - inner_area)
    black_center = inner_sum / inner_area
    return white_ring - (outer_ring + black_center) * 0.5


def find_marker(
    gray: np.ndarray,
    region: tuple[float, float, float, float],
) -> tuple[float, float, float, int]:
    height, width = gray.shape
    x0, y0, x1, y1 = region
    left, right = round(x0 * width), round(x1 * width)
    top, bottom = round(y0 * height), round(y1 * height)
    crop = gray[top:bottom, left:right]
    best = (-math.inf, 0.0, 0.0, 0)
    for radius in range(22, 43, 2):
        if crop.shape[0] <= radius * 2 or crop.shape[1] <= radius * 2:
            continue
        score = marker_score(crop, radius)
        flat_index = int(np.argmax(score))
        y, x = np.unravel_index(flat_index, score.shape)
        value = float(score[y, x])
        if value > best[0]:
            # box_sum's output [0,0] is centered at (radius, radius).
            best = (value, left + x + radius, top + y + radius, radius)
    if best[0] < 0.12:
        raise RuntimeError(f"could not find fiducial (best response {best[0]:.3f})")
    return best


def extract_geometry(
    dng: Path,
    manifest: dict[str, Any],
    diagnostic_dir: Path,
) -> tuple[np.ndarray, list[dict[str, float]], tuple[int, int], int]:
    with rawpy.imread(str(dng)) as raw:
        thumb = raw.extract_thumb()
        if thumb.format != rawpy.ThumbFormat.JPEG:
            raise RuntimeError("DNG does not contain the expected JPEG preview")
        preview_bytes = thumb.data
        raw_size = (raw.sizes.raw_width, raw.sizes.raw_height)
        orientation = int(raw.sizes.flip)

    preview = Image.open(io.BytesIO(preview_bytes))
    # Decode the 48 MP JPEG directly at quarter resolution.
    preview.draft("RGB", (preview.width // 4, preview.height // 4))
    preview = ImageOps.exif_transpose(preview).convert("RGB")
    full_oriented_size = (
        (raw_size[1], raw_size[0]) if orientation in (5, 6, 7, 8) else raw_size
    )
    scale_x = full_oriented_size[0] / preview.width
    scale_y = full_oriented_size[1] / preview.height

    gray = np.asarray(preview.convert("L"), dtype=np.float32) / 255.0
    regions = {
        "top_left": (0.08, 0.05, 0.36, 0.34),
        "top_right": (0.64, 0.05, 0.92, 0.34),
        "bottom_right": (0.64, 0.59, 0.92, 0.84),
        "bottom_left": (0.08, 0.59, 0.36, 0.84),
    }
    detections: list[dict[str, float]] = []
    for entry in manifest["fiducials"]:
        score, x, y, radius = find_marker(gray, regions[entry["corner"]])
        detections.append({
            "corner": entry["corner"],
            "preview_x": float(x),
            "preview_y": float(y),
            "oriented_x": float(x * scale_x),
            "oriented_y": float(y * scale_y),
            "response": float(score),
            "preview_radius": int(radius),
        })

    target_points = np.asarray(
        [entry["center"] for entry in manifest["fiducials"]], dtype=np.float64
    )
    photo_points = np.asarray(
        [[entry["oriented_x"], entry["oriented_y"]] for entry in detections],
        dtype=np.float64,
    )
    homography = solve_homography(target_points, photo_points)

    top = np.linalg.norm(photo_points[1] - photo_points[0])
    bottom = np.linalg.norm(photo_points[2] - photo_points[3])
    right = np.linalg.norm(photo_points[2] - photo_points[1])
    left = np.linalg.norm(photo_points[3] - photo_points[0])
    observed_aspect = ((top + bottom) / 2) / ((left + right) / 2)
    expected_aspect = (
        (target_points[1, 0] - target_points[0, 0])
        / (target_points[3, 1] - target_points[0, 1])
    )
    if abs(observed_aspect / expected_aspect - 1) > 0.12:
        raise RuntimeError(
            f"fiducial geometry has implausible aspect {observed_aspect:.3f}"
        )

    annotated = preview.copy()
    draw = ImageDraw.Draw(annotated)
    for entry in detections:
        x, y = entry["preview_x"], entry["preview_y"]
        radius = entry["preview_radius"]
        draw.ellipse((x - radius, y - radius, x + radius, y + radius),
                     outline=(255, 0, 255), width=5)
        draw.line((x - radius, y, x + radius, y), fill=(255, 0, 255), width=3)
        draw.line((x, y - radius, x, y + radius), fill=(255, 0, 255), width=3)
    annotated.save(diagnostic_dir / "geometry.jpg", quality=94)

    preview_homography = homography.copy()
    preview_homography[0, :] /= scale_x
    preview_homography[1, :] /= scale_y
    coefficients = tuple(preview_homography.flatten()[:8])
    rectified = preview.transform(
        tuple(manifest["size"]),
        Image.Transform.PERSPECTIVE,
        coefficients,
        Image.Resampling.BICUBIC,
    )
    rectified.save(diagnostic_dir / "rectified.jpg", quality=94)
    return homography, detections, raw_size, orientation


def solve_homography(source: np.ndarray, destination: np.ndarray) -> np.ndarray:
    rows: list[list[float]] = []
    values: list[float] = []
    for (x, y), (u, v) in zip(source, destination, strict=True):
        rows.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        values.append(u)
        rows.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        values.append(v)
    solution = np.linalg.solve(np.asarray(rows), np.asarray(values))
    return np.append(solution, 1).reshape(3, 3)


def transform_points(homography: np.ndarray, points: np.ndarray) -> np.ndarray:
    homogeneous = np.column_stack((points, np.ones(len(points))))
    mapped = homogeneous @ homography.T
    return mapped[:, :2] / mapped[:, 2:3]


def oriented_to_raw(
    points: np.ndarray,
    raw_size: tuple[int, int],
    orientation: int,
) -> np.ndarray:
    x, y = points[:, 0], points[:, 1]
    width, height = raw_size
    if orientation == 1:
        return np.column_stack((x, y))
    if orientation == 3:
        return np.column_stack((width - 1 - x, height - 1 - y))
    if orientation == 6:
        return np.column_stack((y, height - 1 - x))
    if orientation == 8:
        return np.column_stack((width - 1 - y, x))
    raise RuntimeError(f"unsupported DNG orientation {orientation}")


def sample_raw(raw_image: np.ndarray, points: np.ndarray) -> np.ndarray:
    """Bilinearly sample RGB from points expressed as raw (x, y)."""
    x = np.clip(points[:, 0], 0, raw_image.shape[1] - 1.001)
    y = np.clip(points[:, 1], 0, raw_image.shape[0] - 1.001)
    x0, y0 = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
    dx, dy = (x - x0)[:, None], (y - y0)[:, None]
    top = (raw_image[y0, x0, :3] * (1 - dx)
           + raw_image[y0, x0 + 1, :3] * dx)
    bottom = (raw_image[y0 + 1, x0, :3] * (1 - dx)
              + raw_image[y0 + 1, x0 + 1, :3] * dx)
    return top * (1 - dy) + bottom * dy


def features(points: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    x = points[:, 0] / size[0] * 2 - 1
    y = points[:, 1] / size[1] * 2 - 1
    return np.column_stack((np.ones(len(points)), x, y, x * x, x * y, y * y))


def fit_reference_surface(
    target: Image.Image,
    palette_index: int,
    homography: np.ndarray,
    raw_image: np.ndarray,
    raw_size: tuple[int, int],
    orientation: int,
    *,
    log_space: bool,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    indices = np.asarray(target)
    erosion = 15 if palette_index == 1 else 9
    safe_reference = Image.fromarray(
        np.uint8(indices == palette_index) * 255
    ).filter(
        ImageFilter.MinFilter(erosion)
    )
    safe = np.asarray(safe_reference) == 255
    step = 24 if palette_index == 1 else 12
    yy, xx = np.mgrid[30:target.height - 30:step,
                      30:target.width - 30:step]
    candidates = np.column_stack((xx.ravel(), yy.ravel()))
    candidates = candidates[safe[candidates[:, 1], candidates[:, 0]]]
    oriented = transform_points(homography, candidates)
    raw_points = oriented_to_raw(oriented, raw_size, orientation)
    values = sample_raw(raw_image, raw_points)
    design = features(candidates, target.size)

    valid = np.all(values > 32, axis=1)
    coefficients = np.zeros((design.shape[1], 3), dtype=np.float64)
    for _ in range(5):
        observations = np.log(values) if log_space else values
        coefficients = np.linalg.lstsq(
            design[valid], observations[valid], rcond=None
        )[0]
        residual = observations - design @ coefficients
        if log_space:
            normalized_residual = residual
            minimum_limit = 0.035
        else:
            channel_scale = np.maximum(np.median(values[valid], axis=0), 1)
            normalized_residual = residual / channel_scale
            minimum_limit = 0.045
        magnitude = np.max(np.abs(normalized_residual), axis=1)
        center = np.median(magnitude[valid])
        mad = np.median(np.abs(magnitude[valid] - center))
        limit = max(minimum_limit, center + 3.5 * 1.4826 * mad)
        valid = np.all(values > 32, axis=1) & (magnitude < limit)
    if valid.sum() < 80:
        name = "white" if palette_index == 1 else "black"
        raise RuntimeError(f"too few clean {name} samples for illumination correction")
    return coefficients, candidates, valid


def sample_patch_points(rect: list[int], step: float = 0.25) -> np.ndarray:
    x, y, width, height = rect
    yy, xx = np.mgrid[y + 0.5:y + height:step, x + 0.5:x + width:step]
    return np.column_stack((xx.ravel(), yy.ravel()))


def parse_matrix(value: Any) -> np.ndarray:
    items = list(value)
    if len(items) == 18 and all(isinstance(item, int) for item in items):
        items = [items[index] / items[index + 1] for index in range(0, 18, 2)]
    if len(items) != 9:
        raise RuntimeError("unexpected DNG ColorMatrix representation")
    return np.asarray(items, dtype=np.float64).reshape(3, 3)


def color_transform(dng: Path) -> tuple[np.ndarray, np.ndarray]:
    with tifffile.TiffFile(dng) as tiff:
        tags = tiff.pages[0].tags
        camera_to_xyz = np.linalg.inv(parse_matrix(tags[50722].value))
    source_white = camera_to_xyz @ np.ones(3)
    source_white /= source_white[1]
    source_lms = BRADFORD @ source_white
    destination_lms = BRADFORD @ D65
    adaptation = np.linalg.inv(BRADFORD) @ np.diag(
        destination_lms / source_lms
    ) @ BRADFORD
    transform = adaptation @ camera_to_xyz
    transform /= (transform @ np.ones(3))[1]
    return transform, source_white


def xyz_to_lab(xyz: np.ndarray) -> np.ndarray:
    ratio = xyz / D65
    delta = 6 / 29
    adjusted = np.where(
        ratio > delta ** 3,
        np.cbrt(np.maximum(ratio, 0)),
        ratio / (3 * delta * delta) + 4 / 29,
    )
    return np.array((
        116 * adjusted[1] - 16,
        500 * (adjusted[0] - adjusted[1]),
        200 * (adjusted[1] - adjusted[2]),
    ))


def xyz_to_srgb(xyz: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    linear = XYZ_TO_LINEAR_SRGB @ xyz
    clipped = np.clip(linear, 0, 1)
    encoded = np.where(
        clipped <= 0.0031308,
        clipped * 12.92,
        1.055 * np.power(clipped, 1 / 2.4) - 0.055,
    )
    return linear, encoded


def color_record(camera_rgb: np.ndarray, transform: np.ndarray) -> dict[str, Any]:
    xyz = transform @ camera_rgb
    linear_srgb, srgb = xyz_to_srgb(xyz)
    return {
        "camera_rgb_relative": camera_rgb.tolist(),
        "xyz_d65": xyz.tolist(),
        "lab_d65": xyz_to_lab(xyz).tolist(),
        "linear_srgb": linear_srgb.tolist(),
        "srgb": srgb.tolist(),
        "srgb8": np.rint(srgb * 255).astype(int).tolist(),
    }


def measure(
    dng: Path,
    target_path: Path,
    manifest: dict[str, Any],
    homography: np.ndarray,
    raw_size: tuple[int, int],
    orientation: int,
) -> tuple[dict[str, Any], Image.Image]:
    target = Image.open(target_path)
    if target.mode != "P" or list(target.size) != manifest["size"]:
        raise RuntimeError("target PNG does not match the calibration manifest")
    transform, source_white = color_transform(dng)

    with rawpy.imread(str(dng)) as raw:
        raw_image = raw.raw_image_visible
        white_coefficients, white_points, white_inliers = fit_reference_surface(
            target, 1, homography, raw_image, raw_size, orientation,
            log_space=True
        )
        black_coefficients, black_points, black_inliers = fit_reference_surface(
            target, 0, homography, raw_image, raw_size, orientation,
            log_space=False
        )
        black_oriented = transform_points(homography, black_points)
        black_raw_points = oriented_to_raw(black_oriented, raw_size, orientation)
        black_raw_values = sample_raw(raw_image, black_raw_points)
        black_local_white = np.exp(
            features(black_points, target.size) @ white_coefficients
        )
        # Retain the pigment's real spectral floor while removing spatial
        # veiling. The least-veiled five percent are a robust estimate of how
        # this panel black reflects under the capture illuminant.
        camera_black_floor = np.clip(
            np.quantile(
                black_raw_values[black_inliers] / black_local_white[black_inliers],
                0.05,
                axis=0,
            ),
            0,
            0.5,
        )
        patches: list[dict[str, Any]] = []
        for source_patch in manifest["patches"]:
            target_points = sample_patch_points(source_patch["sample_rect"])
            oriented = transform_points(homography, target_points)
            raw_points = oriented_to_raw(oriented, raw_size, orientation)
            raw_values = sample_raw(raw_image, raw_points)
            local_features = features(target_points, target.size)
            local_white = np.exp(local_features @ white_coefficients)
            local_black = local_features @ black_coefficients
            black_relative = (
                (raw_values - local_black) / (local_white - local_black)
            )
            relative = (
                camera_black_floor
                + black_relative * (1 - camera_black_floor)
            )
            camera_rgb = np.mean(relative, axis=0)
            patch = dict(source_patch)
            patch["measured"] = color_record(camera_rgb, transform)
            patch["sample_count"] = len(target_points)
            patches.append(patch)

    endpoint_values: dict[int, list[np.ndarray]] = {
        entry["index"]: [] for entry in manifest["palette"]
    }
    for patch in patches:
        fraction = patch["second_fraction"]
        if fraction == 0:
            endpoint_values[patch["first_index"]].append(
                np.asarray(patch["measured"]["camera_rgb_relative"])
            )
        if fraction == 1:
            endpoint_values[patch["second_index"]].append(
                np.asarray(patch["measured"]["camera_rgb_relative"])
            )

    solid_patches = {
        patch["index"]: patch for patch in patches if patch["kind"] == "solid"
    }
    palette: list[dict[str, Any]] = []
    palette_camera: dict[int, np.ndarray] = {}
    for entry in manifest["palette"]:
        samples = np.asarray(endpoint_values[entry["index"]])
        solid_camera = np.asarray(
            solid_patches[entry["index"]]["measured"]["camera_rgb_relative"]
        )
        palette_camera[entry["index"]] = solid_camera
        measured = color_record(solid_camera, transform)
        measured["endpoint_std_camera"] = np.std(samples, axis=0).tolist()
        measured["endpoint_mean_camera"] = np.mean(samples, axis=0).tolist()
        measured["endpoint_samples"] = len(samples)
        palette.append({**entry, "measured": measured})

    # Every dither row has photographed 0/8 and 8/8 endpoints. Use them to
    # remove residual row-local flare, anchoring the curve to the large solid
    # references while retaining any real nonlinear behavior between inks.
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for patch in patches:
        if patch["kind"] == "neutral":
            key = ("neutral",)
        elif patch["kind"] == "pairwise":
            key = ("pairwise", patch["first_index"], patch["second_index"])
        else:
            patch["endpoint_normalized"] = patch["measured"]
            continue
        groups.setdefault(key, []).append(patch)

    for group in groups.values():
        start_patch = next(patch for patch in group if patch["second_fraction"] == 0)
        end_patch = next(patch for patch in group if patch["second_fraction"] == 1)
        start_observed = np.asarray(
            start_patch["measured"]["camera_rgb_relative"]
        )
        end_observed = np.asarray(end_patch["measured"]["camera_rgb_relative"])
        first = start_patch["first_index"]
        second = start_patch["second_index"]
        for patch in group:
            fraction = patch["second_fraction"]
            observed = np.asarray(patch["measured"]["camera_rgb_relative"])
            adjusted = (
                observed
                + (1 - fraction) * (palette_camera[first] - start_observed)
                + fraction * (palette_camera[second] - end_observed)
            )
            patch["endpoint_normalized"] = color_record(adjusted, transform)

    mixture_errors: list[float] = []
    for patch in patches:
        fraction = patch["second_fraction"]
        expected = (
            palette_camera[patch["first_index"]] * (1 - fraction)
            + palette_camera[patch["second_index"]] * fraction
        )
        observed = np.asarray(
            patch["endpoint_normalized"]["camera_rgb_relative"]
        )
        error = float(np.sqrt(np.mean(np.square(observed - expected))))
        patch["linear_mixture_rmse_camera"] = error
        if 0 < fraction < 1:
            mixture_errors.append(error)

    target_rgb = target.convert("RGB")
    reconstruction = target_rgb.copy()
    draw = ImageDraw.Draw(reconstruction)
    for patch in patches:
        x, y, width, height = patch["rect"]
        color = tuple(patch["endpoint_normalized"]["srgb8"])
        draw.rectangle((x + 3, y + 3, x + width - 4, y + height - 4), fill=color)

    design = features(white_points, target.size)
    predicted = np.exp(design @ white_coefficients)
    relative_range = predicted / np.median(predicted, axis=0)
    black_design = features(black_points, target.size)
    predicted_black = black_design @ black_coefficients
    relative_black_range = predicted_black / np.median(predicted_black, axis=0)
    profile = {
        "schema": "reterm.e1004.color-profile.v1",
        "target_id": manifest["target_id"],
        "method": {
            "raw": "Apple ProRAW LinearRaw planes sampled without tone curve",
            "geometry": "four nested-square fiducials and projective transform",
            "illumination": "robust spatial black/white quadratic two-point correction",
            "color": "DNG ColorMatrix2 inverted and Bradford-adapted to D65",
        },
        "dng_source_white_xyz": source_white.tolist(),
        "illumination": {
            "white_candidates": len(white_points),
            "white_inliers": int(white_inliers.sum()),
            "white_relative_min": np.min(
                relative_range[white_inliers], axis=0
            ).tolist(),
            "white_relative_max": np.max(
                relative_range[white_inliers], axis=0
            ).tolist(),
            "log_surface_coefficients": white_coefficients.tolist(),
            "black_candidates": len(black_points),
            "black_inliers": int(black_inliers.sum()),
            "black_relative_min": np.min(
                relative_black_range[black_inliers], axis=0
            ).tolist(),
            "black_relative_max": np.max(
                relative_black_range[black_inliers], axis=0
            ).tolist(),
            "black_surface_coefficients": black_coefficients.tolist(),
            "camera_black_floor": camera_black_floor.tolist(),
        },
        "palette": palette,
        "patches": patches,
        "quality": {
            "pairwise_mixture_rmse_camera_mean": float(np.mean(mixture_errors)),
            "pairwise_mixture_rmse_camera_max": float(np.max(mixture_errors)),
        },
    }
    return profile, reconstruction


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dng", type=Path)
    parser.add_argument("--target", type=Path,
                        default=Path("/root/.cache/e1004-color-target.png"))
    parser.add_argument("--manifest", type=Path,
                        default=Path("/root/.cache/e1004-color-target.json"))
    parser.add_argument("--output-dir", type=Path,
                        default=REPO / "profiles" / "e1004-IMG_5327")
    parser.add_argument("--diagnostic-dir", type=Path,
                        help="private photo diagnostics (defaults under /root/.cache)")
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    diagnostic_dir = args.diagnostic_dir or (
        Path("/root/.cache") / f"e1004-calibration-{args.dng.stem}"
    )
    diagnostic_dir.mkdir(parents=True, exist_ok=True)
    homography, detections, raw_size, orientation = extract_geometry(
        args.dng, manifest, diagnostic_dir
    )
    profile, reconstruction = measure(
        args.dng, args.target, manifest, homography, raw_size, orientation
    )
    profile["source"] = {
        "file": args.dng.name,
        "sha256": file_sha256(args.dng),
        "raw_size": list(raw_size),
        "orientation": orientation,
    }
    profile["geometry"] = {
        "target_to_oriented_photo": homography.tolist(),
        "fiducials": detections,
    }
    profile_path = args.output_dir / "profile.json"
    profile_path.write_text(json.dumps(profile, indent=2) + "\n")
    reconstruction.save(args.output_dir / "measured-patches.png")

    print(f"profile: {profile_path}")
    print(f"geometry diagnostic: {diagnostic_dir / 'geometry.jpg'}")
    print(f"rectified photo: {diagnostic_dir / 'rectified.jpg'}")
    print(f"measured reconstruction: {args.output_dir / 'measured-patches.png'}")
    print("measured palette:")
    for entry in profile["palette"]:
        print(f"  {entry['name']:6s} {entry['measured']['srgb8']} "
              f"Lab={[round(value, 2) for value in entry['measured']['lab_d65']]}")
    quality = profile["quality"]
    print("pairwise linearity RMSE: "
          f"mean={quality['pairwise_mixture_rmse_camera_mean']:.5f}, "
          f"max={quality['pairwise_mixture_rmse_camera_max']:.5f}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, np.linalg.LinAlgError) as error:
        print(f"error: {error}")
        raise SystemExit(1)
