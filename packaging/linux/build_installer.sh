#!/usr/bin/env bash
# Builds a self-extracting Linux installer (.run) for PitchNet, bundling the
# standalone app, the VST3 plugin, and all required models/resources.
#
# Usage: packaging/linux/build_installer.sh [build-dir]
#
# Requires: patchelf, makeself (apt install patchelf makeself)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$(cd "${1:-$ROOT_DIR/build}" && pwd)"

VERSION="$(grep -oP 'project\(PitchNet VERSION \K[0-9.]+' "$ROOT_DIR/CMakeLists.txt")"
ARCH="x86_64"
APP_NAME="PitchNet"
COMPANY_DIR="Session Loops"

STANDALONE_BIN="$BUILD_DIR/PitchNet_artefacts/Release/PitchNet"
VST3_BUNDLE="$BUILD_DIR/PitchNetPlugin_artefacts/Release/VST3/PitchNet.vst3"
ONNX_LIB_DIR="$BUILD_DIR/_deps/onnxruntime-1.19.2-src/lib"

for f in "$STANDALONE_BIN" "$VST3_BUNDLE" "$ONNX_LIB_DIR"; do
    if [ ! -e "$f" ]; then
        echo "Missing build artifact: $f (build the project first: cmake --build \"$BUILD_DIR\" --config Release)" >&2
        exit 1
    fi
done

command -v patchelf >/dev/null || { echo "patchelf is required: sudo apt install patchelf" >&2; exit 1; }
command -v makeself >/dev/null || { echo "makeself is required: sudo apt install makeself" >&2; exit 1; }

STAGE="$BUILD_DIR/installer_stage"
rm -rf "$STAGE"
PAYLOAD="$STAGE/payload"
APP_DIR="$PAYLOAD/app"

mkdir -p "$APP_DIR/lib"
mkdir -p "$PAYLOAD/vst3"

echo "==> Staging standalone app"
cp "$STANDALONE_BIN" "$APP_DIR/PitchNet"
chmod 755 "$APP_DIR/PitchNet"

echo "==> Staging ONNX Runtime"
cp -P "$ONNX_LIB_DIR/libonnxruntime.so" "$ONNX_LIB_DIR/libonnxruntime.so.1" "$APP_DIR/lib/"
cp "$ONNX_LIB_DIR"/libonnxruntime.so.1.*.* "$APP_DIR/lib/"
cp "$ONNX_LIB_DIR/libonnxruntime_providers_shared.so" "$APP_DIR/lib/"

echo "==> Staging models and resources"
cp -r "$ROOT_DIR/Resources/models" "$APP_DIR/models"
# Release builds use SwiftF0; FCPE and its tables are debug-only.
rm -f "$APP_DIR/models/fcpe.onnx" "$APP_DIR/models/mel_filterbank.bin" "$APP_DIR/models/cent_table.bin"
cp -r "$ROOT_DIR/Resources/lang" "$APP_DIR/lang"
cp -r "$ROOT_DIR/Resources/fonts" "$APP_DIR/fonts"

echo "==> Fixing standalone RPATH"
patchelf --set-rpath '$ORIGIN/lib' "$APP_DIR/PitchNet"

echo "==> Staging VST3 plugin"
cp -r "$VST3_BUNDLE" "$PAYLOAD/vst3/PitchNet.vst3"
PLUGIN_DIR="$PAYLOAD/vst3/PitchNet.vst3/Contents/x86_64-linux"
# The plugin carries its own ONNX Runtime so it loads from any install location.
cp -RP "$APP_DIR/lib" "$PLUGIN_DIR/lib"
patchelf --set-rpath '$ORIGIN/lib' "$PLUGIN_DIR/PitchNet.so"

echo "==> Staging desktop integration"
cp "$ROOT_DIR/Resources/images/icon.png" "$PAYLOAD/pitchnet.png"

echo "==> Writing install script"
# Fully-quoted template: nothing here is expanded by *this* shell. Build-time
# values are substituted afterward via sed, at the __PLACEHOLDER__ tokens.
cat > "$STAGE/install.sh" <<'INSTALL_EOF'
#!/usr/bin/env bash
set -euo pipefail
umask 022

APP_NAME="__APP_NAME__"
COMPANY_DIR="__COMPANY_DIR__"
VERSION="__VERSION__"

# Run normally: install for the current user only. Run as root: system-wide.
if [ "$(id -u)" -eq 0 ]; then
    SYSTEM_INSTALL=1
    INSTALL_PREFIX="/opt/${COMPANY_DIR}/${APP_NAME}"
    OTHER_PREFIX_LABEL="per-user"
    BIN_DIR="/usr/local/bin"
    DATA_DIR="/usr/share"
    if [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != "root" ]; then
        VST3_USER="${SUDO_USER}"
    else
        VST3_USER="$(logname 2>/dev/null || echo root)"
    fi
    VST3_HOME="$(getent passwd "${VST3_USER}" | cut -d: -f6)"
    OTHER_PREFIX="${VST3_HOME}/.local/share/${COMPANY_DIR}/${APP_NAME}"
    UNINSTALL_CMD="sudo \"${INSTALL_PREFIX}/uninstall.sh\""
else
    SYSTEM_INSTALL=0
    : "${HOME:?HOME is not set}"
    INSTALL_PREFIX="${HOME}/.local/share/${COMPANY_DIR}/${APP_NAME}"
    OTHER_PREFIX="/opt/${COMPANY_DIR}/${APP_NAME}"
    OTHER_PREFIX_LABEL="system-wide"
    BIN_DIR="${HOME}/.local/bin"
    DATA_DIR="${HOME}/.local/share"
    VST3_USER="$(id -un)"
    VST3_HOME="${HOME}"
    UNINSTALL_CMD="\"${INSTALL_PREFIX}/uninstall.sh\""
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAYLOAD="$SCRIPT_DIR/payload"

echo "Installing PitchNet ${VERSION} to ${INSTALL_PREFIX}..."

mkdir -p "$(dirname "${INSTALL_PREFIX}")"
chmod 755 "$(dirname "${INSTALL_PREFIX}")"
rm -rf "${INSTALL_PREFIX}"
cp -r "$PAYLOAD/app" "${INSTALL_PREFIX}"
chmod -R a+rX "${INSTALL_PREFIX}"
chmod 755 "${INSTALL_PREFIX}/PitchNet"

if [ -z "${VST3_HOME}" ]; then
    echo "Warning: could not resolve home directory for user '${VST3_USER}'; skipping VST3 install." >&2
else
    VST3_DIR="${VST3_HOME}/.vst3"
    mkdir -p "${VST3_DIR}"
    rm -rf "${VST3_DIR}/PitchNet.vst3"
    cp -r "$PAYLOAD/vst3/PitchNet.vst3" "${VST3_DIR}/PitchNet.vst3"
    if [ "${SYSTEM_INSTALL}" -eq 1 ]; then
        chown -R "${VST3_USER}:$(id -gn "${VST3_USER}")" "${VST3_DIR}/PitchNet.vst3"
    fi
    chmod -R u+rwX,go+rX "${VST3_DIR}/PitchNet.vst3"
fi

APPLICATIONS_DIR="${DATA_DIR}/applications"
ICON_DIR="${DATA_DIR}/icons/hicolor/512x512/apps"
mkdir -p "${APPLICATIONS_DIR}" "${ICON_DIR}" "${BIN_DIR}"
if [ "${SYSTEM_INSTALL}" -eq 1 ]; then
    chmod 755 "${APPLICATIONS_DIR}" "${ICON_DIR}"
fi

cat > "${APPLICATIONS_DIR}/pitchnet.desktop" <<DESKTOP_EOF
[Desktop Entry]
Type=Application
Name=PitchNet
Comment=Neural pitch editor
Exec="${BIN_DIR}/pitchnet" %f
Icon=pitchnet
Terminal=false
Categories=AudioVideo;Audio;Music;
DESKTOP_EOF
cp "$PAYLOAD/pitchnet.png" "${ICON_DIR}/pitchnet.png"
chmod a+r "${APPLICATIONS_DIR}/pitchnet.desktop" "${ICON_DIR}/pitchnet.png"

ln -sf "${INSTALL_PREFIX}/PitchNet" "${BIN_DIR}/pitchnet"

cat > "${INSTALL_PREFIX}/uninstall.sh" <<UNINSTALL_EOF
#!/usr/bin/env bash
set -euo pipefail
if [ "${SYSTEM_INSTALL}" -eq 1 ] && [ "\$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo \"${INSTALL_PREFIX}/uninstall.sh\"" >&2
    exit 1
fi
rm -rf "${INSTALL_PREFIX}"
if [ -n "${VST3_HOME}" ]; then
    rm -rf "${VST3_HOME}/.vst3/PitchNet.vst3"
fi
rm -f "${BIN_DIR}/pitchnet"
rm -f "${APPLICATIONS_DIR}/pitchnet.desktop"
rm -f "${ICON_DIR}/pitchnet.png"
echo "PitchNet uninstalled."
UNINSTALL_EOF
chmod +x "${INSTALL_PREFIX}/uninstall.sh"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "${APPLICATIONS_DIR}" >/dev/null 2>&1 || true
if [ "${SYSTEM_INSTALL}" -eq 1 ]; then
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi

for lib in libasound.so.2 libfontconfig.so.1 libfreetype.so.6; do
    found=""
    for dir in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib/x86_64-linux-gnu /lib; do
        if [ -e "$dir/$lib" ]; then
            found=1
            break
        fi
    done
    if [ -z "$found" ]; then
        echo "Warning: $lib not found on this system - install your distro's ALSA/fontconfig/freetype packages." >&2
    fi
done

echo ""
echo "PitchNet ${VERSION} installed."
echo "  Standalone: run 'pitchnet' or find PitchNet in your applications menu."
case ":${PATH}:" in
    *":${BIN_DIR}:"*) ;;
    *) echo "              (${BIN_DIR} is not on your PATH; the applications menu entry still works.)" ;;
esac
if [ -n "${VST3_HOME}" ]; then
    echo "  VST3 plugin: ${VST3_HOME}/.vst3/PitchNet.vst3"
fi
echo "  Uninstall:   ${UNINSTALL_CMD}"

if [ -d "${OTHER_PREFIX}" ]; then
    echo ""
    echo "Note: a ${OTHER_PREFIX_LABEL} PitchNet install also exists at ${OTHER_PREFIX}."
    echo "      Per-user files take priority; remove whichever copy you no longer want"
    echo "      by running its uninstall.sh."
fi
INSTALL_EOF

sed -i \
    -e "s|__APP_NAME__|${APP_NAME}|g" \
    -e "s|__COMPANY_DIR__|${COMPANY_DIR}|g" \
    -e "s|__VERSION__|${VERSION}|g" \
    "$STAGE/install.sh"
chmod +x "$STAGE/install.sh"

echo "==> Packaging with makeself"
mkdir -p "$ROOT_DIR/dist"
OUT="$ROOT_DIR/dist/PitchNet-Linux-${ARCH}.run"
makeself --gzip "$STAGE" "$OUT" "PitchNet ${VERSION} Installer" ./install.sh

echo ""
echo "Installer created: $OUT"
