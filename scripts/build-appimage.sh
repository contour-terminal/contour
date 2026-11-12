#! /bin/bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

# Builds Contour and packs it, with the Qt it was built against, into a single AppImage.
#
# Distributions do not ship the Qt Contour needs (6.11), which is what an AppImage is for: the Qt
# that configures and links the build is the Qt that gets bundled, so nothing is taken from the
# host but the C library. Build it on the OLDEST glibc you want to support (CI uses Ubuntu 24.04).
#
# Prerequisites, installed by the caller (see the `package_for_AppImage` job in build.yml):
#   - Qt 6.11 with qtmultimedia, qt5compat, qtshadertools and qtspeech, reachable through
#     $QT_ROOT_DIR (the variable install-qt-action exports) or qmake6/qmake on PATH
#   - a C++23 compiler, named by $CXX (default: g++), and the build dependencies of
#     scripts/install-deps.sh
#   - cargo with the wasm32-wasip1 target (scripts/install-rust-wasm.sh)
#   - curl, to fetch linuxdeploy, and libspeechd2 (Qt's text-to-speech plugin is bundled)
#
# Environment:
#   VERSION_STRING  version in the file name (default: "dev")
#   BUILD_DIR       CMake build tree (default: out/appimage)
#   OUTPUT_DIR      where the AppImage lands (default: the current directory)
#   CXX             C++ compiler (default: g++)
#   UCD_BASE_DIR    where libunicode keeps the Unicode database it downloads (default: inside BUILD_DIR)

LINUXDEPLOY_VERSION="1-alpha-20251107-1"
LINUXDEPLOY_QT_VERSION="1-alpha-20250213-1"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_STRING="${VERSION_STRING:-dev}"
BUILD_DIR="${BUILD_DIR:-$ROOT/out/appimage}"
OUTPUT_DIR="${OUTPUT_DIR:-$PWD}"
CC="${CC:-gcc}"
CXX="${CXX:-g++}"
APPDIR="$BUILD_DIR/AppDir"
TOOLS="$BUILD_DIR/tools"
APP_ID="org.contourterminal.Contour"

mkdir -p "$TOOLS" "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

# The Qt to bundle: the one the build resolves, found through its qmake.
if [[ -n "${QT_ROOT_DIR:-}" && -x "$QT_ROOT_DIR/bin/qmake" ]]; then
    QMAKE="$QT_ROOT_DIR/bin/qmake"
else
    QMAKE="$(command -v qmake6 || command -v qmake)"
fi
QT_PREFIX="$("$QMAKE" -query QT_INSTALL_PREFIX)"
echo "Bundling Qt $("$QMAKE" -query QT_VERSION) from $QT_PREFIX"

# {{{ build and install into the AppDir
rm -rf "$APPDIR"
launcher=()
command -v ccache >/dev/null && launcher=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
ucd=()
[[ -n "${UCD_BASE_DIR:-}" ]] && ucd=(-DLIBUNICODE_UCD_BASE_DIR="$UCD_BASE_DIR")
cmake -S "$ROOT" -B "$BUILD_DIR/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    "${launcher[@]}" \
    "${ucd[@]}" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCONTOUR_TESTING=OFF \
    -DCONTOUR_PRODUCTION_BUILD=ON \
    -DCONTOUR_WAYLAND=ON \
    -DPEDANTIC_COMPILER_WERROR=OFF
cmake --build "$BUILD_DIR/build"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR/build"
# }}}

# {{{ linuxdeploy and its Qt plugin
fetch()
{
    local url="$1" file="$2"
    [[ -x "$file" ]] || { curl -fsSL --retry 3 -o "$file" "$url" && chmod +x "$file"; }
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_VERSION/linuxdeploy-x86_64.AppImage" \
      "$TOOLS/linuxdeploy-x86_64.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_QT_VERSION/linuxdeploy-plugin-qt-x86_64.AppImage" \
      "$TOOLS/linuxdeploy-plugin-qt-x86_64.AppImage"
# }}}

# {{{ pack
# Neither FUSE nor a display exist on a CI runner: run the tools extracted.
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$TOOLS:$PATH"
export QMAKE
# The Qt libraries (and the FFmpeg ones Qt Multimedia carries) are resolved against Qt's own lib dir.
export LD_LIBRARY_PATH="$QT_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Every QML file is scanned for the modules it imports, so exactly those get bundled.
export QML_SOURCES_PATHS="$ROOT/src/contour"
# Beyond what the plugin derives from the linked Qt libraries: Wayland, TLS, SVG icons, and the FFmpeg
# Multimedia backend (Contour links Qt Multimedia for the bell sound). Qt's plugin set differs
# between builds (an aqtinstall Qt carries no Wayland platform plugin unless qtwayland was installed),
# and linuxdeploy-plugin-qt fails on a plugin that is not there, so only the present ones are named.
existing()
{
    local dir="$1" entry result=""
    shift
    for entry in "$@"; do
        [[ -e "$QT_PREFIX/plugins/$dir/$entry" || ( -z "$dir" && -e "$QT_PREFIX/plugins/$entry" ) ]] \
            && result="${result:+$result;}$entry"
    done
    echo "$result"
}
EXTRA_PLATFORM_PLUGINS="$(existing platforms libqwayland.so libqwayland-egl.so libqwayland-generic.so)"
EXTRA_QT_PLUGINS="$(existing "" svg tls multimedia wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration)"
export EXTRA_PLATFORM_PLUGINS EXTRA_QT_PLUGINS
echo "Extra platform plugins: ${EXTRA_PLATFORM_PLUGINS:-none}; extra Qt plugins: ${EXTRA_QT_PLUGINS:-none}"
export DEPLOY_PLATFORM_THEMES=1
export NO_STRIP=1
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION_STRING"

# The compiler's libstdc++ is newer than the host's on any distribution old enough to matter, and
# linuxdeploy leaves libstdc++ to the host by default, so it travels in the AppImage explicitly.
LIBSTDCXX="$("$CXX" -print-file-name=libstdc++.so.6)"
LIBGCC="$("$CXX" -print-file-name=libgcc_s.so.1)"

(
    cd "$BUILD_DIR"
    linuxdeploy-x86_64.AppImage \
        --appdir "$APPDIR" \
        --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/512x512/apps/$APP_ID.png" \
        --library "$(realpath "$LIBSTDCXX")" \
        --library "$(realpath "$LIBGCC")" \
        --plugin qt \
        --output appimage
)

mv -v "$BUILD_DIR"/Contour-*-x86_64.AppImage "$OUTPUT_DIR/contour-$VERSION_STRING-x86_64.AppImage"
# }}}
