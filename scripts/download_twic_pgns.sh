#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/download_twic_pgns.sh [options]

Download one or more TWIC PGN zip archives and concatenate them into a single
benchmark corpus.

Options:
  --1m                Download a rolling corpus of roughly 1M games using the
                      most recent TWIC issues listed in the TWIC archive
  --all               Download every TWIC issue listed in the TWIC archive
  --first ISSUE       First TWIC issue number to fetch (inclusive)
  --last ISSUE        Last TWIC issue number to fetch (inclusive)
  --output PATH       Output PGN path (default: bench/data/twic-bench.pgn)
  --cache-dir PATH    Download cache for TWIC zip files
                      (default: bench/data/twic-zips)
  --base-url URL      Archive base URL
                      (default: https://theweekinchess.com/zips)
  --target-games N    Approximate target game count for --1m mode
                      (default: 1000000)
  -h, --help          Show this help text

Example:
  scripts/download_twic_pgns.sh --1m
  scripts/download_twic_pgns.sh --all --output bench/data/twic-all.pgn
  scripts/download_twic_pgns.sh --first 1500 --last 1510

Environment:
  MOTIF_IMPORT_PERF_PGN can be pointed at the resulting output file for the
  import and search performance tests. If unset, the tests also look for the
  default repo-local output path.
EOF
}

archive_exists() {
    local issue=$1
    local archive_name="twic${issue}g.zip"
    local archive_url="${base_url}/${archive_name}"
    curl --fail --head --location --silent --show-error \
        "${archive_url}" >/dev/null
}

fetch_available_issues() {
    curl --fail --location --silent --show-error \
        'https://theweekinchess.com/twic' \
        | grep -Eo 'twic[0-9]+g\.zip' \
        | sed -E 's/^twic([0-9]+)g\.zip$/\1/' \
        | sort -n -u
}

ensure_archive() {
    local issue=$1
    local archive_name="twic${issue}g.zip"
    local archive_url="${base_url}/${archive_name}"
    local archive_path="${cache_dir}/${archive_name}"

    if [[ ! -f ${archive_path} ]]; then
        printf 'downloading %s\n' "${archive_url}" >&2
        curl --fail --location --silent --show-error \
            --output "${archive_path}" "${archive_url}"
    else
        printf 'using cached %s\n' "${archive_path}" >&2
    fi

    printf '%s\n' "${archive_path}"
}

count_games_in_archive() {
    local archive_path=$1
    local count_cache="${archive_path}.games"

    if [[ -f ${count_cache} ]]; then
        cat "${count_cache}"
        return 0
    fi

    local pgn_name
    pgn_name=$(unzip -Z1 "${archive_path}" '*.pgn' | head -n 1)
    if [[ -z ${pgn_name} ]]; then
        printf 'archive %s does not contain a .pgn file\n' "${archive_path}" >&2
        return 1
    fi

    local games
    games=$(unzip -p "${archive_path}" "${pgn_name}" | rg -c '^\[Event ')
    printf '%s\n' "${games}" > "${count_cache}"
    printf '%s\n' "${games}"
}

first_issue=''
last_issue=''
output_path='bench/data/twic-bench.pgn'
cache_dir='bench/data/twic-zips'
base_url='https://theweekinchess.com/zips'
latest_issue_guess='1800'
mode=''
output_explicit='false'
selected_issues=()
target_games='1000000'

while (($# > 0)); do
    case "$1" in
        --1m)
            mode='1m'
            shift
            ;;
        --all)
            mode='all'
            shift
            ;;
        --first)
            first_issue=${2:-}
            shift 2
            ;;
        --last)
            last_issue=${2:-}
            shift 2
            ;;
        --output)
            output_path=${2:-}
            output_explicit='true'
            shift 2
            ;;
        --cache-dir)
            cache_dir=${2:-}
            shift 2
            ;;
        --base-url)
            base_url=${2:-}
            shift 2
            ;;
        --target-games)
            target_games=${2:-}
            shift 2
            ;;
        --latest-guess)
            latest_issue_guess=${2:-}
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

if [[ -n ${mode} && ( -n ${first_issue} || -n ${last_issue} ) ]]; then
    printf 'use either --1m/--all or --first/--last, not both\n' >&2
    exit 1
fi

mapfile -t available_issues < <(fetch_available_issues)
if (( ${#available_issues[@]} == 0 )); then
    printf 'could not discover any TWIC issues from the archive page\n' >&2
    exit 1
fi

first_available_issue=${available_issues[0]}
latest_issue=${available_issues[$((${#available_issues[@]} - 1))]}

if [[ -n ${mode} ]]; then
    if [[ ${mode} == 'all' ]]; then
        first_issue=${first_available_issue}
        last_issue=${latest_issue}
        selected_issues=("${available_issues[@]}")
        if [[ ${output_explicit} == 'false' ]]; then
            output_path='bench/data/twic-all.pgn'
        fi
    else
        running_games=0
        for (( index = ${#available_issues[@]} - 1; index >= 0; --index )); do
            issue=${available_issues[index]}
            archive_path=$(ensure_archive "${issue}")
            games=$(count_games_in_archive "${archive_path}")
            selected_issues=("${issue}" "${selected_issues[@]}")
            running_games=$((running_games + games))
            if (( running_games >= target_games )); then
                break
            fi
        done

        first_issue=${selected_issues[0]}
        last_issue=${selected_issues[$((${#selected_issues[@]} - 1))]}
        if [[ ${output_explicit} == 'false' ]]; then
            output_path='bench/data/twic-1m.pgn'
        fi
    fi
fi

if [[ -z ${first_issue} || -z ${last_issue} ]]; then
    printf 'both --first and --last are required\n\n' >&2
    usage >&2
    exit 1
fi

if ! [[ ${first_issue} =~ ^[0-9]+$ && ${last_issue} =~ ^[0-9]+$ ]]; then
    printf '--first and --last must be numeric TWIC issue numbers\n' >&2
    exit 1
fi

if ! [[ ${latest_issue_guess} =~ ^[0-9]+$ ]]; then
    printf '--latest-guess must be numeric\n' >&2
    exit 1
fi

if ! [[ ${target_games} =~ ^[0-9]+$ ]] || (( target_games <= 0 )); then
    printf '--target-games must be a positive integer\n' >&2
    exit 1
fi

if (( first_issue > last_issue )); then
    printf '--first must be less than or equal to --last\n' >&2
    exit 1
fi

if (( ${#selected_issues[@]} == 0 )); then
    for issue in "${available_issues[@]}"; do
        if (( issue >= first_issue && issue <= last_issue )); then
            selected_issues+=("${issue}")
        fi
    done
fi

if (( ${#selected_issues[@]} == 0 )); then
    printf 'no available TWIC issues found in range %s-%s\n' \
        "${first_issue}" "${last_issue}" >&2
    exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
    printf 'curl is required\n' >&2
    exit 1
fi

if ! command -v unzip >/dev/null 2>&1; then
    printf 'unzip is required\n' >&2
    exit 1
fi

mkdir -p "$(dirname "${output_path}")"
mkdir -p "${cache_dir}"

tmp_output="${output_path}.tmp"
rm -f "${tmp_output}"

for issue in "${selected_issues[@]}"; do
    archive_path=$(ensure_archive "${issue}")

    pgn_name=$(unzip -Z1 "${archive_path}" '*.pgn' | head -n 1)
    if [[ -z ${pgn_name} ]]; then
        printf 'archive %s does not contain a .pgn file\n' "${archive_path}" >&2
        exit 1
    fi

    printf 'appending issue %s (%s)\n' "${issue}" "${pgn_name}"
    unzip -p "${archive_path}" "${pgn_name}" >> "${tmp_output}"
    printf '\n' >> "${tmp_output}"
done

mv "${tmp_output}" "${output_path}"
printf 'wrote %s\n' "${output_path}"
