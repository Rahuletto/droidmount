#!/usr/bin/env bash
# Clean build tree, rebuild AndroidMount.app + mtpfuse, then open the app.
# Usage:
#   ./scripts/rebuild-and-run.sh            # kill app + mtpfuse, clean, build, open
#   ./scripts/rebuild-and-run.sh --build    # kill, clean + build only
#   ./scripts/rebuild-and-run.sh --no-kill  # skip kill (e.g. build while another copy runs)
# From repo root:
#   bash scripts/rebuild-and-run.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN=1
KILL=1
for arg in "$@"; do
  case "$arg" in
    --build|-b) RUN=0 ;;
    --no-kill) KILL=0 ;;
    -h|--help)
      echo "Usage: $0 [--build] [--no-kill]"
      echo "  (default)  kill AndroidMount + mtpfuse; make clean && make && make run"
      echo "  --build      make clean && make (no open)"
      echo "  --no-kill    do not kill running processes first"
      exit 0
      ;;
  esac
done

echo "==> $(pwd)"

if [[ "$KILL" -eq 1 ]]; then
  echo "==> stop AndroidMount + mtpfuse (best-effort; ignores if not running)"
  killall AndroidMount 2>/dev/null || true
  killall mtpfuse 2>/dev/null || true
  sleep 0.4
fi

echo "==> make clean"
make clean

echo "==> make check-deps"
make check-deps

echo "==> make (app + mtpfuse + bundle)"
make

if [[ "$RUN" -eq 1 ]]; then
  echo "==> make run"
  make run
else
  echo "==> skip open (pass --build to suppress launch)"
fi

echo "==> done: $ROOT/build/AndroidMount.app"
