#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Build a tt-kmd release tag against the running kernel and load tenstorrent.ko.
# Intended for privileged CI containers. Bind-mount the host's /lib/modules and
# /usr/src so `make` can use the running kernel's build tree.

set -euo pipefail

if [ $# -ne 1 ] || [ -z "$1" ]; then
	echo "Usage: $(basename "$0") <kmd-version>" >&2
	echo "Example: $(basename "$0") 2.10.0" >&2
	exit 1
fi

KMD_VERSION="$1"
KMD_TAG="ttkmd-${KMD_VERSION}"
KVER="$(uname -r)"
KMD_REPO="${KMD_REPO:-https://github.com/tenstorrent/tt-kmd.git}"

if [ "$(id -u)" -eq 0 ]; then
	SUDO=()
else
	SUDO=(sudo)
fi

export DEBIAN_FRONTEND=noninteractive

echo "Running kernel: ${KVER}"
echo "Building ${KMD_TAG} from ${KMD_REPO}"

"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y --no-install-recommends \
	build-essential \
	ca-certificates \
	git \
	kmod

if [ ! -e "/lib/modules/${KVER}/build" ]; then
	echo "Kernel build tree /lib/modules/${KVER}/build not found;" \
		"installing linux-headers-${KVER}"
	"${SUDO[@]}" apt-get install -y --no-install-recommends \
		"linux-headers-${KVER}"
fi

if [ ! -e "/lib/modules/${KVER}/build" ]; then
	echo "Cannot build KMD: /lib/modules/${KVER}/build is missing." >&2
	echo "Bind-mount the host /lib/modules and /usr/src into this" \
		"container, or install linux-headers-${KVER} on the runner." >&2
	exit 1
fi

# Ubuntu's kernel headers invoke the exact gcc the kernel was built with, which
# is often older than the container's default (e.g. a jammy-built kernel needs
# gcc-12 while the noble CI image ships gcc-13). Without it the module build
# dies with "gcc-N: not found".
KERNEL_CC="${KMD_CC:-}"
if [ -z "$KERNEL_CC" ]; then
	CC_VERSION_TEXT="$(sed -n 's/^CONFIG_CC_VERSION_TEXT="\(.*\)"$/\1/p' \
		"/lib/modules/${KVER}/build/.config" 2>/dev/null || true)"
	KERNEL_CC="$(printf '%s\n%s\n' "$CC_VERSION_TEXT" "$(cat /proc/version)" \
		| grep -oE 'gcc-[0-9]+' | head -n1 || true)"
fi

if [ -n "$KERNEL_CC" ] && ! command -v "$KERNEL_CC" >/dev/null 2>&1; then
	echo "Kernel was built with ${KERNEL_CC}; installing it to match"
	if ! "${SUDO[@]}" apt-get install -y --no-install-recommends "$KERNEL_CC"; then
		echo "Cannot build KMD: kernel requires ${KERNEL_CC}," \
			"which is unavailable in this image." >&2
		echo "Install it in the CI image, or set KMD_CC to a compiler that is present." >&2
		exit 1
	fi
fi

BUILD_DIR="$(mktemp -d)"
cleanup() {
	rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

git clone --depth 1 --branch "$KMD_TAG" "$KMD_REPO" "$BUILD_DIR/tt-kmd"

MAKE_ARGS=()
if [ -n "$KERNEL_CC" ]; then
	echo "Building with CC=${KERNEL_CC} ($("$KERNEL_CC" --version | head -n1))"
	MAKE_ARGS+=("CC=$KERNEL_CC")
fi
make -C "$BUILD_DIR/tt-kmd" "${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"}"

# Check /sys/module, not `lsmod | grep -q` (SIGPIPE + pipefail skips rmmod).
if [ -d /sys/module/tenstorrent ]; then
	echo "Unloading tenstorrent $(cat /sys/module/tenstorrent/version)"
	"${SUDO[@]}" rmmod tenstorrent
fi

"${SUDO[@]}" insmod "$BUILD_DIR/tt-kmd/tenstorrent.ko"

LOADED="$(cat /sys/module/tenstorrent/version)"
echo "tt-kmd loaded: ${LOADED}"
if [ "$LOADED" != "$KMD_VERSION" ]; then
	echo "Loaded KMD version '${LOADED}' does not match requested '${KMD_VERSION}'" >&2
	exit 1
fi
