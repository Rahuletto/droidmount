# AndroidMount

A native macOS menu-bar app that auto-mounts your Android phone as a
real Finder volume over MTP, using **libmtp** + **macFUSE**.

No window. No drag-and-drop app. Plug your phone in, pick "File
Transfer" on the device, and use the **menu bar** item → **Open in
Finder**. The mount lives under **`~/.AndroidMount/<device-name>/`**
(a hidden folder in your home directory; `volname` still labels the
volume in Finder).

```
┌──────────────────────┐    USB    ┌─────────────┐
│  AndroidMount.app    │◄─────────►│  Android    │
│  (Swift, menu bar)   │   IOKit   │  phone      │
│        │             │           │  (MTP mode) │
│        ▼             │           └─────────────┘
│  spawns `mtpfuse`    │
│  (C, libmtp + FUSE)  │──► ~/.AndroidMount/... (macFUSE volume)
└──────────────────────┘
```

## Prerequisites

1. **Xcode command line tools** – `xcode-select --install`
2. **Homebrew + libmtp** – `brew install libmtp`
3. **macFUSE** – download from <https://osxfuse.github.io>, install,
   and **reboot** (it needs to load a kernel extension).
4. Android device with **MTP / "File Transfer"** mode enabled (not PTP
   or charge-only).

## Build and run

From the directory that contains this `Makefile` (repository root):

```bash
make check-deps     # verifies macFUSE + libmtp are reachable
make                # builds build/AndroidMount.app
make run            # same as: open build/AndroidMount.app
```

**Using it:** unlock the phone, set USB to **File Transfer / MTP**, then
launch the app (or keep it running). When the menu shows **Connected**,
choose **Open in Finder** (⌘O). **Eject** before unplugging.

**Debug `mtpfuse`:** to see `[LOAD]` / libmtp logs on stderr, run the
helper by hand after the phone is connected (adjust paths if needed):

```bash
mkdir -p "$HOME/.AndroidMount/Debug"
./build/mtpfuse -f -o volname=Debug "$HOME/.AndroidMount/Debug"
```

The Makefile compiles two binaries:

| Binary    | Source        | Purpose                                 |
|-----------|---------------|-----------------------------------------|
| `AndroidMount` | `AndroidMount/*.swift` | menu-bar app, USB watcher |
| `mtpfuse`      | `MTPFuse/*.c`          | FUSE daemon (libmtp bridge) |

Both are bundled into `build/AndroidMount.app`. The app finds the
helper via `Bundle.main.bundleURL/Contents/MacOS/mtpfuse`.

The build is **ad-hoc signed** (`codesign -s -`). No Apple Developer
account is required, and Gatekeeper will allow the app on the machine
that built it. macOS may still prompt the first time you launch.

## How it works

* `USBWatcher.swift` – `IOServiceAddMatchingNotification` on
  `kIOUSBDeviceClassName`, filtering by known Android vendor IDs
  (Google 0x18D1, Samsung 0x04E8, etc.) and "MTP" interface strings.
* `MountManager.swift` – on connect, spawns `mtpfuse -f -o … volname=…`
  on `~/.AndroidMount/<device>/`. On disconnect (or "Eject"), runs
  `diskutil unmount force` then `terminate()` + SIGKILL fallback.
* `mtp_bridge.c` – wraps libmtp: `LIBMTP_Init`, `Detect_Raw_Devices` /
  `Open_Raw_Device_Uncached`, `Get_Storage`, `Get_Files_And_Folders`
  **one folder level at a time** (lazy tree under `path → object_id`).
* `fs_ops.c` – FUSE 2.x `getattr / readdir / read / write / rename /
  unlink / mkdir / rmdir / create / release / truncate`. Reads/writes are
  staged through a per-handle temp file because MTP is request/response
  and not seekable; on `release()` a dirty handle is streamed back to the
  device with `LIBMTP_Send_File_From_File_Descriptor` (no full-file
  `malloc`).

## Limitations

* **Several phones at once:** the menu-bar app starts one `mtpfuse` per
  USB `locationID` under `~/.AndroidMount/<name>_<locationHex>/`. Each
  helper sets `MTP_USB_BUS_LOCATION` so libmtp opens the matching raw
  device (`bus_location`). If a device is not found, set `MTP_RAW_INDEX`
  or check stderr for the listed `bus_location` values.
* Writes happen on `release()`, so very large copies still need enough
  free disk space for the staging file under `/tmp`.
* **Rename / move** uses `LIBMTP_Set_Object_Filename` and
  `LIBMTP_Move_Object`; moving a folder to another parent refreshes the
  in-memory tree. Some vendor MTP stacks may behave oddly.
* There is no widely used “modern” replacement for **libmtp** on macOS
  for Android file transfer; this project stays on libmtp for device
  compatibility.
* macFUSE is required; if the kext is missing the app pops a one-shot
  alert with a link to the installer instead of crashing.

## Project layout

```
AndroidMount/
├── Makefile
├── README.md
├── AndroidMount/         ← Swift menu-bar app
│   ├── AppDelegate.swift
│   ├── USBWatcher.swift
│   ├── MountManager.swift
│   └── Info.plist        ← LSUIElement = YES
└── MTPFuse/              ← C FUSE daemon
    ├── main.c
    ├── fs_ops.c
    ├── fs_ops.h
    ├── mtp_bridge.c
    └── mtp_bridge.h
```

## Troubleshooting

* **Finder stuck on “Loading…”** – while the mount is active, watch the trace
  file (same PID as `mtpfuse`; stable symlink always points at the latest run):

  ```bash
  tail -f /tmp/mtpfuse-debug-latest.log
  ```

  Lines are tagged with monotonic time and pthread id. Long gaps between
  `readdir_snapshot ENTER` and `Get_Files_And_Folders returned` mean the
  phone/USB is slow listing that folder; a flood of `fuse open` / `mtp_read`
  means Finder is pulling whole files (previews). Disable file logging with
  `MTPFUSE_DEBUG=0` in the environment if you do not want `/tmp/mtpfuse-*.log`.

* **“Mount point is already in use”** – usually a leftover macFUSE mount after
  Finder or the app hung. On the next connect, the app **force-unmounts** that
  path and stops matching `mtpfuse` processes automatically; if it still fails,
  run **Eject** in the menu or `diskutil unmount force ~/.AndroidMount/<device>`.

* **"MTPFuse helper binary not found"** – run `make` (or `make run`);
  the app looks for `mtpfuse` next to its own executable.
* **`fuse: device not found, try 'modprobe fuse' first`** – macFUSE
  kext was blocked. Open *System Settings → Privacy & Security* and
  click *Allow* next to "System software from developer …", then
  reboot.
* **`No raw devices found`** when the daemon starts – the phone is
  still in *Charging* mode. Pull down the notification shade on the
  device and switch USB usage to *File Transfer*.
* **Mount appears but is empty** – tap *Allow* on the phone the very
  first time the Mac connects; MTP requires per-host authorization.
