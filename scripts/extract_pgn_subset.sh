#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/extract_pgn_subset.sh [options]

Create a smaller PGN corpus by copying the first N games from a larger PGN.

Options:
  --input PATH        Source PGN path (default: bench/data/twic-1m.pgn)
  --output PATH       Output PGN path (default: bench/data/twic-100k.pgn)
  --games COUNT       Number of games to keep (default: 100000)
  -h, --help          Show this help text

Example:
  scripts/extract_pgn_subset.sh
  scripts/extract_pgn_subset.sh --games 50000 --output bench/data/twic-50k.pgn
EOF
}

input_path='bench/data/twic-1m.pgn'
output_path='bench/data/twic-100k.pgn'
game_count='100000'

while (($# > 0)); do
    case "$1" in
        --input)
            input_path=${2:-}
            shift 2
            ;;
        --output)
            output_path=${2:-}
            shift 2
            ;;
        --games)
            game_count=${2:-}
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

if ! [[ ${game_count} =~ ^[0-9]+$ ]] || (( game_count <= 0 )); then
    printf '--games must be a positive integer\n' >&2
    exit 1
fi

if [[ ! -f ${input_path} ]]; then
    printf 'input PGN not found: %s\n' "${input_path}" >&2
    exit 1
fi

mkdir -p "$(dirname "${output_path}")"

tmp_output="${output_path}.tmp"
rm -f "${tmp_output}"

awk -v max_games="${game_count}" '
    /^\[Event / {
        if (games >= max_games) {
            exit
        }
        ++games
    }

    games > 0 {
        print
    }
' "${input_path}" > "${tmp_output}"

mv "${tmp_output}" "${output_path}"
printf 'wrote %s from %s (%s games)\n' \
    "${output_path}" "${input_path}" "${game_count}"
