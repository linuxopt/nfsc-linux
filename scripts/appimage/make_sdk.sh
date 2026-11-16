#!/bin/bash
# The AppImage's game SDK: everything the launcher needs to turn the
# player's own default.xex into the game on their PC, without the game's
# code ever being in the AppImage:
#
#   scripts/appimage/make_sdk.sh <out dir> [v3 build dir] [v4 build dir]
#
#   <out>/bin/         clang (compiles only, in-process), lld, XenonRecomp
#   <out>/lib/         their libraries; lib/clang/<ver>/include (clang's headers)
#   <out>/sysroot/     glibc, Linux and libstdc++ headers; the C runtime and
#                      libraries the game links against (crt*, libc, libm,
#                      librt, libstdc++, libgcc)
#   <out>/link/        SDL3, Vulkan, glslang, SPIRV-Tools, FFmpeg's XMA decoder
#   <out>/engine-v3/   SpeedBreaker's runtime (no game code: it calls the
#   <out>/engine-v4/   recompiled functions by name), x86-64-v3 and -v4
#   <out>/include/     XenonRecomp's ppc_context.h and SIMDe; the runtime's two
#                      headers the recompiled code includes
#   <out>/config/      XenonRecomp's NFS: Carbon config (config/carbon)
#   <out>/SDK          what it was built from
#
# The builds must be configured as for the AppImage (-DSB_GAME=carbon,
# -DSB_X86_LEVEL=...). Run on the build machine (it copies its toolchain).
set -euo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
OUT="$1"
V3="${2:-$ROOT/build/v3}"
V4="${3:-$ROOT/build/v4}"
rm -rf "$OUT"
mkdir -p "$OUT"/{bin,lib,sysroot/usr/include,sysroot/usr/lib,link,include/runtime/cpu,config}

CLANG="$(readlink -f "$(command -v clang++)")"
LLD="$(readlink -f "$(command -v ld.lld)")"
CLANG_VER="$("$CLANG" -dumpversion | cut -d. -f1)"
RESOURCE="$("$CLANG" -print-resource-dir)"
GCC_DIR="$(dirname "$("$CLANG" -print-libgcc-file-name)")"
GCC_VER="$(basename "$GCC_DIR")"
GCC_TRIPLE="$(basename "$(dirname "$GCC_DIR")")"
XR="$ROOT/tools/XenonRecomp/build/XenonRecomp/XenonRecomp"

# Programs and their libraries (the AppImage's loader runs them with lib/).
cp "$CLANG" "$OUT/bin/clang"
cp "$LLD" "$OUT/bin/lld"
cp "$XR" "$OUT/bin/XenonRecomp"
for elf in "$OUT"/bin/*; do
    ldd "$elf" | sed -n 's/^\s*\(\S*\) => \(\S*\) .*/\1 \2/p' | while read -r soname path; do
        case "$soname" in
            libc.so.* | libm.so.* | libmvec.so.* | libpthread.so.* | libdl.so.* | librt.so.* | ld-linux* | */ld-linux*) ;;  # the AppImage's glibc (or the system's)
            libstdc++.so.* | libgcc_s.so.*) ;;  # the AppImage's lib/cxx (or the system's)
            *) [ -e "$OUT/lib/$soname" ] || cp -L "$path" "$OUT/lib/$soname" ;;
        esac
    done
done
mkdir -p "$OUT/lib/clang/$CLANG_VER"
cp -r "$RESOURCE/include" "$OUT/lib/clang/$CLANG_VER/include"

# Sysroot headers: glibc's and Linux's (their packages' files), libstdc++'s,
# and any other the recompiled code includes on this machine (a dependency
# scan of the build: <execution> reaches for oneTBB's when it's installed).
SYS="$OUT/sysroot"
if command -v pacman >/dev/null; then
    pacman -Ql glibc linux-api-headers | awk '$2 ~ /^\/usr\/include\/.+[^\/]$/ {print $2}' | while read -r f; do
        mkdir -p "$SYS$(dirname "$f")"; cp -P "$f" "$SYS$f"
    done
else
    cp -r /usr/include/. "$SYS/usr/include/"
fi
cp -r "/usr/include/c++/$GCC_VER" "$SYS/usr/include/c++/"
for o in "$V3"/recomp/CMakeFiles/NfsmwRecompLib.dir/__/ppc-carbon/*.o; do
    ninja -C "$V3" -t deps "${o#$V3/}" 2>/dev/null | tail -n +2
done | sed 's/^ *//' | grep '^/usr/include/' | sort -u | while read -r f; do
    [ -e "$SYS$f" ] || { mkdir -p "$SYS$(dirname "$f")"; cp -L "$f" "$SYS$f"; }
done

# Sysroot libraries: what the clang driver links (clang++ -###), linked by lld.
L=/usr/lib
for f in Scrt1.o crti.o crtn.o libc.so libc.so.6 libc_nonshared.a ld-linux-x86-64.so.2 libm.so libm.so.6 libmvec.so.1 \
    librt.so.1 librt.a libstdc++.so.6 libgcc_s.so.1; do
    cp -L "$L/$f" "$SYS/usr/lib/$f"
done
ln -sf libstdc++.so.6 "$SYS/usr/lib/libstdc++.so"
ln -sf librt.so.1 "$SYS/usr/lib/librt.so"
G="$SYS/usr/lib/gcc/$GCC_TRIPLE/$GCC_VER"
mkdir -p "$G"
for f in crtbegin.o crtbeginS.o crtendS.o crtend.o libgcc.a libgcc_eh.a libgcc_s.so; do
    cp -L "$GCC_DIR/$f" "$G/$f"
done
# libgcc_s.so is a linker script naming libgcc_s.so.1: keep that beside it.
cp -L "$L/libgcc_s.so.1" "$G/libgcc_s.so.1"

# Libraries the game links (as the build's link line names them).
LINK="$(ninja -C "$V3" -t commands runtime/SpeedBreaker | tail -1)"
for lib in $(tr ' ' '\n' <<<"$LINK" | grep -E '\.so(\.[0-9]+)*$|ffmpeg-xma/lib/.*\.a$'); do
    case "$lib" in
        *.a) cp "$lib" "$OUT/link/" ;;
        *) soname="$(objdump -p "$lib" | awk '/SONAME/ {print $2}')"; cp -L "$lib" "$OUT/link/${soname:-$(basename "$lib")}" ;;
    esac
done

# The engine: the runtime's objects (one archive, linked whole: its game
# hooks replace recompiled functions) and its static libraries.
for level in v3 v4; do
    B="$V3"; [ "$level" = v4 ] && B="$V4"
    [ -d "$B/runtime/CMakeFiles/SpeedBreaker.dir" ] || continue
    E="$OUT/engine-$level"
    mkdir -p "$E"
    find "$B/runtime/CMakeFiles/SpeedBreaker.dir" -name '*.o' | sort > "$E/objects.txt"
    ar rcs "$E/libspeedbreaker.a" $(cat "$E/objects.txt")
    rm "$E/objects.txt"
    cp "$B/runtime/libXenonUtilsLite.a" "$B/runtime/libo1heap.a" "$B/runtime/libimgui.a" "$E/"
done

# Headers for the recompiled code, and XenonRecomp's config.
cp -r "$ROOT/tools/XenonRecomp/XenonUtils/." "$OUT/include/XenonUtils/"
find "$OUT/include/XenonUtils" -type f ! -name '*.h' -delete
cp -r "$ROOT/tools/XenonRecomp/thirdparty/simde" "$OUT/include/simde"
cp "$ROOT/runtime/cpu/indirect.h" "$ROOT/runtime/cpu/guest_time.h" "$OUT/include/runtime/cpu/"
cp -r "$ROOT/config/carbon" "$OUT/config/carbon"

cat > "$OUT/SDK" <<EOF
clang $CLANG_VER ($("$CLANG" --version | head -1)), lld, XenonRecomp (patched: tools/XenonRecomp, branch nfsmw)
gcc runtime $GCC_TRIPLE $GCC_VER
engine: NFSC Linux $(sed -n 's/^set(SB_VERSION_CARBON \([0-9.]*\)).*/\1/p' "$ROOT/CMakeLists.txt") $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo -dirty) built $(date -u +%FT%TZ)
clang_version=$CLANG_VER
gcc_triple=$GCC_TRIPLE
gcc_version=$GCC_VER
EOF
du -sh "$OUT"
