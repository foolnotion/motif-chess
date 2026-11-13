#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run_perf_benchmarks.sh [options]

Run motif-chess performance tests against a selected benchmark corpus.

Modes:
  quick              Uses a 100k-game corpus and runs the production-path perf guard
  full               Uses a 1M-game corpus and runs the same perf guard on the full dataset
  exhaustive         Uses a 1M-game corpus and runs all existing performance-tagged tests
  bench-quick        Benchmark suite (motif_bench_test) on the 100k corpus via scripts/bench_compare.py
  bench-full         Benchmark suite on the 1M corpus (slow: every scenario rebuilds the 1M bundle indexes)

Options:
  --mode MODE        quick | full | exhaustive | bench-quick | bench-full (default: quick)
  --build-dir PATH   Build directory to use (default: build/dev; bench modes need Release)
  --skip-build       Do not build test binaries before running benchmarks
  --dry-run          Print commands without executing them
  --repeat N         bench modes: repetitions per scenario (default: 3)
  --out FILE         bench modes: result JSON (default: <build-dir>/bench-<tier>.json)
  --compare FILE     bench modes: afterwards compare against this baseline JSON
                     (exit non-zero on regression); baselines live in docs/bench-baselines/
  -h, --help         Show this help text

Examples:
  scripts/run_perf_benchmarks.sh
  scripts/run_perf_benchmarks.sh --mode full
  scripts/run_perf_benchmarks.sh --mode exhaustive --build-dir build/release
  scripts/run_perf_benchmarks.sh --mode bench-quick --build-dir build/release \
      --compare docs/bench-baselines/quick-twic-100k.json
EOF
}

build_dir='build/dev'
mode='quick'
skip_build='false'
dry_run='false'
bench_repeat='3'
bench_out=''
bench_baseline=''

while (($# > 0)); do
    case "$1" in
        --mode)
            mode=${2:-}
            shift 2
            ;;
        --build-dir)
            build_dir=${2:-}
            shift 2
            ;;
        --skip-build)
            skip_build='true'
            shift
            ;;
        --dry-run)
            dry_run='true'
            shift
            ;;
        --repeat)
            bench_repeat=${2:-}
            shift 2
            ;;
        --out)
            bench_out=${2:-}
            shift 2
            ;;
        --compare)
            bench_baseline=${2:-}
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'unknown option: %s\n\n' "$1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ ${mode} != 'quick' && ${mode} != 'full' && ${mode} != 'exhaustive' && ${mode} != 'bench-quick' && ${mode} != 'bench-full' ]]; then
    printf '--mode must be quick, full, exhaustive, bench-quick, or bench-full\n' >&2
    exit 1
fi

repo_root=$(pwd)
benchmark_dir='bench/data'
one_million_pgn="${benchmark_dir}/twic-1m.pgn"
quick_pgn="${benchmark_dir}/twic-100k.pgn"

run() {
    printf '+ %s\n' "$*"
    if [[ ${dry_run} == 'false' ]]; then
        "$@"
    fi
}

ensure_one_million_pgn() {
    if [[ -f ${one_million_pgn} ]]; then
        return
    fi

    run bash scripts/download_twic_pgns.sh --1m
}

ensure_quick_pgn() {
    if [[ -f ${quick_pgn} ]]; then
        return
    fi

    ensure_one_million_pgn
    run bash scripts/extract_pgn_subset.sh
}

if [[ ${mode} == 'quick' || ${mode} == 'bench-quick' ]]; then
    ensure_quick_pgn
    corpus_path="${repo_root}/${quick_pgn}"
else
    ensure_one_million_pgn
    corpus_path="${repo_root}/${one_million_pgn}"
fi

# Benchmark suite: scenario binaries run one process each via bench_compare.py.
if [[ ${mode} == bench-* ]]; then
    tier=${mode#bench-}
    if [[ ${skip_build} == 'false' ]]; then
        run cmake --build "${build_dir}" --target motif_bench_test
    fi
    bench_out=${bench_out:-${build_dir}/bench-${tier}.json}
    bench_script="$(dirname "$0")/bench_compare.py"
    run python3 "${bench_script}" run --build-dir "${build_dir}" --tier "${tier}" \
        --repeat "${bench_repeat}" --out "${bench_out}"
    if [[ -n ${bench_baseline} ]]; then
        run python3 "${bench_script}" compare "${bench_baseline}" "${bench_out}"
    fi
    exit 0
fi

if [[ ${skip_build} == 'false' ]]; then
    run cmake --build "${build_dir}" --target motif_import_test motif_search_test
fi

import_test_bin="${build_dir}/test/motif_import_test"
search_test_bin="${build_dir}/test/motif_search_test"

if [[ ! -x ${import_test_bin} || ! -x ${search_test_bin} ]]; then
    printf 'expected test binaries are missing under %s\n' "${build_dir}" >&2
    exit 1
fi

if [[ ${mode} == 'exhaustive' ]]; then
    import_filter='[performance]'
else
    import_filter='import_pipeline: default fast path perf'
fi

run env MOTIF_IMPORT_PERF_PGN="${corpus_path}" \
    "${import_test_bin}" "${import_filter}" --reporter console

run env MOTIF_IMPORT_PERF_PGN="${corpus_path}" \
    "${search_test_bin}" "[performance]" --reporter console
