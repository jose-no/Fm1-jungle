#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build JUNGLE with fm1-base (base/, a git submodule).
#   ./build.sh [--release X.Y] [--host | --web]
#   JIELI_TOOLCHAIN  JieLi Linux toolchain (default: ~/.jieli/toolchain; base/tools/get_toolchain.sh)
#   AC79_SDK         JieLi AC79 SDK (default: ~/.jieli/ac79-sdk; base/bsp/tools/get_sdk.sh)
set -e
cd "$(dirname "$0")"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/.jieli/ac79-sdk}"
PY="${PYTHON:-python3}"
[ -f base/tools/build.py ] || { echo "build.sh: no fm1-base in base/ (git submodule update --init --recursive)"; exit 1; }
exec "$PY" base/tools/build.py app "$@"
