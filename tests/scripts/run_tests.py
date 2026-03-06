#!/usr/bin/env python3
"""
AV Auto-Sync: Run detection tests on generated test samples and produce reports.

Usage:
    python run_tests.py -i <test_samples_dir> [options]

Test sample filenames must follow the convention: {source}__{offset_label}.mp4
where offset_label is one of: 0ms, pos_50ms, neg_300ms, etc.
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

# ── Defaults ──────────────────────────────────────────────────────────────────

DEFAULT_TOLERANCE_MS = 40
DEFAULT_THRESHOLD_MS = 40
DEFAULT_WINDOW_SEC = 10.0
DEFAULT_STEP_SEC = 5.0
DEFAULT_TIMEOUT_SEC = 60
DEFAULT_DETECTORS = ["onset_align", "syncnet"]


# ── Data classes ──────────────────────────────────────────────────────────────


@dataclass
class TestResult:
    sample: str
    source: str
    offset_label: str
    injected_ms: int
    detector: str
    detected_ms: str  # may be "N/A" or "TIMEOUT"
    error_ms: str
    avg_confidence: str
    result: str  # PASS / FAIL / ERR
    notes: str


@dataclass
class DetectorStats:
    total: int = 0
    passed: int = 0
    failed: int = 0
    errors: int = 0

    @property
    def pass_rate(self) -> float:
        return (self.passed / self.total * 100) if self.total > 0 else 0.0


# ── Filename parsing ─────────────────────────────────────────────────────────


def parse_injection_from_filename(basename: str) -> int:
    """Extract injected offset (ms) from test sample filename.

    Expected pattern: {source}__{label}.mp4
    Labels: 0ms, pos_50ms, neg_300ms, etc.
    """
    # Extract the part after the last "__"
    parts = basename.rsplit("__", 1)
    if len(parts) != 2:
        return 0
    label = parts[1]

    m = re.match(r"(pos|neg)_(\d+)ms$", label)
    if m:
        sign = 1 if m.group(1) == "pos" else -1
        return sign * int(m.group(2))

    if re.match(r"0ms$", label):
        return 0

    return 0


# ── Output parsing ───────────────────────────────────────────────────────────


def parse_detection(output: str) -> str:
    """Parse detected offset from avsync stdout."""
    # Try global constant offset first
    m = re.search(r"Global constant offset.*?([-]?\d+\.?\d*)", output)
    if m:
        return m.group(1)

    # Fall back to averaging YES correction decisions
    corrections_block = re.split(r"Correction Decisions", output, maxsplit=1)
    if len(corrections_block) > 1:
        yes_offsets = re.findall(r"YES\s+([-]?\d+\.?\d*)", corrections_block[1])
        if yes_offsets:
            vals = [float(v) for v in yes_offsets]
            avg = sum(vals) / len(vals)
            return f"{avg:.1f}"

    # Check if explicitly reported no corrections
    if re.search(r"no corrections|0 segments corrected", output):
        return "0"

    return "N/A"


def parse_avg_confidence(output: str) -> str:
    """Extract average confidence from avsync output."""
    confs = re.findall(r"confidence=([\d.]+)", output)
    if confs:
        vals = [float(c) for c in confs]
        return f"{sum(vals) / len(vals):.2f}"
    return "0.00"


# ── Evaluation ───────────────────────────────────────────────────────────────


def evaluate(
    injected_ms: int, detected: str, tolerance: int, threshold: int
) -> tuple[str, str, str]:
    """Evaluate detection result.

    Returns (result, error_ms, notes) where result is PASS/FAIL/ERR.
    """
    if detected in ("N/A",):
        return "ERR", "N/A", "no output"
    if detected == "TIMEOUT":
        return "ERR", "N/A", "timeout"

    det_val = float(detected)
    abs_det = abs(det_val)
    abs_inj = abs(injected_ms)

    # Baseline (no offset injected)
    if injected_ms == 0:
        if abs_det < tolerance:
            return "PASS", "0", "baseline ok"
        else:
            return "FAIL", detected, "false positive"

    # Below threshold
    if abs_inj < threshold:
        if abs_det < threshold:
            return "PASS", "0", "below thresh ok"
        else:
            return "FAIL", detected, "below thresh but corrected"

    # Should have been corrected
    if det_val != 0:
        err = det_val - injected_ms
        if abs(err) <= tolerance:
            return "PASS", f"{err:.1f}", ""
        else:
            return "FAIL", f"{err:.1f}", f"err>±{tolerance}ms"
    else:
        return "FAIL", "N/A", "missed"


# ── Runner ───────────────────────────────────────────────────────────────────


def find_avsync_binary() -> str:
    """Locate the avsync binary."""
    # Check environment variable
    env_bin = os.environ.get("AVSYNC")
    if env_bin and Path(env_bin).is_file():
        return env_bin

    # Check relative to script location (project build dir)
    script_dir = Path(__file__).resolve().parent
    project_dir = script_dir.parent.parent
    candidates = [
        project_dir / "build" / "bin" / "avsync",
        project_dir / "build" / "avsync",
    ]
    for c in candidates:
        if c.is_file():
            return str(c)

    print("ERROR: Cannot find avsync binary.", file=sys.stderr)
    print("  Set AVSYNC environment variable or build the project first.", file=sys.stderr)
    sys.exit(1)


def run_detector(
    avsync_bin: str,
    input_file: Path,
    output_file: Path,
    detector: str,
    threshold: int,
    window: float,
    step: float,
    timeout: int,
) -> str:
    """Run avsync on a single file and return stdout+stderr."""
    cmd = [
        avsync_bin,
        "-i", str(input_file),
        "-o", str(output_file),
        "-m", "force",
        "-d", detector,
        "-v",
        "-t", str(threshold),
        "-w", str(window),
        "-s", str(step),
    ]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        return ""


def collect_test_samples(input_dir: Path) -> list[Path]:
    """Collect all test sample files matching the naming convention."""
    samples = sorted(input_dir.glob("*__*.mp4"))
    if not samples:
        print(f"ERROR: No test samples found in {input_dir}", file=sys.stderr)
        print("  Expected filenames like: source__pos_50ms.mp4", file=sys.stderr)
        sys.exit(1)
    return samples


# ── Report formatting ────────────────────────────────────────────────────────

COL_SAMPLE = 42
COL_NUM = 7


def fmt_header():
    return (
        f"{'Sample':<{COL_SAMPLE}} | {'Inject':>{COL_NUM}} | {'Detect':>{COL_NUM}} "
        f"| {'Error':>{COL_NUM}} | {'Conf':>5} | {'Result':>6} | Notes"
    )


def fmt_separator():
    return "-" * 110


def fmt_row(r: TestResult):
    return (
        f"{r.sample:<{COL_SAMPLE}} | {r.injected_ms:>{COL_NUM - 2}}ms "
        f"| {r.detected_ms:>{COL_NUM - 2}}ms | {r.error_ms:>{COL_NUM - 2}}ms "
        f"| {r.avg_confidence:>5} | {r.result:>6} | {r.notes}"
    )


# ── Main ─────────────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(
        description="Run AV sync detection tests and produce comparison reports.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s -i ./test_samples
  %(prog)s -i ./test_samples --detectors onset_align
  %(prog)s -i ./test_samples -o ./output --tolerance 50
        """,
    )
    parser.add_argument("-i", "--input", required=True, help="Directory containing test sample files")
    parser.add_argument("-o", "--output", default=None, help="Directory for corrected output files")
    parser.add_argument("-r", "--report-dir", default=None, help="Directory for reports")
    parser.add_argument("--detectors", nargs="+", default=DEFAULT_DETECTORS, help="Detectors to test")
    parser.add_argument("--tolerance", type=int, default=DEFAULT_TOLERANCE_MS, help="Tolerance in ms")
    parser.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD_MS, help="Threshold in ms")
    parser.add_argument("--window", type=float, default=DEFAULT_WINDOW_SEC, help="Segment window (sec)")
    parser.add_argument("--step", type=float, default=DEFAULT_STEP_SEC, help="Segment step (sec)")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT_SEC, help="Per-sample timeout (sec)")
    args = parser.parse_args()

    input_dir = Path(args.input).resolve()
    if not input_dir.is_dir():
        print(f"ERROR: {input_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    parent = input_dir.parent
    output_dir = Path(args.output).resolve() if args.output else parent / "test_output"
    report_dir = Path(args.report_dir).resolve() if args.report_dir else parent / "test_reports"

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    detail_dir = report_dir / f"details_{timestamp}"
    report_path = report_dir / f"report_{timestamp}.txt"
    csv_path = report_dir / f"report_{timestamp}.csv"

    for d in (output_dir, report_dir, detail_dir):
        d.mkdir(parents=True, exist_ok=True)

    avsync_bin = find_avsync_binary()
    samples = collect_test_samples(input_dir)

    # ── Header ────────────────────────────────────────────────────────────
    header_lines = [
        "=" * 70,
        " AV Auto-Sync: Detection Test Report",
        f" {datetime.now()}",
        f" Tolerance: ±{args.tolerance}ms | Threshold: {args.threshold}ms",
        f" Segment: {args.window}s window / {args.step}s step",
        f" Detectors: {', '.join(args.detectors)}",
        f" Input: {input_dir}",
        f" Timeout: {args.timeout}s per sample",
        f" Samples: {len(samples)}",
        "=" * 70,
        "",
    ]
    report_lines: list[str] = list(header_lines)
    for line in header_lines:
        print(line)

    # ── CSV setup ─────────────────────────────────────────────────────────
    csv_rows: list[dict] = []

    # ── Per-detector stats ────────────────────────────────────────────────
    stats: dict[str, DetectorStats] = {d: DetectorStats() for d in args.detectors}
    # Per-source per-detector: source -> detector -> DetectorStats
    source_stats: dict[str, dict[str, DetectorStats]] = {}

    # ── Run tests ─────────────────────────────────────────────────────────
    for det in args.detectors:
        section_lines = [
            "",
            "╔" + "═" * 58 + "╗",
            f"║  Detector: {det:<46s}║",
            "╚" + "═" * 58 + "╝",
            "",
            fmt_header(),
            fmt_separator(),
        ]
        for line in section_lines:
            print(line)
        report_lines.extend(section_lines)

        for sample in samples:
            basename = sample.stem
            source_name = basename.rsplit("__", 1)[0] if "__" in basename else basename
            injected = parse_injection_from_filename(basename)

            out_file = output_dir / f"{basename}_{det}.mp4"
            log_file = detail_dir / f"{basename}_{det}.log"

            # Run
            output = run_detector(
                avsync_bin, sample, out_file, det,
                args.threshold, args.window, args.step, args.timeout,
            )
            log_file.write_text(output)

            # Parse
            if not output:
                detected = "TIMEOUT"
                confidence = "0.00"
            else:
                detected = parse_detection(output)
                confidence = parse_avg_confidence(output)

            # Evaluate
            result, error_ms, notes = evaluate(
                injected, detected, args.tolerance, args.threshold
            )

            tr = TestResult(
                sample=basename,
                source=source_name,
                offset_label=basename.rsplit("__", 1)[-1] if "__" in basename else "",
                injected_ms=injected,
                detector=det,
                detected_ms=detected,
                error_ms=error_ms,
                avg_confidence=confidence,
                result=result,
                notes=notes,
            )

            row_str = fmt_row(tr)
            print(row_str)
            report_lines.append(row_str)

            csv_rows.append({
                "source": source_name,
                "offset_category": tr.offset_label,
                "injected_ms": injected,
                "detector": det,
                "detected_ms": detected,
                "error_ms": error_ms,
                "avg_confidence": confidence,
                "result": result,
                "notes": notes,
            })

            # Update stats
            ds = stats[det]
            ds.total += 1
            if result == "PASS":
                ds.passed += 1
            elif result == "FAIL":
                ds.failed += 1
            else:
                ds.errors += 1

            if source_name not in source_stats:
                source_stats[source_name] = {}
            if det not in source_stats[source_name]:
                source_stats[source_name][det] = DetectorStats()
            ss = source_stats[source_name][det]
            ss.total += 1
            if result == "PASS":
                ss.passed += 1
            elif result == "FAIL":
                ss.failed += 1
            else:
                ss.errors += 1

    # ── Summary ───────────────────────────────────────────────────────────
    summary_lines = [
        "",
        "",
        "╔" + "═" * 74 + "╗",
        "║" + "SUMMARY COMPARISON".center(74) + "║",
        "╚" + "═" * 74 + "╝",
        "",
        f"{'Detector':<15} | {'Total':>6} | {'Pass':>6} | {'Fail':>6} | {'Error':>6} | {'Pass Rate':>9}",
        "-" * 72,
    ]
    for det in args.detectors:
        ds = stats[det]
        summary_lines.append(
            f"{det:<15} | {ds.total:>6} | {ds.passed:>6} | {ds.failed:>6} "
            f"| {ds.errors:>6} | {ds.pass_rate:>8.1f}%"
        )

    summary_lines.extend([
        "",
        "--- Per-Source Breakdown ---",
        "",
    ])

    # Build per-source header
    src_header = f"{'Source':<12}"
    for det in args.detectors:
        src_header += f" | {det:>22}"
    summary_lines.append(src_header)
    summary_lines.append("-" * (15 + 25 * len(args.detectors)))

    for src in sorted(source_stats.keys()):
        row = f"{src:<12}"
        for det in args.detectors:
            ss = source_stats[src].get(det)
            if ss:
                row += f" | {ss.passed:>3}/{ss.total:<3} ({ss.pass_rate:>5.1f}%)"
            else:
                row += f" | {'N/A':>22}"
        summary_lines.append(row)

    summary_lines.extend([
        "",
        "=" * 60,
        f"Report: {report_path}",
        f"CSV:    {csv_path}",
        f"Logs:   {detail_dir}/",
        "=" * 60,
    ])

    for line in summary_lines:
        print(line)
    report_lines.extend(summary_lines)

    # ── Write files ───────────────────────────────────────────────────────
    report_path.write_text("\n".join(report_lines) + "\n")

    with open(csv_path, "w", newline="") as f:
        fieldnames = [
            "source", "offset_category", "injected_ms", "detector",
            "detected_ms", "error_ms", "avg_confidence", "result", "notes",
        ]
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(csv_rows)

    print(f"\nDone. {sum(s.total for s in stats.values())} tests executed.")


if __name__ == "__main__":
    main()
