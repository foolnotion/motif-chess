#!/usr/bin/env python3
"""Run the motif-chess benchmark suite and compare two result files.

  bench_compare.py run --build-dir D --tier quick|full --repeat N --out FILE.json
  bench_compare.py compare BASE.json NEW.json [--threshold PCT]

`run` executes every scenario of the motif_bench_test binary (one test case,
hence one process, per scenario and repetition), parses its
`BENCH <scenario>.<metric>=<number>` lines and stores the per-metric
median/min/max plus run metadata. `compare` prints a delta table and exits
non-zero when any metric regressed beyond its tolerance.

Python 3 standard library only. Nothing here modifies the repository.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SCHEMA_VERSION = 1

# Scenario name -> Catch2 test-case name. Order matters: `import` seeds the
# shared reference-bundle cache that the other scenarios copy or open.
SCENARIOS = {
    "import": "bench: import",
    "open": "bench: open",
    "insert_single": "bench: insert_single",
    "mutate": "bench: mutate",
    "multi_open": "bench: multi_open",
    "segments": "bench: segments",
    "patch_chain": "bench: patch_chain",
}

CORPORA = {"quick": "twic-100k.pgn", "full": "twic-1m.pgn"}

BENCH_LINE = re.compile(r"^BENCH (\S+?)=(\S+)\s*$")

RELEASE_BUILD_TYPES = {"Release", "RelWithDebInfo"}

DEFAULT_TIME_THRESHOLD = 10.0  # _ms, _us, _per_s, _ratio
DEFAULT_BYTES_THRESHOLD = 5.0  # _bytes
DEFAULT_RSS_THRESHOLD = 15.0  # peak RSS and other RSS figures (_mib)

# Changes smaller than this (in the metric's own unit) are never regressions,
# so sub-microsecond jitter on tiny metrics cannot trip the percentage test.
ABSOLUTE_FLOOR = {"time_ms": 0.2, "time_us": 2.0, "throughput": 0.0, "ratio": 0.02, "bytes": 0.0, "rss": 1.0}


# --------------------------------------------------------------------------
# run
# --------------------------------------------------------------------------


def repo_root() -> Path:
    """motif-chess/ (the directory holding scripts/ and bench/)."""
    return Path(__file__).resolve().parent.parent


def read_cmake_cache(build_dir: Path) -> dict[str, str]:
    cache: dict[str, str] = {}
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.is_file():
        return cache
    for line in cache_file.read_text(errors="replace").splitlines():
        match = re.match(r"^([A-Za-z0-9_\-]+):[A-Z]+=(.*)$", line)
        if match:
            cache[match.group(1)] = match.group(2)
    return cache


def run_text(command: list[str], cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    except OSError:
        return "unknown"
    out = completed.stdout.strip()
    return out.splitlines()[0] if completed.returncode == 0 and out else "unknown"


def find_binary(build_dir: Path) -> Path:
    for candidate in (build_dir / "bin" / "motif_bench_test", build_dir / "test" / "motif_bench_test"):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    sys.exit(f"error: motif_bench_test not found under {build_dir} (build target motif_bench_test)")


def collect_metadata(build_dir: Path, cache: dict[str, str], args: argparse.Namespace, corpus: Path) -> dict:
    root = repo_root()
    compiler = cache.get("CMAKE_CXX_COMPILER", "unknown")
    return {
        "git_describe": run_text(["git", "describe", "--always", "--dirty", "--tags"], cwd=root),
        "git_commit": run_text(["git", "rev-parse", "HEAD"], cwd=root),
        "compiler": compiler,
        "compiler_version": run_text([compiler, "--version"]) if compiler != "unknown" else "unknown",
        "cxx_flags": cache.get("CMAKE_CXX_FLAGS", ""),
        "build_type": cache.get("CMAKE_BUILD_TYPE", "unknown"),
        "x86_64_v3": cache.get("motif_ENABLE_X86_64_V3", "unknown"),
        "nproc": os.cpu_count(),
        "hostname": socket.gethostname(),
        "corpus": corpus.name,
        "tier": args.tier,
        "repeat": args.repeat,
        "timestamp": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "build_dir": str(build_dir),
    }


def run_scenario(binary: Path, name: str, env: dict[str, str], verbose: bool) -> tuple[dict[str, float], float]:
    """Runs one scenario process; returns its metrics and wall seconds."""
    started = time.monotonic()
    process = subprocess.Popen(
        [str(binary), SCENARIOS[name]],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    assert process.stdout is not None
    metrics: dict[str, float] = {}
    output_tail: list[str] = []
    for line in process.stdout:
        match = BENCH_LINE.match(line)
        if match:
            try:
                metrics[match.group(1)] = float(match.group(2))
            except ValueError:
                sys.exit(f"error: non-numeric BENCH value in scenario {name}: {line.strip()}")
            if verbose:
                print("   ", line.rstrip(), file=sys.stderr)
        else:
            output_tail.append(line.rstrip())
            output_tail = output_tail[-40:]
    returncode = process.wait()
    wall = time.monotonic() - started
    if returncode != 0:
        print("\n".join(output_tail), file=sys.stderr)
        sys.exit(f"error: scenario {name} failed (exit {returncode})")
    if not metrics:
        print("\n".join(output_tail), file=sys.stderr)
        sys.exit(
            f"error: scenario {name} printed no BENCH lines (skipped? needs a Release build and the corpus)"
        )
    return metrics, wall


def summarize(samples: list[float]) -> dict:
    return {
        "median": statistics.median(samples),
        "min": min(samples),
        "max": max(samples),
        "n": len(samples),
    }


def command_run(args: argparse.Namespace) -> int:
    build_dir = Path(args.build_dir).resolve()
    binary = find_binary(build_dir)
    cache = read_cmake_cache(build_dir)
    build_type = cache.get("CMAKE_BUILD_TYPE", "unknown")
    if build_type not in RELEASE_BUILD_TYPES:
        print(f"warning: build type is {build_type!r}; benchmarks skip or mislead outside Release/RelWithDebInfo", file=sys.stderr)

    corpus = Path(args.corpus) if args.corpus else repo_root() / "bench" / "data" / CORPORA[args.tier]
    if not corpus.is_file():
        sys.exit(f"error: corpus not found: {corpus} (see scripts/download_twic_pgns.sh and scripts/extract_pgn_subset.sh)")

    names = list(SCENARIOS) if not args.scenarios else args.scenarios.split(",")
    unknown = [name for name in names if name not in SCENARIOS]
    if unknown:
        sys.exit(f"error: unknown scenario(s) {unknown}; choose from {list(SCENARIOS)}")
    # `import` first so its bundle seeds the cache for the others.
    names.sort(key=list(SCENARIOS).index)

    cache_dir = Path(args.cache_dir) if args.cache_dir else Path(tempfile.mkdtemp(prefix="motif-bench-cache-"))
    cache_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, MOTIF_BENCH_PGN=str(corpus), MOTIF_BENCH_CACHE=str(cache_dir))

    samples: dict[str, list[float]] = {}
    scenario_wall: dict[str, list[float]] = {name: [] for name in names}
    suite_started = time.monotonic()
    try:
        for repetition in range(1, args.repeat + 1):
            for name in names:
                print(f"[{repetition}/{args.repeat}] {name} ...", file=sys.stderr, flush=True)
                metrics, wall = run_scenario(binary, name, env, args.verbose)
                scenario_wall[name].append(round(wall, 2))
                print(f"[{repetition}/{args.repeat}] {name}: {len(metrics)} metrics in {wall:.1f}s", file=sys.stderr, flush=True)
                for key, value in metrics.items():
                    samples.setdefault(key, []).append(value)
    finally:
        if not args.cache_dir and not args.keep_cache:
            shutil.rmtree(cache_dir, ignore_errors=True)
    total_wall = time.monotonic() - suite_started

    meta = collect_metadata(build_dir, cache, args, corpus)
    meta["scenarios"] = names
    meta["scenario_wall_s"] = scenario_wall
    meta["suite_wall_s"] = round(total_wall, 1)
    result = {
        "schema": SCHEMA_VERSION,
        "meta": meta,
        "metrics": {key: summarize(values) for key, values in sorted(samples.items())},
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2, sort_keys=False) + "\n")
    print(f"wrote {out} ({len(result['metrics'])} metrics, {args.repeat} repetition(s), {total_wall:.0f}s)", file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# compare
# --------------------------------------------------------------------------


class MetricClass:
    """How a metric is judged: direction, tolerance and absolute floor."""

    def __init__(self, kind: str, higher_is_better: bool, threshold: float | None, floor_key: str | None) -> None:
        self.kind = kind
        self.higher_is_better = higher_is_better
        self.threshold = threshold
        self.floor_key = floor_key


def classify(metric: str, args: argparse.Namespace) -> MetricClass:
    leaf = metric.split(".")[-1]
    # Standalone single-game insert latency is not a gated quantity (edits are
    # infrequent); the batch path (batch_*) stays gated.
    if metric.startswith("insert_single.single_"):
        return MetricClass("info", False, None, None)
    if leaf.endswith("incremental"):
        return MetricClass("flag", True, 0.0, None)
    if leaf.endswith("_per_s"):
        return MetricClass("throughput", True, args.threshold, "throughput")
    if leaf.endswith("_ms"):
        return MetricClass("time", False, args.threshold, "time_ms")
    if leaf.endswith("_us"):
        return MetricClass("time", False, args.threshold, "time_us")
    if leaf.endswith("_ratio"):
        return MetricClass("ratio", False, args.threshold, "ratio")
    if "rss" in leaf:
        return MetricClass("rss", False, args.rss_threshold, "rss")
    if leaf.endswith("_bytes"):
        return MetricClass("bytes", False, args.bytes_threshold, "bytes")
    if leaf.endswith("_mib"):
        return MetricClass("size", False, args.bytes_threshold, "rss")
    return MetricClass("info", False, None, None)


def percent_change(base: float, new: float) -> float:
    if base == 0:
        return 0.0 if new == 0 else math.inf if new > 0 else -math.inf
    return (new - base) / abs(base) * 100.0


def spread_percent(entry: dict) -> float:
    median = entry["median"]
    span = entry["max"] - entry["min"]
    if span == 0:
        return 0.0
    return math.inf if median == 0 else span / abs(median) * 100.0


def judge(base: dict, new: dict, cls: MetricClass) -> tuple[str, float]:
    """Returns (status, delta percent)."""
    delta = percent_change(base["median"], new["median"])
    if cls.kind == "info":
        return "info", delta
    absolute = new["median"] - base["median"]
    if cls.kind == "flag":
        if absolute < 0:
            return "REGRESSION", delta
        return ("improved" if absolute > 0 else "ok"), delta
    assert cls.threshold is not None
    floor = ABSOLUTE_FLOOR[cls.floor_key] if cls.floor_key else 0.0
    worse = delta < -cls.threshold if cls.higher_is_better else delta > cls.threshold
    better = delta > cls.threshold if cls.higher_is_better else delta < -cls.threshold
    if abs(absolute) < floor or not (worse or better):
        return "ok", delta
    if spread_percent(base) > cls.threshold:
        return "noisy", delta
    return ("REGRESSION" if worse else "improved"), delta


def fmt_number(value: float) -> str:
    return f"{value:.4g}" if abs(value) < 1e6 else f"{value:.6g}"


def fmt_delta(delta: float) -> str:
    if math.isinf(delta):
        return "+inf%" if delta > 0 else "-inf%"
    return f"{delta:+.1f}%"


def load_result(path: str) -> dict:
    try:
        data = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as error:
        sys.exit(f"error: cannot read {path}: {error}")
    if data.get("schema") != SCHEMA_VERSION or "metrics" not in data or "meta" not in data:
        sys.exit(f"error: {path} is not a bench_compare result (schema {SCHEMA_VERSION})")
    return data


def command_compare(args: argparse.Namespace) -> int:
    base = load_result(args.base)
    new = load_result(args.new)

    for label, data, path in (("base", base, args.base), ("new", new, args.new)):
        build_type = data["meta"].get("build_type")
        if build_type not in RELEASE_BUILD_TYPES:
            print(
                f"error: refusing to compare: {label} ({path}) was built as {build_type!r}; "
                "need Release or RelWithDebInfo",
                file=sys.stderr,
            )
            return 2
    if base["meta"].get("corpus") != new["meta"].get("corpus") and not args.allow_corpus_mismatch:
        print(
            f"error: refusing to compare different corpora ({base['meta'].get('corpus')} vs {new['meta'].get('corpus')}); "
            "use --allow-corpus-mismatch to override",
            file=sys.stderr,
        )
        return 2

    print(f"base: {base['meta'].get('git_describe')}  {base['meta'].get('timestamp')}  {base['meta'].get('corpus')}  "
          f"{base['meta'].get('build_type')}  repeat={base['meta'].get('repeat')}")
    print(f"new:  {new['meta'].get('git_describe')}  {new['meta'].get('timestamp')}  {new['meta'].get('corpus')}  "
          f"{new['meta'].get('build_type')}  repeat={new['meta'].get('repeat')}")
    if base["meta"].get("hostname") != new["meta"].get("hostname"):
        print("warning: results come from different hosts; timings are not comparable", file=sys.stderr)

    rows: list[tuple[str, str, str, str, str]] = []
    counts = {"ok": 0, "REGRESSION": 0, "improved": 0, "noisy": 0, "info": 0, "added": 0, "removed": 0}
    base_metrics = base["metrics"]
    new_metrics = new["metrics"]
    for metric in sorted(set(base_metrics) | set(new_metrics)):
        if metric not in new_metrics:
            counts["removed"] += 1
            rows.append((metric, fmt_number(base_metrics[metric]["median"]), "-", "", "removed"))
            continue
        if metric not in base_metrics:
            counts["added"] += 1
            rows.append((metric, "-", fmt_number(new_metrics[metric]["median"]), "", "added"))
            continue
        status, delta = judge(base_metrics[metric], new_metrics[metric], classify(metric, args))
        counts[status] += 1
        rows.append(
            (
                metric,
                fmt_number(base_metrics[metric]["median"]),
                fmt_number(new_metrics[metric]["median"]),
                fmt_delta(delta),
                status,
            )
        )

    shown = [row for row in rows if not args.only_changes or row[4] not in ("ok", "info")]
    headers = ("metric", "base", "new", "delta", "status")
    widths = [max(len(headers[i]), *(len(row[i]) for row in shown)) if shown else len(headers[i]) for i in range(5)]
    print()
    print("  ".join(headers[i].ljust(widths[i]) if i in (0, 4) else headers[i].rjust(widths[i]) for i in range(5)))
    for row in shown:
        print("  ".join(row[i].ljust(widths[i]) if i in (0, 4) else row[i].rjust(widths[i]) for i in range(5)))
    print()
    print(", ".join(f"{counts[key]} {key}" for key in counts))
    if counts["REGRESSION"]:
        print("FAIL: regressions detected")
        return 1
    print("PASS")
    return 0


# --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser("run", help="run the scenarios and write a result file")
    run.add_argument("--build-dir", required=True, help="Release build tree containing motif_bench_test")
    run.add_argument("--tier", choices=sorted(CORPORA), default="quick", help="quick = 100k corpus, full = 1M corpus")
    run.add_argument("--repeat", type=int, default=3, help="repetitions of every scenario (default 3)")
    run.add_argument("--out", required=True, help="result JSON to write")
    run.add_argument("--scenarios", help=f"comma-separated subset of {','.join(SCENARIOS)}")
    run.add_argument("--corpus", help="PGN override (default: bench/data for the tier)")
    run.add_argument("--cache-dir", help="reuse this reference-bundle cache; only valid for one build of the code")
    run.add_argument("--keep-cache", action="store_true", help="keep the temporary cache directory")
    run.add_argument("--verbose", action="store_true", help="echo BENCH lines to stderr as they arrive")
    run.set_defaults(handler=command_run)

    compare = commands.add_parser("compare", help="compare two result files; exit 1 on regression")
    compare.add_argument("base")
    compare.add_argument("new")
    compare.add_argument("--threshold", type=float, default=DEFAULT_TIME_THRESHOLD, metavar="PCT",
                         help=f"tolerance for _ms/_us/_per_s/_ratio metrics (default {DEFAULT_TIME_THRESHOLD:g})")
    compare.add_argument("--bytes-threshold", type=float, default=DEFAULT_BYTES_THRESHOLD, metavar="PCT",
                         help=f"tolerance for _bytes metrics (default {DEFAULT_BYTES_THRESHOLD:g})")
    compare.add_argument("--rss-threshold", type=float, default=DEFAULT_RSS_THRESHOLD, metavar="PCT",
                         help=f"tolerance for peak RSS and other RSS figures (default {DEFAULT_RSS_THRESHOLD:g})")
    compare.add_argument("--only-changes", action="store_true", help="hide ok/info rows")
    compare.add_argument("--allow-corpus-mismatch", action="store_true")
    compare.set_defaults(handler=command_compare)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "run" and args.repeat < 1:
        sys.exit("error: --repeat must be >= 1")
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
