# AndroidMount build
#
# Targets:
#   make            → build mtpfuse helper + AndroidMount.app
#   make mtpfuse    → C FUSE daemon only
#   make app        → Swift menu-bar app only
#   make run        → build & launch the app
#   make clean      → remove build/

SHELL := /bin/bash

# ---------- toolchain auto-detect ----------
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
    BREW_PREFIX ?= /opt/homebrew
else
    BREW_PREFIX ?= /usr/local
endif

# macFUSE always installs headers to /usr/local/include/fuse and the
# framework to /Library/Frameworks/macFUSE.framework regardless of arch.
FUSE_INC  := /usr/local/include/fuse
FUSE_LIB  := /usr/local/lib

MTP_INC   := $(BREW_PREFIX)/include
MTP_LIB   := $(BREW_PREFIX)/lib

CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter \
           -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 \
           -I$(FUSE_INC) -I$(MTP_INC)
LDFLAGS ?= -L$(FUSE_LIB) -L$(MTP_LIB) \
           -lfuse -lmtp -framework CoreFoundation

SWIFT       ?= swiftc
SWIFT_FLAGS ?= -O -target $(UNAME_M)-apple-macos11.0

BUILD := build
APP   := $(BUILD)/AndroidMount.app
# App icon → Contents/Resources/iphone.icns (first existing path wins):
#   1. icon/iphone.icns     (commit this; build/ is gitignored)
#   2. AndroidMount/iphone.icns
#   3. build/iphone.icns    (local only)
#   4. system com.apple.iphone.icns
ICNS_ICON := icon/iphone.icns
ICNS_LEGACY := AndroidMount/iphone.icns
ICNS_BUILD := $(BUILD)/iphone.icns
ICNS_FALLBACK := /System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/com.apple.iphone.icns
# Default: scripts/pack_iphone_icns.sh trims transparent edges + aspect-fills (no black letterbox).
# NO_ASPECT_FILL_ICNS=1 copies the source .icns verbatim.
PACK_ICNS := $(CURDIR)/scripts/pack_iphone_icns.sh

C_SRCS := MTPFuse/main.c MTPFuse/fs_ops.c MTPFuse/mtp_debug.c MTPFuse/mtp_bridge.c
SWIFT_SRCS := \
    AndroidMount/main.swift \
    AndroidMount/PhoneSymbol.swift \
    AndroidMount/AppDelegate.swift \
    AndroidMount/USBWatcher.swift \
    AndroidMount/MountManager.swift

.PHONY: all mtpfuse app run clean check-deps check-mtpfuse check-e2e

all: app mtpfuse bundle

# Requires an MTP device + macFUSE; exercises mount, synth dirs, xattr, small copy.
check-mtpfuse: $(BUILD)/mtpfuse
	@./scripts/mtpfuse-selftest.sh

# Launches AndroidMount.app, waits for ~/.AndroidMount FUSE, runs same ops as check-mtpfuse.
# Needs GUI session + device. Optional: ANDROIDMOUNT_E2E_RESTART=1 ANDROIDMOUNT_E2E_QUIT=1 make check-e2e
check-e2e: bundle
	@chmod +x ./scripts/androidmount-e2e.sh
	@./scripts/androidmount-e2e.sh

# ---------- C FUSE helper ----------
$(BUILD)/mtpfuse: $(C_SRCS) | $(BUILD)
	@echo "  CC   mtpfuse"
	@$(CC) $(CFLAGS) -o $@ $(C_SRCS) $(LDFLAGS)

mtpfuse: $(BUILD)/mtpfuse

# ---------- Swift app ----------
$(BUILD)/AndroidMount.bin: $(SWIFT_SRCS) | $(BUILD)
	@echo "  SWIFT AndroidMount"
	@$(SWIFT) $(SWIFT_FLAGS) -o $@ $(SWIFT_SRCS) \
	    -framework Cocoa -framework IOKit -framework UserNotifications

app: $(BUILD)/AndroidMount.bin

# ---------- .app bundle ----------
bundle: $(BUILD)/AndroidMount.bin $(BUILD)/mtpfuse
	@echo "  BUNDLE $(APP)"
	@rm -rf "$(APP)"
	@mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	@cp AndroidMount/Info.plist "$(APP)/Contents/Info.plist"
	@OUT_ICNS="$(APP)/Contents/Resources/iphone.icns"; \
	if [ -f "$(ICNS_ICON)" ]; then SRC="$(ICNS_ICON)"; echo "  ICNS $$SRC"; \
	elif [ -f "$(ICNS_LEGACY)" ]; then SRC="$(ICNS_LEGACY)"; echo "  ICNS $$SRC"; \
	elif [ -f "$(ICNS_BUILD)" ]; then SRC="$(ICNS_BUILD)"; echo "  ICNS $$SRC (build/)"; \
	elif [ -f "$(ICNS_FALLBACK)" ]; then SRC="$(ICNS_FALLBACK)"; echo "  ICNS (system fallback)"; \
	else echo "  ! add $(ICNS_ICON) (or $(ICNS_LEGACY))"; exit 1; fi; \
	if [ "$${NO_ASPECT_FILL_ICNS:-}" = "1" ]; then cp "$$SRC" "$$OUT_ICNS"; \
	else "$(PACK_ICNS)" "$$SRC" "$$OUT_ICNS"; fi
	@cp $(BUILD)/AndroidMount.bin "$(APP)/Contents/MacOS/AndroidMount"
	@cp $(BUILD)/mtpfuse "$(APP)/Contents/MacOS/mtpfuse"
	@chmod +x "$(APP)/Contents/MacOS/AndroidMount" \
	          "$(APP)/Contents/MacOS/mtpfuse"
	@# Ad-hoc sign so Gatekeeper allows local launches without a dev account
	@codesign --force --sign - "$(APP)/Contents/MacOS/mtpfuse" 2>/dev/null || true
	@codesign --force --deep --sign - "$(APP)" 2>/dev/null || true
	@echo "  built $(APP)"

run: bundle
	@open "$(APP)"

clean:
	rm -rf $(BUILD)

$(BUILD):
	@mkdir -p $(BUILD)

check-deps:
	@echo "Checking prerequisites..."
	@test -d /Library/Frameworks/macFUSE.framework \
	    || test -d /Library/Frameworks/OSXFUSE.framework \
	    || { echo "  ✗ macFUSE not installed → https://osxfuse.github.io"; exit 1; }
	@test -f $(FUSE_INC)/fuse.h \
	    || { echo "  ✗ FUSE headers missing at $(FUSE_INC)/fuse.h"; exit 1; }
	@test -f $(MTP_INC)/libmtp.h \
	    || { echo "  ✗ libmtp not installed → brew install libmtp"; exit 1; }
	@echo "  ✓ macFUSE + libmtp present"
