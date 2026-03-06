#!/usr/bin/env python3
"""
AV Auto-Sync: Generate test samples with known AV offsets using FFmpeg.

Usage:
    python generate_test_samples.py <source> <output_dir>
    python generate_test_samples.py -i <source> -o <output_dir> [--offsets 0,50,-50,300,-300]

<source> can be a single video file or a directory of video files.
"""

import argparse
import subprocess
import sys
from pathlib import Path

# Default offset values in milliseconds (positive = audio delayed / behind video)
DEFAULT_OFFSETS = [0, 50, -50, 300, -300, 1000, -1000, 2000, -2000]


def offset_to_label(offset_ms: int) -> str:
    """Convert an offset value to a filename-safe label.

    Convention:
      0     -> '0ms'
      50    -> 'pos_50ms'   (audio delayed behind video)
      -300  -> 'neg_300ms'  (audio ahead of video)
    """
    if offset_ms == 0:
        return "0ms"
    elif offset_ms > 0:
        return f"pos_{offset_ms}ms"
    else:
        return f"neg_{abs(offset_ms)}ms"


def generate_sample(source: Path, output_dir: Path, offset_ms: int) -> Path:
    """Generate a single test sample with the given AV offset."""
    src_stem = source.stem
    label = offset_to_label(offset_ms)
    out_path = output_dir / f"{src_stem}__{label}.mp4"

    if offset_ms == 0:
        # Baseline: just copy
        cmd = [
            "ffmpeg", "-y", "-i", str(source),
            "-c", "copy",
            str(out_path),
        ]
    else:
        # Apply offset: positive offset_ms means audio is delayed (shifted later)
        # itsoffset shifts the *second* input (audio source) by the given amount
        offset_sec = offset_ms / 1000.0
        cmd = [
            "ffmpeg", "-y",
            "-i", str(source),
            "-itsoffset", f"{offset_sec}",
            "-i", str(source),
            "-map", "0:v", "-map", "1:a",
            "-c", "copy",
            str(out_path),
        ]

    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return out_path


def collect_sources(path: Path) -> list[Path]:
    """Collect video source files from a path (file or directory)."""
    video_exts = {".mp4", ".mkv", ".avi", ".mov", ".webm", ".ts", ".flv"}
    if path.is_file():
        return [path]
    elif path.is_dir():
        files = sorted(p for p in path.iterdir() if p.suffix.lower() in video_exts)
        if not files:
            print(f"ERROR: No video files found in {path}", file=sys.stderr)
            sys.exit(1)
        return files
    else:
        print(f"ERROR: {path} does not exist", file=sys.stderr)
        sys.exit(1)


def parse_offsets(offset_str: str) -> list[int]:
    """Parse a comma-separated string of offset values."""
    return [int(x.strip()) for x in offset_str.split(",")]


def main():
    parser = argparse.ArgumentParser(
        description="Generate AV sync test samples with known offsets.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s -i video.mp4 -o ./test_samples
  %(prog)s -i ./source_videos/ -o ./test_samples
  %(prog)s -i video.mp4 -o ./test_samples --offsets 0,100,-100,500,-500
        """,
    )
    parser.add_argument("-i", "--input", required=True, help="Source video file or directory")
    parser.add_argument("-o", "--output", required=True, help="Output directory for test samples")
    parser.add_argument(
        "--offsets",
        default=",".join(str(o) for o in DEFAULT_OFFSETS),
        help=f"Comma-separated offset values in ms (default: {','.join(str(o) for o in DEFAULT_OFFSETS)})",
    )
    args = parser.parse_args()

    source_path = Path(args.input).resolve()
    output_dir = Path(args.output).resolve()
    offsets = parse_offsets(args.offsets)

    output_dir.mkdir(parents=True, exist_ok=True)

    sources = collect_sources(source_path)

    print("=" * 60)
    print(" AV Auto-Sync: Test Sample Generator")
    print("=" * 60)
    print(f"  Sources:  {len(sources)} file(s) from {source_path}")
    print(f"  Output:   {output_dir}")
    print(f"  Offsets:  {offsets}")
    print(f"  Total:    {len(sources) * len(offsets)} samples")
    print("=" * 60)
    print()

    generated = 0
    errors = 0

    for src in sources:
        print(f"Source: {src.name}")
        for offset in offsets:
            label = offset_to_label(offset)
            try:
                out = generate_sample(src, output_dir, offset)
                print(f"  [{label:>12s}] -> {out.name}")
                generated += 1
            except subprocess.CalledProcessError as e:
                print(f"  [{label:>12s}] FAILED: ffmpeg error (exit {e.returncode})")
                errors += 1
        print()

    print("=" * 60)
    print(f"  Generated: {generated}  |  Errors: {errors}")
    print(f"  Output:    {output_dir}")
    print("=" * 60)


if __name__ == "__main__":
    main()
