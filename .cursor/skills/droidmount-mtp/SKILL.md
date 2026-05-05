---
name: droidmount-mtp
description: >-
  macOS AndroidMount / mtpfuse stack: Swift app plus C FUSE daemon using libmtp
  and macFUSE. Covers USB serialization, streaming reads, upload verification,
  logging, and environment variables. Use when working on MTPFuse (C),
  AndroidMount (Swift), MTP performance, duplicate/copy issues, QuickTime
  compatibility, or mtpfuse debugging.
---

# droidmount — MTP FUSE (mtpfuse)

## Architecture

- **`mtpfuse`**: FUSE filesystem; each VFS operation may call into **`mtp_bridge.c`**, which wraps **libmtp** on a single `LIBMTP_mtpdevice_t`.

- **Changes on the phone** (new photos, deletes in another app): MTP does not give a reliable non-blocking event stream here (`Read_Event` is unsafe). **macOS**: a thread periodically calls `fuse_invalidate_path` on each top-level storage path; optional **`MTP_AGGRESSIVE_TREE_INVALIDATE=1`** also runs `mtp_invalidate_fuse_dir_cache` so in-memory dir listings are dropped and the next listing hits USB. Interval: **`MTP_DEVICE_SYNC_INTERVAL_SEC`** (default 60).
- **`g_mtp` mutex**: All libmtp USB I/O must run with this lock held. Do not run two libmtp transfers concurrently on the same device; there is no supported “parallel chunk” session here.
- **`g_lock`**: Protects the in-memory node tree; ordering with `g_mtp` is documented in `mtp_bridge.c` (never hold `g_mtp` while waiting on `g_lock` in ways that deadlock).
- **Long transfers**: `g_mtp_long_xfer` lets cheap ops (statfs, cached getattr) avoid queueing behind multi-GB Get/Send.

## Read path

- **`mtp_read_partial` / `LIBMTP_GetPartialObject`**: When `MTP_STREAM_READ` is on and the device reports partial support, `op_open` sets **`use_partial=1`** for **non–video-container** files (or for video too if `MTP_STREAM_VIDEO=1`).

- **Video / common containers** (e.g. `mp4`, `mov`, `m4v`, `m4a`, `mkv`, `avi`, `3gp`, `webm`): by default **`use_partial=0`**, so the first read path does a full **`Get_File`** to the per-handle temp file, then `pread` — same byte fidelity as “move” (which does not re-read the payload). `GetPartialObject` while **duplicating** in Finder can still produce streams QuickTime rejects on some devices; **move** can work while **duplicate** fails without this split.

- **`LIBMTP_Get_File_To_File_Descriptor`**: Also used when `MTP_STREAM_READ=0` or for `ensure_staging_from_partial` before writes on a handle that was opened partial.

**Chunk size:** `MTP_PARTIAL_CHUNK` (roughly 64K–16M bytes; default 2MiB).

**Metadata on read:** `mtp_read` throttles `Get_Filemetadata` using the same TTL idea as `mtp_stat` (`MTP_STAT_META_TTL_SEC`).

## Write path (Finder copy / duplicate / save)

- **Duplicate** is not an MTP “clone object” op: **read** bytes from the source (streaming partials OK) into Finder’s buffers, **write** them to a new temp file on the FUSE node, then **`release`** → **`mtp_write_send_fd`** → **`LIBMTP_Send_File_From_File_Descriptor`**. The bug class for a bad new **.mov** on the phone is in **staging + send + post-send verify**, not in disabling read streaming for `.mov` **opens** of the *source* file.

- **Post-upload verify** (`mtp_verify_upload_head_tail_vs_fd`): compares head and tail of the new object to the staging `fd`; for large files (≥3× the chunk size) also samples the **mid** 256 KiB, so a same-length corrupt middle fails verification and the upload is retried/rejected.

- If a handle used **`use_partial`** and the app then **writes** or must **truncate**, `ensure_staging_from_partial` pulls a full copy before mutating the staging file.

## Logging

- **File log** (when enabled): `mtp_log.c` — env includes `MTPFUSE_LOG`, `MTPFUSE_LOG_PATH`, `MTPFUSE_LOG_SYNC`, `MTPFUSE_DEBUG`. App often sets a default path under `~/.AndroidMount/`.
- **`fuse` operation traces** in `fs_ops.c` when logging is on — `open` / `read` / `write` / `release` on **both** the source and `… copy` paths during duplicate.

## Swift integration

- **`MountManager.swift`**: Spawns `mtpfuse` with the bundle path, mount options, and env. Changes to default env for users belong here and must match docs.

## When changing behavior, update

- **`AGENTS.md`** (root) if architecture or “where to look” shifts.
- **This skill** if env vars, read/upload behavior, or mutex/long-transfer rules change.
- **`README.md`** only if user-facing troubleshooting or setup changes.

## Build reference

- `make` — full app + `mtpfuse`
- `make mtpfuse` / `make app` — partial builds
- `make check-mtpfuse` (runs `scripts/test.sh` with a device) / `make check-e2e` — validation (see `Makefile`)

## Pitfalls

- Assuming **parallel** USB MTP — not safe with one libmtp device connection.
- Blaming **QuickTime** on **read streaming** when the new file is created by an **upload**; debug **`mtp_write_send_fd`**, `mtp_verify_upload_*`, and **`fuse op=release`** lines.
- Holding **`g_lock`** across **libmtp** calls without following existing lock order comments.
