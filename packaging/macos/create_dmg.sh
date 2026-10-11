#!/usr/bin/env bash
# Create an unsigned pre-release DMG. macOS Gatekeeper may require the user to
# explicitly approve opening the copied application because this artifact is not
# signed or notarized.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <input-app-path> <output-dmg-path>\n' "$0" >&2
    exit 64
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf 'error: DMG creation requires macOS and hdiutil\n' >&2
    exit 69
fi

input_app=$1
output_dmg=$2

if [[ ! -d "$input_app" || "${input_app##*.}" != "app" ]]; then
    printf 'error: input app must be an existing .app bundle: %s\n' "$input_app" >&2
    exit 66
fi

if [[ -e "$output_dmg" && ! -f "$output_dmg" ]]; then
    printf 'error: output path is not a file: %s\n' "$output_dmg" >&2
    exit 73
fi

output_directory=$(dirname "$output_dmg")
mkdir -p "$output_directory"
staging_directory=$(mktemp -d "${TMPDIR:-/tmp}/motif-chess-dmg.XXXXXX")
trap 'chmod -R u+w "$staging_directory"; rm -rf "$staging_directory"' EXIT

ditto "$input_app" "$staging_directory/$(basename "$input_app")"
rm -f "$output_dmg"
hdiutil create -volname "Motif Chess" -srcfolder "$staging_directory" -ov -format UDZO "$output_dmg"
printf 'created unsigned DMG: %s\n' "$output_dmg"
