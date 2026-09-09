#!/usr/bin/env bash
# Full release pipeline for Linux: configure + build PitchNet (Release), then
# package the standalone app, VST3 plugin, and models into a self-extracting
# installer (.run) via build_installer.sh.
#
# Usage: packaging/linux/release.sh [build-dir]
#
# Requires: cmake 3.22+, patchelf, makeself (apt install patchelf makeself)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build}"

CMAKE="cmake"
if ! command -v cmake >/dev/null 2>&1; then
    if [ -x "$HOME/.local/bin/cmake" ]; then
        CMAKE="$HOME/.local/bin/cmake"
    else
        echo "cmake 3.22+ not found. Install it, e.g.: pip3 install --user cmake" >&2
        exit 1
    fi
fi

echo "==> Configuring ($BUILD_DIR)"
"$CMAKE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "==> Building"
"$CMAKE" --build "$BUILD_DIR" --config Release -j"$(nproc)"

echo "==> Packaging installer"
"$SCRIPT_DIR/build_installer.sh" "$BUILD_DIR"
