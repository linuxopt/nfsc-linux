#!/bin/bash
# SpeedBreaker: Carbon as one AppImage for this person's own Linux PCs:
#
#   scripts/appimage/make_appimage.sh [options]    -> <out>/NFSC-Linux-x86_64.AppImage (+ .sha256)
#                                                     and NFSC-Linux-x86_64.zip, the same folder
#     --v3 <build dir>         x86-64-v3 build (default build/v3; needs -DSB_EXTRAS=ON)
#     --v4 <build dir>         x86-64-v4 (AVX-512) build (default build/v4; optional)
#     --realesrgan <dir>       Real-ESRGAN ncnn-vulkan (its binary and models/)
#     --appimagetool <file>    appimagetool; its own runtime is reused (no download)
#     --out <dir>              default build/appimage
#
# The AppImage holds no game code and no game data: on each PC the launcher
# takes default.xex from the player's own disc image, translates and compiles
# it for that PC's CPU with the SDK inside (usr/lib/sdk, make_sdk.sh), links
# it with SpeedBreaker's engine, installs the game from the image and builds
# that PC's AI texture pack. So the AppImage itself can be shared; each player
# needs their own disc. Its source (GPL-3.0) is inside, as source.tar.xz.
set -euo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
V3="$ROOT/build/v3" V4="$ROOT/build/v4" ESRGAN="" TOOL="" OUT="$ROOT/build/appimage"
while [ $# -gt 0 ]; do
    case "$1" in
        --v3) V3="$2"; shift 2 ;;
        --v4) V4="$2"; shift 2 ;;
        --realesrgan) ESRGAN="$2"; shift 2 ;;
        --appimagetool) TOOL="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "make_appimage.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
[ -x "$V3/runtime/SpeedBreaker" ] || { echo "no $V3/runtime/SpeedBreaker" >&2; exit 1; }
[ -x "$V3/extras/sb-launcher" ] && [ -x "$V3/extras/sb-texpack" ] || { echo "no launcher in $V3 (configure with -DSB_EXTRAS=ON)" >&2; exit 1; }
[ -n "$TOOL" ] && [ -x "$TOOL" ] || { echo "give --appimagetool <file>" >&2; exit 1; }
VERSION="$(sed -n 's/^set(SB_VERSION_CARBON \([0-9.]*\)).*/\1/p' "$ROOT/CMakeLists.txt")"

APPDIR="$OUT/SpeedBreaker-Carbon.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib/cxx" "$APPDIR/usr/lib/glibc" "$APPDIR/usr/share/speedbreaker-carbon/licenses" \
    "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# Programs (debug information stripped; symbol tables kept for crash reports).
# No game program: each PC builds its own (usr/lib/sdk/build-game.sh).
cp "$V3/extras/sb-launcher" "$V3/extras/sb-texpack" "$APPDIR/usr/bin/"
strip --strip-debug "$APPDIR"/usr/bin/*
install -m 755 "$ROOT/scripts/appimage/AppRun" "$APPDIR/AppRun"

# Real-ESRGAN: the binary and the models the launcher's presets use.
if [ -n "$ESRGAN" ]; then
    mkdir -p "$APPDIR/usr/lib/realesrgan/models"
    cp "$ESRGAN/realesrgan-ncnn-vulkan" "$APPDIR/usr/lib/realesrgan/"
    chmod 755 "$APPDIR/usr/lib/realesrgan/realesrgan-ncnn-vulkan"
    for m in realesrgan-x4plus realesr-animevideov3-x2 realesr-animevideov3-x3 realesr-animevideov3-x4; do
        cp "$ESRGAN/models/$m.param" "$ESRGAN/models/$m.bin" "$APPDIR/usr/lib/realesrgan/models/"
    done
fi

# Libraries: everything the programs link but what the system must provide.
# (The game's libraries from this machine's build of it: the PCs' builds link the same ones.)
elves=("$APPDIR"/usr/bin/* "$V3/runtime/SpeedBreaker")
[ -n "$ESRGAN" ] && elves+=("$APPDIR/usr/lib/realesrgan/realesrgan-ncnn-vulkan")
LIBC=""
declare -A seen
for elf in "${elves[@]}"; do
    while read -r soname path; do
        [ -n "$path" ] && [ -e "$path" ] || continue
        [ -n "${seen[$soname]:-}" ] && continue
        seen[$soname]=1
        case "$soname" in
            */ld-linux* | ld-linux* | linux-vdso*) ;;
            libvulkan.so.* | libX11* | libXext* | libxcb* | libwayland-* | libdecor-* | libdrm* | libgbm* | libGL* | libEGL* | \
            libpipewire-* | libpulse* | libasound* | libudev* | libdbus-1* | libsystemd*) ;;
            libc.so.*) LIBC="$path" ;;
            libm.so.* | libmvec.so.* | libpthread.so.* | libdl.so.* | librt.so.* | libutil.so.* | libanl.so.* | libresolv.so.*) ;;
            libstdc++.so.* | libgcc_s.so.*) cp -L "$path" "$APPDIR/usr/lib/cxx/$soname" ;;
            *) cp -L "$path" "$APPDIR/usr/lib/$soname" ;;
        esac
    done < <(ldd "$elf" | sed -n 's/^\s*\(\S*\) => \(\S*\) .*/\1 \2/p')
done
# Libraries those libraries need (SDL3's are loaded at run time from the system).
for lib in "$APPDIR"/usr/lib/*.so*; do
    while read -r soname path; do
        [ -e "$APPDIR/usr/lib/$soname" ] || [ -e "$APPDIR/usr/lib/cxx/$soname" ] && continue
        case "$soname" in
            libglslang* | libSPIRV* | libgomp* | libz.so* | libzstd* ) cp -L "$path" "$APPDIR/usr/lib/$soname" ;;
        esac
    done < <(ldd "$lib" | sed -n 's/^\s*\(\S*\) => \(\S*\) .*/\1 \2/p')
done
# The whole C library (system libraries loaded under it need all of it).
GLIBC_DIR="$(dirname "$LIBC")"
for soname in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libmvec.so.1 libpthread.so.0 libdl.so.2 librt.so.1 libutil.so.1 \
    libanl.so.1 libresolv.so.2 libnss_files.so.2 libnss_dns.so.2 libnss_compat.so.2; do
    [ -e "$GLIBC_DIR/$soname" ] && cp -L "$GLIBC_DIR/$soname" "$APPDIR/usr/lib/glibc/$soname"
done
[ -e "$APPDIR/usr/lib/glibc/ld-linux-x86-64.so.2" ] || cp -L /lib64/ld-linux-x86-64.so.2 "$APPDIR/usr/lib/glibc/"
chmod 755 "$APPDIR/usr/lib/glibc/ld-linux-x86-64.so.2"

# The game SDK, and the script that builds the game with it.
"$ROOT/scripts/appimage/make_sdk.sh" "$APPDIR/usr/lib/sdk" "$V3" "$V4" >&2
install -m 755 "$ROOT/scripts/appimage/build-game.sh" "$APPDIR/usr/lib/sdk/build-game.sh"
# Its libraries' libraries that the AppImage's lib/ doesn't have yet stay in sdk/lib.

# Data: the settings template (a fresh PC's first settings), the UI font, licenses.
SHARE="$APPDIR/usr/share/speedbreaker-carbon"
cp "$ROOT/scripts/appimage/settings-template.toml" "$SHARE/"
cp "$ROOT/runtime/thirdparty/imgui/fonts/Roboto-Medium.ttf" "$SHARE/"
cp "$ROOT/COPYING" "$ROOT/NOTICE" "$ROOT/THIRD_PARTY_NOTICES.md" "$SHARE/licenses/"
cp "$ROOT"/LICENSES/* "$SHARE/licenses/" 2>/dev/null || true
# The AppImage's own extras (Real-ESRGAN and its ncnn port, ncnn, the HUFF decoder's origin).
cp "$ROOT"/scripts/appimage/licenses/* "$SHARE/licenses/"
# The source (GPL-3.0): the repository's files, with this build's changes (never the
# git-ignored game files, generated code or toolchain).
( cd "$ROOT" && git ls-files -co --exclude-standard -z | tar --null -T - -cJf "$SHARE/source.tar.xz" --transform 's,^,speedbreaker-carbon/,' )
echo "NFSC Linux $VERSION (AppImage, $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown))" > "$SHARE/VERSION"
# RecompLauncher (recomplauncher.com) reads its profile from the AppDir's root: how it sets the
# game up from the player's disc (--prepare), starts it (--play), its settings and saves. Its
# RecompLauncher runs the setup step again after every update (the launcher's --prepare
# rebuilds only when the SDK changed).
cp "$ROOT/scripts/appimage/recomplauncher.json" "$APPDIR/recomplauncher.json"

# Everything copied from this machine's packages: their licenses and sources.
"$ROOT/scripts/appimage/collect_licenses.sh" "$APPDIR" "$SHARE/licenses"

# Desktop entry and icon.
cat > "$APPDIR/speedbreaker-carbon.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=NFSC Linux
Comment=Need for Speed: Carbon (Xbox 360), built natively from your own disc, with AI-enhanced textures
Exec=sb-launcher
Icon=speedbreaker-carbon
Categories=Game;
Terminal=false
EOF
cp "$APPDIR/speedbreaker-carbon.desktop" "$APPDIR/usr/share/applications/"
python3 -I -c "
from PIL import Image
import sys
Image.open(sys.argv[1]).convert('RGBA').resize((256, 256), Image.LANCZOS).save(sys.argv[2])
" "$ROOT/scripts/macos/AppIcon-carbon.png" "$APPDIR/speedbreaker-carbon.png"
cp "$APPDIR/speedbreaker-carbon.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/"
ln -sf speedbreaker-carbon.png "$APPDIR/.DirIcon"

# The AppImage runtime: the one inside appimagetool (an AppImage itself).
OFFSET="$("$TOOL" --appimage-offset)"
head -c "$OFFSET" "$TOOL" > "$OUT/runtime-x86_64"
# A stable name (the launcher's updater replaces the file in place, so
# shortcuts to it keep working); the version is inside (usr/share/.../VERSION).
OUTFILE="$OUT/NFSC-Linux-x86_64.AppImage"
ARCH=x86_64 "$TOOL" --no-appstream --runtime-file "$OUT/runtime-x86_64" --comp zstd "$APPDIR" "$OUTFILE" >&2
( cd "$OUT" && sha256sum "$(basename "$OUTFILE")" > "$(basename "$OUTFILE").sha256" )
# The same folder as a zip, for RecompLauncher: recomplauncher.com's release check reads
# recomplauncher.json from the top of a release's zip (never from inside an AppImage or a
# tarball). The launcher itself installs the AppImage; a zip it would install the same way, into
# a plain folder started from AppRun (unzip keeps AppRun's mode and the .DirIcon symlink).
ZIPFILE="$OUT/NFSC-Linux-x86_64.zip"
rm -f "$ZIPFILE"
( cd "$APPDIR" && zip -q -X -9 -y "$ZIPFILE" recomplauncher.json &&
    zip -q -X -9 -r -y "$ZIPFILE" $(ls -A | grep -vx recomplauncher.json) )
( cd "$OUT" && sha256sum "$(basename "$ZIPFILE")" > "$(basename "$ZIPFILE").sha256" )
echo "$OUTFILE"
