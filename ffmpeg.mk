# Define scripting
ifndef SHELL
SHELL:=/bin/bash
endif

# General directories definitions
# NOTE: PWD is passed as the "WORKING_DIRECTORY"
PROJECT_DIR = $(shell pwd)
BUILD_DIR = $(PROJECT_DIR)/build
PREFIX = $(PROJECT_DIR)/_install_dir
EXEC_PREFIX = $(PREFIX)
BINDIR = $(EXEC_PREFIX)/bin
SBINDIR = $(EXEC_PREFIX)/sbin
INCLUDEDIR = $(PREFIX)/include
LIBDIR = $(EXEC_PREFIX)/lib
NASM_SRCDIRS = $(PROJECT_DIR)/3rdplibs/nasm
X264_SRCDIRS = $(PROJECT_DIR)/3rdplibs/x264
FFMPEG_SRCDIRS = $(PROJECT_DIR)/3rdplibs/ffmpeg

# Exports
export PATH := $(BINDIR):$(SBINDIR):$(PATH)
export PKG_CONFIG_PATH := $(LIBDIR)/pkgconfig

.PHONY: nasm x264 ffmpeg

.ONESHELL:
nasm:
	@$(eval _BUILD_DIR := $(BUILD_DIR)/$@)
	@mkdir -p "$(_BUILD_DIR)"
	@if [ ! -f "$(_BUILD_DIR)"/Makefile ] ; then \
		echo "Configuring $@..."; \
		cp -a "$(NASM_SRCDIRS)"/* "$(_BUILD_DIR)"; \
		cd "$(_BUILD_DIR)" && ./autogen.sh && ./configure --prefix="$(PREFIX)" \
		$(HOST) || exit 1; \
	fi
	@$(MAKE) -C "$(_BUILD_DIR)" install || exit 1


.ONESHELL:
x264:
	@$(eval _BUILD_DIR := $(BUILD_DIR)/$@)
	@mkdir -p "$(_BUILD_DIR)"
	@if [ ! -f "$(_BUILD_DIR)"/x264_config.h ] ; then \
		echo "Configuring $@..."; \
		cp -a "$(X264_SRCDIRS)"/* "$(_BUILD_DIR)"; \
		cd "$(_BUILD_DIR)"; "$(_BUILD_DIR)"/configure $(HOST) \
		--prefix="$(PREFIX)" --enable-shared --enable-strip \
		$(PLATFORM_SPEC_OPT_X264) || exit 1; \
	fi
	@$(MAKE) -C "$(_BUILD_DIR)" install || exit 1

.ONESHELL:
ffmpeg: nasm x264
	@$(eval _BUILD_DIR := $(BUILD_DIR)/$@)
	@mkdir -p "$(_BUILD_DIR)"
	@if [ ! -f "$(_BUILD_DIR)"/Makefile ] ; then \
		echo "Configuring $@..."; \
		cd "$(_BUILD_DIR)" && "$(FFMPEG_SRCDIRS)"/configure \
		--prefix="$(PREFIX)" --enable-asm --enable-ffplay \
		--disable-doc --disable-htmlpages --disable-manpages \
		--disable-podpages --disable-txtpages --disable-debug --disable-static \
		--enable-shared --enable-gpl --enable-nonfree \
		--enable-libx264 --enable-bsf=h264_mp4toannexb \
		--extra-cflags="-I${PREFIX}/include" --extra-cxxflags="" \
		--extra-ldflags="-L${PREFIX}/lib" \
		--extra-libs="-lpthread -lm -lssl -lcrypto" \
		--optflags='-O3' \
		--pkgconfigdir="${LIBDIR}/pkgconfig" --pkg-config-flags="--static" \
		|| exit 1; \
	fi
	@$(MAKE) -C "$(_BUILD_DIR)" install -j$$(nproc) || exit 1
