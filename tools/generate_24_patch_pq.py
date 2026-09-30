#!/usr/bin/env python3
"""Generate a 24-patch HDR PQ test-pattern MP4 with FFmpeg.

The default pq-code mode maps the original 8-bit reference values directly to
10-bit PQ code values. The optional absolute-nits mode interprets them as
linear-light fractions of --peak-nits before SMPTE ST 2084 encoding.
"""

from __future__ import annotations

import argparse
import math
import shutil
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Patch:
    name: str
    rgb: tuple[int, int, int]


PATCHES: tuple[Patch, ...] = (
    Patch("Black", (0, 0, 0)),
    Patch("Gray 10%", (26, 26, 26)),
    Patch("Gray 25%", (64, 64, 64)),
    Patch("Gray 50%", (128, 128, 128)),
    Patch("Gray 75%", (192, 192, 192)),
    Patch("Gray 90%", (230, 230, 230)),
    Patch("White", (255, 255, 255)),
    Patch("Red", (255, 0, 0)),
    Patch("Green", (0, 255, 0)),
    Patch("Blue", (0, 0, 255)),
    Patch("Cyan", (0, 255, 255)),
    Patch("Magenta", (255, 0, 255)),
    Patch("Yellow", (255, 255, 0)),
    Patch("Mid Red", (128, 0, 0)),
    Patch("Mid Green", (0, 128, 0)),
    Patch("Mid Blue", (0, 0, 128)),
    Patch("Mid Cyan", (0, 128, 128)),
    Patch("Mid Magenta", (128, 0, 128)),
    Patch("Mid Yellow", (128, 128, 0)),
    Patch("Low Red", (64, 0, 0)),
    Patch("Low Green", (0, 64, 0)),
    Patch("Low Blue", (0, 0, 64)),
    Patch("Gray 40%", (102, 102, 102)),
    Patch("Gray 60%", (153, 153, 153)),
)

ROWS = 4
COLUMNS = 6
PQ_M1 = 2610.0 / 16384.0
PQ_M2 = 2523.0 / 32.0
PQ_C1 = 3424.0 / 4096.0
PQ_C2 = 2413.0 / 128.0
PQ_C3 = 2392.0 / 128.0


def parse_size(value: str) -> tuple[int, int]:
    try:
        width_text, height_text = value.lower().split("x", 1)
        width, height = int(width_text), int(height_text)
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError("size must be WIDTHxHEIGHT") from error
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("width and height must be positive")
    return width, height


def pq_oetf(nits: float) -> float:
    """Convert absolute luminance in cd/m² to normalized ST 2084 code."""
    normalized = min(max(nits / 10000.0, 0.0), 1.0)
    power = normalized ** PQ_M1
    return ((PQ_C1 + PQ_C2 * power) / (1.0 + PQ_C3 * power)) ** PQ_M2


def pq_eotf(code: float) -> float:
    """Convert normalized ST 2084 code to absolute luminance in cd/m²."""
    encoded = min(max(code, 0.0), 1.0)
    power = encoded ** (1.0 / PQ_M2)
    numerator = max(power - PQ_C1, 0.0)
    denominator = max(PQ_C2 - PQ_C3 * power, 1.0e-12)
    return 10000.0 * (numerator / denominator) ** (1.0 / PQ_M1)


def code10_to_u16(code: int) -> int:
    return round(min(max(code, 0), 1023) * 65535 / 1023)


def patch_codes(patch: Patch, mode: str, peak_nits: float) -> tuple[int, int, int]:
    if mode == "pq-code":
        return tuple(round(component * 1023 / 255) for component in patch.rgb)
    return tuple(round(pq_oetf(component / 255.0 * peak_nits) * 1023)
                 for component in patch.rgb)


def write_rgb48_ppm(
    path: Path, width: int, height: int, mode: str, peak_nits: float
) -> None:
    codes = [patch_codes(patch, mode, peak_nits) for patch in PATCHES]
    with path.open("wb") as output:
        output.write(f"P6\n{width} {height}\n65535\n".encode("ascii"))
        for y in range(height):
            row = min(y * ROWS // height, ROWS - 1)
            scanline = bytearray()
            for x in range(width):
                column = min(x * COLUMNS // width, COLUMNS - 1)
                rgb16 = tuple(code10_to_u16(value) for value in codes[row * COLUMNS + column])
                scanline.extend(struct.pack(">HHH", *rgb16))
            output.write(scanline)


def patch_component_nits(component: int, mode: str, peak_nits: float) -> float:
    if mode == "pq-code":
        return pq_eotf(component / 255.0)
    return component / 255.0 * peak_nits


def max_fall_nits(mode: str, peak_nits: float) -> int:
    # BT.2020 constant-luminance estimate used only for HDR10 MaxFALL metadata.
    kr, kg, kb = 0.2627, 0.6780, 0.0593
    average = sum(
        kr * patch_component_nits(patch.rgb[0], mode, peak_nits)
        + kg * patch_component_nits(patch.rgb[1], mode, peak_nits)
        + kb * patch_component_nits(patch.rgb[2], mode, peak_nits)
        for patch in PATCHES
    ) / len(PATCHES)
    return max(1, round(average))


def find_ffmpeg(requested: str) -> str:
    resolved = shutil.which(requested)
    if resolved is None:
        raise FileNotFoundError(
            f"FFmpeg was not found: {requested!r}. Install FFmpeg or pass --ffmpeg PATH."
        )
    return resolved


def encode_mp4(
    ffmpeg: str,
    source: Path,
    output: Path,
    width: int,
    height: int,
    fps: int,
    duration: float,
    mode: str,
    peak_nits: float,
    crf: int,
) -> None:
    content_peak_nits = 10000.0 if mode == "pq-code" else peak_nits
    mastering_peak = round(content_peak_nits * 10000)
    fall = max_fall_nits(mode, peak_nits)
    x265_params = (
        "hdr10=1:repeat-headers=1:aud=1:"
        "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:"
        f"master-display=G(13250,34500)B(7500,3000)R(34000,16000)"
        f"WP(15635,16450)L({mastering_peak},1):"
        f"max-cll={round(content_peak_nits)},{fall}"
    )
    video_filter = (
        "zscale=matrixin=gbr:transferin=smpte2084:primariesin=bt2020:rangein=full:"
        "matrix=bt2020nc:transfer=smpte2084:primaries=bt2020:range=limited,"
        "format=yuv420p10le"
    )
    command = [
        ffmpeg,
        "-hide_banner",
        "-y",
        "-loop", "1",
        "-framerate", str(fps),
        "-i", str(source),
        "-t", str(duration),
        "-vf", video_filter,
        "-an",
        "-c:v", "libx265",
        "-preset", "slow",
        "-crf", str(crf),
        "-pix_fmt", "yuv420p10le",
        "-tag:v", "hvc1",
        "-color_primaries", "bt2020",
        "-color_trc", "smpte2084",
        "-colorspace", "bt2020nc",
        "-color_range", "tv",
        "-x265-params", x265_params,
        "-movflags", "+faststart",
        str(output),
    ]
    print("Running:", subprocess.list2cmdline(command))
    subprocess.run(command, check=True)


def print_patch_table(mode: str, peak_nits: float) -> None:
    print("Patch PQ values (10-bit full-range RGB code before YCbCr encoding):")
    for index, patch in enumerate(PATCHES):
        nits = tuple(round(patch_component_nits(component, mode, peak_nits), 3)
                     for component in patch.rgb)
        print(
            f"{index:02d} {patch.name:12s} nits={nits!s:28s} "
            f"PQ10={patch_codes(patch, mode, peak_nits)}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate a BT.2020/ST 2084 24-patch 10-bit HEVC MP4."
    )
    parser.add_argument("--output", type=Path, help="output MP4 path")
    parser.add_argument("--size", type=parse_size, default=(1280, 720), metavar="WIDTHxHEIGHT")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--duration", type=float, default=60.0, help="video duration in seconds")
    parser.add_argument(
        "--mode",
        choices=("pq-code", "absolute-nits"),
        default="pq-code",
        help="pq-code is for capture calibration; absolute-nits is for preview testing",
    )
    parser.add_argument(
        "--peak-nits", type=float, default=1000.0,
        help="white luminance used only by --mode absolute-nits (default: 1000)",
    )
    parser.add_argument("--crf", type=int, default=10, help="x265 CRF (default: 10)")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg executable or path")
    parser.add_argument("--keep-ppm", type=Path, help="also retain the generated 16-bit RGB PPM")
    parser.add_argument("--print-values", action="store_true", help="print patch nits and PQ codes")
    args = parser.parse_args()

    if args.fps <= 0 or args.duration <= 0:
        parser.error("--fps and --duration must be positive")
    if not math.isfinite(args.peak_nits) or not 0 < args.peak_nits <= 10000:
        parser.error("--peak-nits must be in the range 0 < value <= 10000")
    if args.crf not in range(0, 52):
        parser.error("--crf must be between 0 and 51")

    if args.print_values:
        print_patch_table(args.mode, args.peak_nits)

    ffmpeg = find_ffmpeg(args.ffmpeg)
    output = args.output or Path(
        "24_patch_pq_code.mp4" if args.mode == "pq-code"
        else f"24_patch_pq_{args.peak_nits:g}nit.mp4"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="capture-calibration-pq-") as temporary:
        ppm = Path(temporary) / "24_patch_pq_rgb48.ppm"
        write_rgb48_ppm(ppm, args.size[0], args.size[1], args.mode, args.peak_nits)
        if args.keep_ppm is not None:
            args.keep_ppm.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ppm, args.keep_ppm)
        encode_mp4(
            ffmpeg, ppm, output, args.size[0], args.size[1],
            args.fps, args.duration, args.mode, args.peak_nits, args.crf
        )
    print(f"Created: {output.resolve()}")


if __name__ == "__main__":
    main()
