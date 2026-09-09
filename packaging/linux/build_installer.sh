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
INSTALL_PREFIX="/opt/${COMPANY_DIR}/${APP_NAME}"

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
APP_DIR="$PAYLOAD/opt/${COMPANY_DIR}/${APP_NAME}"

mkdir -p "$APP_DIR/lib"
mkdir -p "$PAYLOAD/usr/lib/vst3"
mkdir -p "$PAYLOAD/usr/share/applications"
mkdir -p "$PAYLOAD/usr/share/icons/hicolor/512x512/apps"

echo "==> Staging standalone app"
cp "$STANDALONE_BIN" "$APP_DIR/PitchNet"
chmod 755 "$APP_DIR/PitchNet"

echo "==> Staging ONNX Runtime"
cp -P "$ONNX_LIB_DIR/libonnxruntime.so" "$ONNX_LIB_DIR/libonnxruntime.so.1" "$APP_DIR/lib/"
cp "$ONNX_LIB_DIR"/libonnxruntime.so.1.*.* "$APP_DIR/lib/"
cp "$ONNX_LIB_DIR/libonnxruntime_providers_shared.so" "$APP_DIR/lib/"

echo "==> Staging models and resources"
cp -r "$ROOT_DIR/Resources/models" "$APP_DIR/models"
cp -r "$ROOT_DIR/Resources/lang" "$APP_DIR/lang"
cp -r "$ROOT_DIR/Resources/fonts" "$APP_DIR/fonts"

echo "==> Fixing standalone RPATH"
patchelf --set-rpath '$ORIGIN/lib' "$APP_DIR/PitchNet"

echo "==> Staging VST3 plugin"
cp -r "$VST3_BUNDLE" "$PAYLOAD/usr/lib/vst3/PitchNet.vst3"
PLUGIN_SO="$PAYLOAD/usr/lib/vst3/PitchNet.vst3/Contents/x86_64-linux/PitchNet.so"
patchelf --set-rpath "${INSTALL_PREFIX}/lib" "$PLUGIN_SO"

echo "==> Staging desktop integration"
cp "$ROOT_DIR/Resources/images/icon.png" "$PAYLOAD/usr/share/icons/hicolor/512x512/apps/pitchnet.png"
cat > "$PAYLOAD/usr/share/applications/pitchnet.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=PitchNet
Comment=Neural pitch editor
Exec=/usr/local/bin/pitchnet
Icon=pitchnet
Terminal=false
Categories=AudioVideo;Audio;Music;
EOF

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
INSTALL_PREFIX="__INSTALL_PREFIX__"

if [ "$(id -u)" -ne 0 ]; then
    echo "This installer must be run as root, e.g.: sudo ./$(basename "$0")" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAYLOAD="$SCRIPT_DIR/payload"

echo "Installing PitchNet ${VERSION}..."

mkdir -p "/opt/${COMPANY_DIR}"
chmod 755 "/opt/${COMPANY_DIR}"
rm -rf "${INSTALL_PREFIX}"
cp -r "$PAYLOAD/opt/${COMPANY_DIR}/${APP_NAME}" "${INSTALL_PREFIX}"
chmod -R a+rX "${INSTALL_PREFIX}"
chmod 755 "${INSTALL_PREFIX}/PitchNet"

mkdir -p /usr/lib/vst3
chmod 755 /usr/lib/vst3
rm -rf /usr/lib/vst3/PitchNet.vst3
cp -r "$PAYLOAD/usr/lib/vst3/PitchNet.vst3" /usr/lib/vst3/PitchNet.vst3
chmod -R a+rX /usr/lib/vst3/PitchNet.vst3

mkdir -p /usr/share/applications /usr/share/icons/hicolor/512x512/apps
chmod 755 /usr/share/applications /usr/share/icons/hicolor/512x512/apps
cp "$PAYLOAD/usr/share/applications/pitchnet.desktop" /usr/share/applications/pitchnet.desktop
cp "$PAYLOAD/usr/share/icons/hicolor/512x512/apps/pitchnet.png" /usr/share/icons/hicolor/512x512/apps/pitchnet.png
chmod a+r /usr/share/applications/pitchnet.desktop /usr/share/icons/hicolor/512x512/apps/pitchnet.png

ln -sf "${INSTALL_PREFIX}/PitchNet" /usr/local/bin/pitchnet

cat > "${INSTALL_PREFIX}/uninstall.sh" <<UNINSTALL_EOF
#!/usr/bin/env bash
set -euo pipefail
if [ "\$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo ${INSTALL_PREFIX}/uninstall.sh" >&2
    exit 1
fi
rm -rf "${INSTALL_PREFIX}"
rm -rf /usr/lib/vst3/PitchNet.vst3
rm -f /usr/local/bin/pitchnet
rm -f /usr/share/applications/pitchnet.desktop
rm -f /usr/share/icons/hicolor/512x512/apps/pitchnet.png
echo "PitchNet uninstalled."
UNINSTALL_EOF
chmod +x "${INSTALL_PREFIX}/uninstall.sh"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true

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
echo "  VST3 plugin: /usr/lib/vst3/PitchNet.vst3"
echo "  Uninstall:   sudo ${INSTALL_PREFIX}/uninstall.sh"
INSTALL_EOF

sed -i \
    -e "s|__APP_NAME__|${APP_NAME}|g" \
    -e "s|__COMPANY_DIR__|${COMPANY_DIR}|g" \
    -e "s|__VERSION__|${VERSION}|g" \
    -e "s|__INSTALL_PREFIX__|${INSTALL_PREFIX}|g" \
    "$STAGE/install.sh"
chmod +x "$STAGE/install.sh"

echo "==> Packaging with makeself"
mkdir -p "$ROOT_DIR/dist"
OUT="$ROOT_DIR/dist/PitchNet-${VERSION}-Linux-${ARCH}.run"
makeself --gzip "$STAGE" "$OUT" "PitchNet ${VERSION} Installer" ./install.sh

echo ""
echo "Installer created: $OUT"
