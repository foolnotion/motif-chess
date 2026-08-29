#!/usr/bin/env bash

set -euo pipefail

range=${1:---all}
prohibited_pattern='^(co-authored-by:.*(anthropic|opencode)|claude-session:)'

if git log --format=%B "$range" | grep --extended-regexp --ignore-case --quiet "$prohibited_pattern"; then
    echo "Commit history contains prohibited AI attribution trailers." >&2
    exit 1
fi
