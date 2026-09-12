#!/usr/bin/env bash
#
# Build the zepLinux shell sample for the NXP i.MX RT1064 (Cortex-M7).
#
#   ./build_rt1064.sh                 # incremental build
#   ./build_rt1064.sh clean           # wipe the build dir and reconfigure
#   ./build_rt1064.sh menuconfig      # open the Kconfig editor
#
#   SAMPLE=zephyr/samples/<somewhere> ./build_rt1064.sh
#
# The script is self-locating: it builds from wherever this file lives, so it
# works from any clone at any path. It expects the standard Zephyr setup and
# hard-codes nothing about the machine it runs on.
#
# ---------------------------------------------------------------------------
# One-time setup
# ---------------------------------------------------------------------------
#
# 1. A west workspace for this repository. The repository ships its own Zephyr
#    tree and a west manifest (zephyr/west.yml) but no checked-out modules, so
#    they have to be fetched once:
#
#        cd /path/to/OneWo-zepLinux-RT1064
#        west init -l zephyr
#        west update
#
#    After that west resolves every module from the manifest, at the revisions
#    this Zephyr tree was written against, and nothing has to be listed by hand.
#
# 2. Zephyr SDK 0.17.x, exported so the build can find it:
#
#        export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.4
#
#    Note: SDK 1.0.x does NOT work with this tree. Its CMake package config
#    refuses any request below version 1.0, and the tree asks for 0.16, so the
#    configure step fails with "Could not find a configuration file for package
#    Zephyr-sdk that is compatible with requested version 0.16".
#
# 3. `west` and the Zephyr Python requirements on PATH - normally a virtualenv:
#
#        python -m venv .venv && . .venv/bin/activate
#        pip install -r zephyr/scripts/requirements.txt
#
# ---------------------------------------------------------------------------
# Flashing
# ---------------------------------------------------------------------------
#
# The build produces build-rt1064/zephyr/zephyr.elf. Any runner for this part
# works, for example over a DAPLink:
#
#     pyocd load --target mimxrt1064 -e chip build-rt1064/zephyr/zephyr.elf
#
# Console is LPUART1 at 115200 8N1 (pads GPIO_AD_B0_12 / GPIO_AD_B0_13).
#
# ---------------------------------------------------------------------------

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SAMPLE="${SAMPLE:-zephyr/samples/ansilic/rt1064_shell_process}"
BUILD_DIR="$REPO_DIR/build-rt1064"
BOARD="mimxrt1064_evk"

# ---------------------------------------------------------------- checks ----

if [ ! -d "$REPO_DIR/zephyr" ]; then
	echo "error: $REPO_DIR/zephyr not found." >&2
	echo "Run this script from inside the repository." >&2
	exit 1
fi

if ! command -v west >/dev/null 2>&1; then
	echo "error: 'west' not found on PATH." >&2
	echo "Install the Zephyr requirements and activate the virtualenv, then retry." >&2
	exit 1
fi

if [ -z "${ZEPHYR_SDK_INSTALL_DIR:-}" ]; then
	echo "error: ZEPHYR_SDK_INSTALL_DIR is not set." >&2
	echo >&2
	echo "   export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.x" >&2
	echo >&2
	echo "SDK 0.17.x is required; 1.0.x is rejected by this tree (it asks for 0.16)." >&2
	exit 1
fi

if [ ! -d "$ZEPHYR_SDK_INSTALL_DIR" ]; then
	echo "error: ZEPHYR_SDK_INSTALL_DIR does not exist: $ZEPHYR_SDK_INSTALL_DIR" >&2
	exit 1
fi

# The manifest lives in zephyr/, so its modules land under <repo>/modules.
# Warn rather than fail: a workspace may legitimately keep them elsewhere.
if [ -d "$REPO_DIR/zephyr" ] && [ ! -d "$REPO_DIR/modules/hal/nxp" ] \
   && [ -z "${ZEPHYR_MODULES:-}" ]; then
	echo "warning: $REPO_DIR/modules/hal/nxp is missing." >&2
	echo "         If this is a fresh clone, fetch the modules first:" >&2
	echo "             west init -l zephyr && west update" >&2
	echo >&2
fi

# ---------------------------------------------------------------- build -----

export ZEPHYR_BASE="$REPO_DIR/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

# Note: no ZEPHYR_MODULES is set here on purpose. With a proper west workspace
# the module set comes from the manifest, which is the only way to get the
# revisions this Zephyr tree expects.

case "${1:-build}" in
	clean)
		rm -rf "$BUILD_DIR"
		FLAG="-p always"
		;;
	menuconfig)
		FLAG="-t menuconfig"
		;;
	build | "")
		FLAG=""
		;;
	*)
		echo "usage: $0 [build|clean|menuconfig]" >&2
		exit 2
		;;
esac

echo "=== building $SAMPLE for $BOARD ==="
echo "    repo : $REPO_DIR"
echo "    sdk  : $ZEPHYR_SDK_INSTALL_DIR"

west build $FLAG -b "$BOARD" -d "$BUILD_DIR" "$REPO_DIR/$SAMPLE"

echo
echo "=== artifacts ==="
ls -la "$BUILD_DIR/zephyr/zephyr.elf" "$BUILD_DIR/zephyr/zephyr.bin"
