# Linux build for the GoldHEN payload and the OpenOrbis home-screen PKG.
#
#   make payload     build/BTA-Audio.bin (needs clang and ld.lld only)
#   make pkg         build/Bluetooth-Audio-<version>.pkg (needs OO_PS4_TOOLCHAIN,
#                    Python with Pillow and matplotlib, and libssl1.1 for PkgTool)
#   make dist        both, renamed for release, plus SHA256SUMS in build/dist
#   make host-ui     build/host_ui, renders a UI scene to a PPM on the PC
#   make test        build and run the host unit tests (needs only a native cc)
#
# The Docker image in docker/ has everything; scripts/docker-build.sh runs this
# Makefile inside it. Same flags and object order as the old build_app.ps1.

VERSION      := $(shell cat VERSION)
# 2.7.5 -> 02.75, the param.sfo APP_VER/VERSION format
SFO_VERSION  := $(shell printf '%02d.%d%d' $(subst ., ,$(VERSION)))
CONTENT_ID   := IV0000-BTAD00001_00-BTAUDIO000000001
TITLE_ID     := BTAD00001

CLANG ?= clang
LLD   ?= ld.lld
PYTHON ?= python3

BUILD := build
OBJ   := $(BUILD)/bta-app
NATIVE := $(BUILD)/native
PKGDIR := $(NATIVE)/package
DIST  := $(BUILD)/dist

PAYLOAD := $(BUILD)/BTA-Audio.bin
PKG     := $(BUILD)/Bluetooth-Audio-$(VERSION).pkg

PAYLOAD_FLAGS := --target=x86_64-pc-freebsd12-elf -O2 -fPIE -ffreestanding -fno-builtin \
	-fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables \
	-Wall -Wextra -Werror -I $(OBJ)
APP_HEADERS := $(wildcard app/*.h)

.PHONY: all payload pkg dist clean host-ui test
all: payload pkg
payload: $(PAYLOAD)
pkg: $(PKG)

$(OBJ) $(NATIVE) $(DIST) $(BUILD):
	mkdir -p $@

$(OBJ)/web_page.h: app/web.html scripts/embed.py | $(OBJ)
	$(PYTHON) scripts/embed.py $< $@ web_page

$(OBJ)/launcher.o $(OBJ)/worker.o: $(OBJ)/%.o: app/%.c $(APP_HEADERS) $(OBJ)/web_page.h
	$(CLANG) $(PAYLOAD_FLAGS) -c $< -o $@

$(OBJ)/bt_devices.o: app/bt_devices.c $(APP_HEADERS) | $(OBJ)
	$(CLANG) $(PAYLOAD_FLAGS) -mgeneral-regs-only -c $< -o $@

$(OBJ)/bt_accept_patch.o: probe/bt_accept_patch.c $(APP_HEADERS) | $(OBJ)
	$(CLANG) $(PAYLOAD_FLAGS) -mgeneral-regs-only -D_start=bta_apply_patch -c $< -o $@

# Link order matters: launcher first, its entry sits at offset 0.
PAYLOAD_OBJS := $(addprefix $(OBJ)/,launcher.o worker.o bt_accept_patch.o bt_devices.o)

$(OBJ)/BTA-Audio.debug.elf: $(PAYLOAD_OBJS) app/payload.ld
	$(LLD) -T app/payload.ld $(PAYLOAD_OBJS) -o $@

$(PAYLOAD): $(PAYLOAD_OBJS) app/payload.ld $(OBJ)/BTA-Audio.debug.elf
	$(LLD) --oformat=binary -T app/payload.ld $(PAYLOAD_OBJS) -o $@

# --- Home-screen app (OpenOrbis) -------------------------------------------------

OO_BIN = $(OO_PS4_TOOLCHAIN)/bin/linux
PKGTOOL ?= $(OO_BIN)/PkgTool.Core
# PkgTool's bundled .NET runtime fails without matching ICU; it needs no locale data.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1

define need_toolchain
	@test -n "$(OO_PS4_TOOLCHAIN)" && test -d "$(OO_PS4_TOOLCHAIN)/include" || \
		{ echo 'Set OO_PS4_TOOLCHAIN to the OpenOrbis PS4Toolchain folder (or use scripts/docker-build.sh).'; exit 1; }
endef

$(NATIVE)/ui_font.h $(NATIVE)/ui_icons.h $(NATIVE)/icon0.png &: native/make_assets.py | $(NATIVE)
	$(PYTHON) native/make_assets.py

$(NATIVE)/main.o: native/main.c native/ui.h native/qr.h $(NATIVE)/ui_font.h $(NATIVE)/ui_icons.h
	$(need_toolchain)
	$(CLANG) --target=x86_64-pc-freebsd12-elf -O2 -fPIC -funwind-tables \
		-isysroot $(OO_PS4_TOOLCHAIN) -isystem $(OO_PS4_TOOLCHAIN)/include -I $(NATIVE) \
		-Wall -Wextra -Wno-misleading-indentation -c $< -o $@

$(NATIVE)/BluetoothAudio.elf: $(NATIVE)/main.o
	$(LLD) $< $(OO_PS4_TOOLCHAIN)/lib/crt1.o -m elf_x86_64 -pie --script $(OO_PS4_TOOLCHAIN)/link.x \
		--eh-frame-hdr -L $(OO_PS4_TOOLCHAIN)/lib -lc -lkernel -lSceVideoOut -lScePad \
		-lSceUserService -lSceSystemService -o $@

PKG_FILES := eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx \
	sce_module/libc.prx sce_module/libSceFios2.prx assets/BTA-Audio.bin

$(PKG): $(NATIVE)/BluetoothAudio.elf $(NATIVE)/icon0.png $(PAYLOAD) VERSION
	rm -rf $(PKGDIR)
	mkdir -p $(PKGDIR)/sce_sys/about $(PKGDIR)/sce_module $(PKGDIR)/assets
	@# Relative paths: create-fself records the -in path inside eboot.bin.
	cd $(NATIVE) && $(OO_BIN)/create-fself -in=BluetoothAudio.elf -out=BluetoothAudio.oelf \
		--eboot package/eboot.bin --paid 0x3800000000000011
	cp $(NATIVE)/icon0.png $(PKGDIR)/sce_sys/icon0.png
	cp $(OO_PS4_TOOLCHAIN)/samples/hello_world/sce_sys/about/right.sprx $(PKGDIR)/sce_sys/about/
	cp $(OO_PS4_TOOLCHAIN)/samples/hello_world/sce_module/libc.prx \
	   $(OO_PS4_TOOLCHAIN)/samples/hello_world/sce_module/libSceFios2.prx $(PKGDIR)/sce_module/
	cp $(PAYLOAD) $(PKGDIR)/assets/BTA-Audio.bin
	$(PKGTOOL) sfo_new $(PKGDIR)/sce_sys/param.sfo
	set -e; sfo() { $(PKGTOOL) sfo_setentry $(PKGDIR)/sce_sys/param.sfo $$1 --type $$2 --maxsize $$3 --value "$$4"; }; \
	sfo APP_TYPE Integer 4 1; sfo APP_VER Utf8 8 $(SFO_VERSION); \
	sfo ATTRIBUTE Integer 4 0; sfo CATEGORY Utf8 4 gd; \
	sfo CONTENT_ID Utf8 48 $(CONTENT_ID); sfo DOWNLOAD_DATA_SIZE Integer 4 0; \
	sfo SYSTEM_VER Integer 4 0; sfo TITLE Utf8 128 'Bluetooth Audio'; \
	sfo TITLE_ID Utf8 12 $(TITLE_ID); sfo VERSION Utf8 8 $(SFO_VERSION)
	cd $(PKGDIR) && $(OO_BIN)/create-gp4 -out package.gp4 --content-id=$(CONTENT_ID) --files "$(PKG_FILES)"
	cd $(PKGDIR) && $(PKGTOOL) pkg_build package.gp4 $(abspath $(NATIVE))
	cp $(NATIVE)/$(CONTENT_ID).pkg $@
	$(PKGTOOL) pkg_validate $@

# --- Release files ----------------------------------------------------------------

dist: $(PAYLOAD) $(PKG) | $(DIST)
	cp $(PAYLOAD) $(DIST)/BTA-Audio-$(VERSION).bin
	cp $(PKG) $(DIST)/Bluetooth-Audio-$(VERSION).pkg
	cd $(DIST) && sha256sum BTA-Audio-$(VERSION).bin Bluetooth-Audio-$(VERSION).pkg > SHA256SUMS
	cat $(DIST)/SHA256SUMS

clean:
	rm -rf $(BUILD)

# --- PC harness for the TV UI ---------------------------------------------------

HOST_CC ?= $(CLANG)
host-ui: $(BUILD)/host_ui
$(BUILD)/host_ui: native/host_ui.c native/ui.h native/qr.h $(NATIVE)/ui_font.h $(NATIVE)/ui_icons.h
	$(HOST_CC) -O2 -Wall -Wno-format-truncation -I $(NATIVE) $< -o $@

# --- Host unit tests ------------------------------------------------------------
# Pure C that runs natively on the PC (no PS4 toolchain, no libm). The dsp and
# strfmt suites check the extracted modules against copies of the pre-refactor
# logic over randomised inputs. TEST_CC is a native compiler; in CI this runs on
# the runner, not inside the FreeBSD-targeted build image.
TEST_CC   ?= cc
TEST_SRC  := $(wildcard tests/*.c)
TEST_HDRS := $(wildcard tests/*.h)
test: $(BUILD)/tests
	$(BUILD)/tests
$(BUILD)/tests: $(TEST_SRC) $(TEST_HDRS) $(APP_HEADERS) | $(BUILD)
	@# -Wno-unused-function: a test file may include a module header (e.g. json.h
	@# via timing.h) without exercising every helper in it; that is not a defect.
	$(TEST_CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -I app -I tests $(TEST_SRC) -o $@
