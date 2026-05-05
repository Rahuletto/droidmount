#!/usr/bin/env bash
# MTP FUSE shell smoke test (mkdir, touch, cp, mv, rm, rmdir). Run from any cwd;
# the repo root is the parent of this script’s directory.
# Requires a mounted Android volume or MTPFUSE_TEST_MNT. No device: exit 0 (skip).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
cd "$ROOT"

if [[ ! -f "Makefile" ]]; then
  echo "scripts/test.sh: Makefile not found above $ROOT" >&2
  exit 1
fi

make -s "build/mtpfuse" 2>/dev/null || make "build/mtpfuse"

MNT="${MTPFUSE_TEST_MNT:-}"
if [[ -z "$MNT" ]]; then
  shopt -s nullglob
  for d in "${HOME}/.AndroidMount"/*; do
    if [[ -d "$d" ]]; then
      MNT=$d
      break
    fi
  done
  shopt -u nullglob
fi

if [[ -z "$MNT" || ! -d "$MNT" ]]; then
  echo "scripts/test.sh: skip — no FUSE mount (connect phone + AndroidMount, or set MTPFUSE_TEST_MNT)"
  exit 0
fi

BASE=""
for cand in \
  "$MNT/Internal shared storage" \
  "$MNT/SD card" \
  "$MNT"; do
  if [[ -d "$cand" ]]; then
    BASE=$cand
    break
  fi
done

if [[ -z "$BASE" ]]; then
  echo "scripts/test.sh: mount at $MNT has no expected storage folder" >&2
  exit 1
fi

NAME="mtpfuse_test_${RANDOM}_$$"
TESTROOT="${BASE}/${NAME}"
cleanup() { rm -rf "$TESTROOT" 2>/dev/null || true; }
trap cleanup EXIT

mkdir -p "$TESTROOT"
cd "$TESTROOT"

echo "scripts/test.sh: mount=$MNT"
echo "scripts/test.sh: working in $TESTROOT"

mkdir sub
touch a.txt
printf 'hello\n' > a.txt
cp a.txt b.txt
test "$(cat b.txt)" = "hello"
mkdir -p d1
mv b.txt d1/moved.txt
test -f d1/moved.txt
cp d1/moved.txt copy.txt
mkdir empty
rmdir empty
rm -f copy.txt
rm d1/moved.txt
rmdir d1
rm a.txt
rmdir sub

cd "$BASE"
rmdir "$NAME" || { echo "scripts/test.sh: rmdir $NAME failed" >&2; exit 1; }

trap - EXIT
echo "scripts/test.sh: ok (mkdir touch cp mv rmdir rm)"
