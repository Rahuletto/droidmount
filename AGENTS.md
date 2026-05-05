# AGENTS.md — droidmount (AndroidMount)

This file orients coding agents and humans working in this repository. For deep MTP/FUSE behavior, environment flags, and debugging, use the project skill **droidmount-mtp** (`.cursor/skills/droidmount-mtp/SKILL.md`).

## What this repo is

- **AndroidMount**: macOS menu-bar app (Swift) that mounts Android phones over **MTP** using a bundled **FUSE** helper `mtpfuse` (C + libmtp + macFUSE).
- **Not** a web/Next.js project; ignore template rules that assume React/TypeScript unless you are editing unrelated files.

## Layout

| Path | Role |
|------|------|
| `AndroidMount/*.swift` | App UI, USB watch, session/mount orchestration (`MountManager` spawns `mtpfuse`) |
| `MTPFuse/main.c` | FUSE entry, process lifecycle |
| `MTPFuse/fs_ops.c` | FUSE operations, file handles, partial vs full read policy for open/read |
| `MTPFuse/mtp_bridge.c` | libmtp: device open, tree, `GetPartialObject` / `Get_File`, upload, rename |
| `MTPFuse/mtp_log.c` | Optional file logging for `mtpfuse` |
| `Makefile` | Builds `build/mtpfuse` and `build/AndroidMount.app` |
| `scripts/` | `run.sh`, `test.sh` (FUSE smoke: mkdir/cp/mv/rm/…), pack icons |

## Build and verify

- **Dependencies**: macFUSE (headers/libs as in `Makefile`), **libmtp** (Homebrew `libmtp`).
- **Build**: `make` from the repo root → `build/AndroidMount.app` and `build/mtpfuse`.
- **Checks**: `make check-mtpfuse` (needs device + FUSE); `make check-e2e` (GUI + device).

## Agent expectations

- Match existing style; **small, purposeful diffs** — do not refactor unrelated code or add noise.
- **libmtp is not thread-safe**: the bridge serializes USB I/O on `g_mtp`; do not introduce parallel libmtp calls without a second connection (not supported here).
- **Reads**: `MTP_STREAM_READ` + partial (when supported) for most files. **A/V containers** (mp4, mov, …) default to **full** `Get_File` on first read (`use_partial=0`) so Finder **duplicate** matches **move** (no chunked partial read of the source). `MTP_STREAM_VIDEO=1` re-enables partial streaming for those extensions (faster, can break QuickTime on some phones). See the skill.
- **Logging**: `MTPFUSE_LOG`, `MTPFUSE_LOG_PATH` — see README and the skill.
- **Device changed files (macOS)**: there is no safe always-on `LIBMTP_Read_Event` path (blocks USB). A background thread **invalidates** each storage root on an interval; **`MTP_DEVICE_SYNC_INTERVAL_SEC`** (3–600, default 60) tunes how often. **`MTP_AGGRESSIVE_TREE_INVALIDATE=1`** also drops the bridge’s cached children under those roots so the next `readdir` refetches from the phone (heavier). Neither can be instant; unplug/reopen app if you need a full reset.

## Skills

- **Project skill**: `.cursor/skills/droidmount-mtp/SKILL.md` — when to read it: MTP bugs, performance, duplicates, QuickTime, upload/download, env tuning, or changes to `mtp_bridge.c` / `fs_ops.c`.

Keep this file and the skill **in sync** when behavior or env vars change.
