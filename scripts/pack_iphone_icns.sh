#!/usr/bin/env bash
# Aspect-fill pack source artwork into a full macOS .icns (no black letterbox).
# Usage: pack_iphone_icns.sh <source.icns|png|…> <dest.icns>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:?source path}"
DST="${2:?destination .icns path}"
SWIFT_BIN="${SWIFT:-swift}"
SCRIPT="$ROOT/scripts/aspect_fill_icns.swift"
[[ -f "$SCRIPT" ]] || { echo "missing $SCRIPT" >&2; exit 1; }

exec "$SWIFT_BIN" -suppress-warnings -framework AppKit "$SCRIPT" "$SRC" "$DST"
