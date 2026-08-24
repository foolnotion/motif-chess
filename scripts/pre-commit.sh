#!/usr/bin/env bash
# Pre-commit hook: run clang-format and clang-tidy on staged C++ files.
# Install: cp scripts/pre-commit.sh .git/hooks/pre-commit

set -euo pipefail

build_dir="build/dev"
staged_cpp_files=$(git diff --cached --name-only --diff-filter=ACM -- '*.cpp' '*.hpp' '*.h' 2>/dev/null || true)

if [ -z "$staged_cpp_files" ]; then
    exit 0
fi

# `git clang-format --staged` only reformats lines that differ between the
# index and HEAD, and -- critically -- refuses (exit 2) if any of those
# files also have unstaged changes, rather than silently reformatting them
# too. That's the property a plain `clang-format -i` + `git add` lacks: it
# reformats the whole working-tree file regardless of git state, and
# re-staging afterward silently sweeps any unrelated unstaged edits in that
# file into this commit. Exit code 1 from git-clang-format means "changes
# were applied", not an error -- only treat 2+ as fatal.
echo "Running clang-format on staged changes..."
set +e
git clang-format --staged --quiet -- '*.cpp' '*.hpp' '*.h'
clang_format_status=$?
set -e
if [ "$clang_format_status" -gt 1 ]; then
    exit "$clang_format_status"
fi

echo "Running clang-tidy on staged files..."
if [ ! -f "$build_dir/compile_commands.json" ]; then
    echo "clang-tidy requires $build_dir/compile_commands.json; run cmake --preset=dev first." >&2
    exit 1
fi

for f in $staged_cpp_files; do
    if [ -f "$f" ]; then
        clang-tidy --quiet -p "$build_dir" --exclude-header-filter='^.*/build/.*' --extra-arg="-w" "$f"
    fi
done

# Safe now: git-clang-format already verified none of these files have
# unstaged changes (it would have refused above otherwise), so re-staging
# the whole file only picks up its own formatting edit.
echo "$staged_cpp_files" | xargs git add

echo "Pre-commit checks passed."
