# AndroidMount

A native macOS menu-bar app that auto-mounts your Android phone as a
real Finder volume over MTP, using **libmtp** + **macFUSE**.

No window. No drag-and-drop app. Plug your phone in, pick "File
Transfer" on the device, and `/Volumes/AndroidDevice` appears in the
Finder sidebar.

```
┌──────────────────────┐    USB    ┌─────────────┐
│  AndroidMount.app    │◄─────────►│  Android    │
│  (Swift, menu bar)   │   IOKit   │  phone      │
│        │             │           │  (MTP mode) │
│        ▼             │           └─────────────┘
│  spawns `mtpfuse`    │
│  (C, libmtp + FUSE)  │──► /Volumes/AndroidDevice in Finder
└──────────────────────┘
```

## Prerequisites

1. **Xcode command line tools** – `xcode-select --install`
2. **Homebrew + libmtp** – `brew install libmtp`
3. **macFUSE** – download from <https://osxfuse.github.io>, install,
   and **reboot** (it needs to load a kernel extension).
4. Android device with **MTP / "File Transfer"** mode enabled (not PTP
   or charge-only).

## Build

```bash
cd AndroidMount
make check-deps     # verifies macFUSE + libmtp are reachable
make                # builds build/AndroidMount.app
make run            # launches the app
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
* `MountManager.swift` – on connect, spawns `mtpfuse -f -o
  volname=<name> /Volumes/AndroidDevice`. On disconnect (or "Eject"),
  runs `diskutil unmount force` then `terminate()` + SIGKILL fallback.
* `mtp_bridge.c` – wraps libmtp: `LIBMTP_Init`, `Get_First_Device`,
  `Get_Storage`, `Get_Files_And_Folders` walked lazily into an
  in-memory `path → object_id` tree.
* `fs_ops.c` – FUSE 2.x `getattr / readdir / read / write / unlink /
  mkdir / rmdir / create / release / truncate`. Reads/writes are
  staged through a per-handle temp file because MTP is request/response
  and not seekable; on `release()` a dirty handle is pushed back to the
  device with `LIBMTP_Send_File_From_File_Descriptor`.

## Limitations

* Single device at a time (libmtp's `Get_First_Device`).
* Writes happen on `release()`, so very large copies stage to `/tmp`
  first. Make sure you have enough free disk space.
* No rename support yet (MTP rename is fiddly across vendors).
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
