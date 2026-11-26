#!/bin/bash
# OB-12 Audio Unit (AUv2, universal arm64 + x86_64) and its .pkg installer.
#
# usage: mac/build_au.sh <rom folder> [--install] [--no-pkg]
#
#   <rom folder>  the decoded firmware: the "rom" folder of the Windows release bundle
#                 (OB12.vst3/Contents/x86_64-win/rom), or one made with tools/ob12_sysex.py.
#   --install     also copy the component to ~/Library/Audio/Plug-Ins/Components and run auval.
#   --no-pkg      skip the installer.
#
# Needs only the Command Line Tools (xcode-select --install): clang, python3, codesign, pkgbuild.
# Optional: OB12_SIGN="Developer ID Application: ..." and OB12_INSTALLER_SIGN="Developer ID
# Installer: ..." to sign for other Macs; without them the component is signed ad hoc, which is
# what this Mac needs to load it.
set -euo pipefail

VERSION="0.10.0"
VERSION_INT=$(( (0 << 16) | (10 << 8) | 0 ))

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/build/mac"
INSTALL=0
PKG=1
ROM_IN=""
for arg in "$@"; do
    case "$arg" in
        --install) INSTALL=1 ;;
        --no-pkg) PKG=0 ;;
        *) ROM_IN="$arg" ;;
    esac
done
if [ -z "$ROM_IN" ]; then echo "usage: mac/build_au.sh <rom folder> [--install] [--no-pkg]"; exit 1; fi
ROM_IN="$(cd "$ROM_IN" && pwd)"

say() { printf '\n== %s\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1 ($2)"; exit 1; }; }
need xcrun "xcode-select --install"
need python3 "xcode-select --install"
need pkgbuild "part of macOS"
CXX="$(xcrun --find clang++ 2>/dev/null || true)"
SDK="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
if [ -z "$CXX" ] || [ -z "$SDK" ] || [ ! -d "$SDK/System/Library/Frameworks/AudioToolbox.framework" ]; then
    echo "The Command Line Tools are missing or still installing (no macOS SDK found)."
    echo "Run: xcode-select --install   then wait for the installer to finish, and run this script again."
    exit 1
fi
echo "SDK: $SDK"

say "Firmware"
for f in 12cpu152-CPU-v1.52-FINAL.bin system-parameters.bin boot_152-emergency-boot.bin \
         12dsp111-DSP-v1.11-FINAL.bin presets/12tmbFS.bin presets/12prgFS.bin; do
    [ -f "$ROM_IN/$f" ] || { echo "not in $ROM_IN: $f"; exit 1; }
done
[ -f "$ROM_IN/panel.ob12p" ] || echo "warning: no panel.ob12p, the editor will show a bare panel"
[ -f "$ROM_IN/font_sector.bin" ] || echo "warning: no font_sector.bin, the LCD text will be unreadable"
rm -rf "$OUT"
mkdir -p "$OUT/obj" "$OUT/gen"
ditto "$ROM_IN" "$OUT/rom"
find "$OUT/rom" -name '.DS_Store' -delete
if [ ! -d "$OUT/rom/dsp" ] || [ -z "$(ls "$OUT/rom/dsp" 2>/dev/null)" ]; then
    python3 "$ROOT/tools/ob12_carve.py" "$OUT/rom/12dsp111-DSP-v1.11-FINAL.bin" -o "$OUT/rom/dsp"
fi
# The DSP programs translated into straight-line code (as build_master.bat does).
python3 "$ROOT/tools/gen_fast.py" "$OUT/rom/dsp" "$OUT/gen/tms_fast.inc"

say "Compile (arm64 + x86_64)"
ARCH="-arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -isysroot $SDK"
CXXFLAGS="$ARCH -std=c++17 -O3 -DNDEBUG -w -fvisibility=hidden -fvisibility-inlines-hidden"
CORE="-I$ROOT/src/shim -I$ROOT/src/f2mc16 -I$ROOT/src -I$OUT/gen -include emu.h"
MAC="-I$ROOT/src -I$ROOT/mac -fobjc-arc"
"$CXX" $CXXFLAGS $CORE -c "$ROOT/src/f2mc16/f2mc16.cpp"       -o "$OUT/obj/f2mc16.o" &
"$CXX" $CXXFLAGS $CORE -c "$ROOT/src/tms57070/tms57070.cpp"   -o "$OUT/obj/tms57070.o" &
"$CXX" $CXXFLAGS $CORE -c "$ROOT/src/ob12_engine.cpp"         -o "$OUT/obj/ob12_engine.o" &
"$CXX" $CXXFLAGS $MAC  -c "$ROOT/mac/ob12_au.mm"              -o "$OUT/obj/ob12_au.o" &
"$CXX" $CXXFLAGS $MAC  -c "$ROOT/mac/panel_view_mac.mm"       -o "$OUT/obj/panel_view_mac.o" &
FAIL=0
for job in $(jobs -p); do wait "$job" || FAIL=1; done
[ $FAIL = 0 ] || { echo "compile failed"; exit 1; }

say "Link and bundle"
BUNDLE="$OUT/OB12.component"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
"$CXX" $ARCH -bundle -o "$BUNDLE/Contents/MacOS/OB12" "$OUT"/obj/*.o \
    -framework AudioToolbox -framework CoreAudio -framework AppKit -framework Foundation \
    -framework CoreFoundation -framework CoreGraphics \
    -Wl,-exported_symbol,_OB12AUFactory -Wl,-dead_strip
sed -e "s/@VERSION@/$VERSION/g" -e "s/@VERSION_INT@/$VERSION_INT/g" "$HERE/Info.plist.in" > "$BUNDLE/Contents/Info.plist"
printf 'BNDL????' > "$BUNDLE/Contents/PkgInfo"
ditto "$OUT/rom" "$BUNDLE/Contents/Resources/rom"
rm -rf "$BUNDLE/Contents/Resources/rom/dsp"   # the build's input only; the engine reads the firmware
codesign --force --sign "${OB12_SIGN:--}" ${OB12_SIGN:+--options runtime --timestamp} "$BUNDLE"
codesign --verify --strict "$BUNDLE" && echo "signed: ${OB12_SIGN:-ad hoc}"
lipo -info "$BUNDLE/Contents/MacOS/OB12"

if [ $INSTALL = 1 ]; then
    say "Install for this user and validate"
    DEST="$HOME/Library/Audio/Plug-Ins/Components"
    mkdir -p "$DEST"
    rm -rf "$DEST/OB12.component"
    ditto "$BUNDLE" "$DEST/OB12.component"
    killall -9 AudioComponentRegistrar 2>/dev/null || true
    sleep 2
    auval -v aumu Ob12 Tuth | tail -25
fi

if [ $PKG = 1 ]; then
    say "Installer"
    STAGE="$OUT/pkgroot"
    mkdir -p "$STAGE/Library/Audio/Plug-Ins/Components" "$OUT/scripts"
    ditto "$BUNDLE" "$STAGE/Library/Audio/Plug-Ins/Components/OB12.component"
    cp "$HERE/pkg/postinstall" "$OUT/scripts/postinstall"
    chmod 755 "$OUT/scripts/postinstall"
    pkgbuild --analyze --root "$STAGE" "$OUT/components.plist" >/dev/null
    # Install where it belongs, never into a copy the user moved elsewhere.
    plutil -replace 0.BundleIsRelocatable -bool NO "$OUT/components.plist"
    pkgbuild --root "$STAGE" --component-plist "$OUT/components.plist" --scripts "$OUT/scripts" \
        --identifier com.tuth.ob12.au.component --version "$VERSION" --install-location / \
        "$OUT/OB12-component.pkg"
    sed -e "s/@VERSION@/$VERSION/g" "$HERE/pkg/distribution.xml" > "$OUT/distribution.xml"
    mkdir -p "$OUT/resources"
    cp "$HERE/pkg/welcome.html" "$OUT/resources/"
    productbuild --distribution "$OUT/distribution.xml" --package-path "$OUT" --resources "$OUT/resources" \
        ${OB12_INSTALLER_SIGN:+--sign "$OB12_INSTALLER_SIGN"} "$OUT/OB12-AU-$VERSION.pkg"
    echo "installer: $OUT/OB12-AU-$VERSION.pkg"
fi
say "Done: $BUNDLE"
