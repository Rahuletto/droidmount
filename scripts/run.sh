#!/usr/bin/env bash
# AndroidMount — build & launch helper.
#
#   ./scripts/run.sh              # kill stale UI, clean, build, open app
#   ./scripts/run.sh --build      # same but do not open
#   ./scripts/run.sh --no-kill    # no killall (e.g. parallel work)
#   make run  →  make bundle && ./scripts/run.sh --open-only
#
# Kills: AndroidMount, mtpfuse, Finder (macOS restarts Finder — clears stuck FUSE windows).
# Skip kills: ANDROIDMOUNT_SKIP_KILL=1
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
APP="${ROOT}/build/AndroidMount.app"

RUN=1
KILL=1
OPEN_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --open-only) OPEN_ONLY=1 ;;
    --build|-b) RUN=0 ;;
    --no-kill) KILL=0 ;;
    -h|--help)
      echo "Usage: $0 [--open-only] [--build] [--no-kill]"
      echo "  (default)  kill → make clean && make check-deps && make && open"
      echo "  --open-only  kill → open $APP  (use after 'make' / 'make bundle' — e.g. make run)"
      echo "  --build      no open"
      echo "  --no-kill    do not run killall"
      exit 0
      ;;
  esac
done

kill_stale() {
  if [[ "${ANDROIDMOUNT_SKIP_KILL:-}" == "1" || "$KILL" -eq 0 ]]; then
    return 0
  fi
  echo "==> kill stale: AndroidMount, mtpfuse, Finder (ignore if not running)"
  killall AndroidMount 2>/dev/null || true
  killall mtpfuse 2>/dev/null || true
  killall Finder 2>/dev/null || true
  sleep 1
}

if [[ "$OPEN_ONLY" -eq 1 ]]; then
  echo "==> open-only: ${APP}"
  kill_stale
  if [[ ! -d "$APP" ]]; then
    echo "  ! build the app first: make or make bundle" >&2
    exit 1
  fi
  open "$APP"
  echo "==> done"
  exit 0
fi

echo "==> $ROOT"

kill_stale

echo "==> make clean"
make clean

echo "==> make check-deps"
make check-deps

echo "==> make (app + mtpfuse + bundle)"
make

if [[ "$RUN" -eq 1 ]]; then
  echo "==> open ${APP}"
  open "$APP"
else
  echo "==> skip open (--build)"
fi

echo "==> done: $APP"
