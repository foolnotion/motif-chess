#!/usr/bin/env bash
# Pre-commit hook: run clang-format and clang-tidy on staged C++ files.
# Install: cp scripts/pre-commit.sh .git/hooks/pre-commit

set -euo pipefail

build_dir="build/dev"
staged_cpp_files=$(git diff --cached --name-only --diff-filter=ACM -- '*.cpp' '*.hpp' '*.h' 2>/dev/null || true)

if [ -z "$staged_cpp_files" ]; then
    exit 0
fi

echo "Running clang-format on staged files..."
echo "$staged_cpp_files" | xargs clang-format -i

echo "Running clang-tidy on staged files..."
if [ ! -f "$build_dir/compile_commands.json" ]; then
    echo "clang-tidy requires $build_dir/compile_commands.json; run cmake --preset=dev first." >&2
    exit 1
fi

for f in $staged_cpp_files; do
    if [ -f "$f" ]; then
        clang-tidy --quiet -p "$build_dir" --extra-arg="-w" "$f"
    fi
done

# Re-stage any files that clang-format modified
echo "$staged_cpp_files" | xargs git add

echo "Pre-commit checks passed."
