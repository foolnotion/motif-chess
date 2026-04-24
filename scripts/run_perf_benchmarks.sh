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

Options:
  --mode MODE        quick | full | exhaustive (default: quick)
  --build-dir PATH   Build directory to use (default: build/dev)
  --skip-build       Do not build test binaries before running benchmarks
  --dry-run          Print commands without executing them
  -h, --help         Show this help text

Examples:
  scripts/run_perf_benchmarks.sh
  scripts/run_perf_benchmarks.sh --mode full
  scripts/run_perf_benchmarks.sh --mode exhaustive --build-dir build/release
EOF
}

build_dir='build/dev'
mode='quick'
skip_build='false'
dry_run='false'

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

if [[ ${mode} != 'quick' && ${mode} != 'full' && ${mode} != 'exhaustive' ]]; then
    printf '--mode must be quick, full, or exhaustive\n' >&2
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

if [[ ${mode} == 'quick' ]]; then
    ensure_quick_pgn
    corpus_path="${repo_root}/${quick_pgn}"
else
    ensure_one_million_pgn
    corpus_path="${repo_root}/${one_million_pgn}"
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
