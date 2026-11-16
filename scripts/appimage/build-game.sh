#!/bin/bash
# Builds NFS: Carbon for this PC from the player's own default.xex, with the
# AppImage's SDK (scripts/appimage/make_sdk.sh). The launcher runs it.
#
#   build-game.sh --xex <default.xex> --out <dir> [--jobs N] [--march native|x86-64-v3|x86-64-v4]
#
#   1. XenonRecomp translates the XEX's PowerPC code into C++ (<out>/work/ppc)
#   2. clang compiles it for this CPU (-march=native by default), N at a time
#   3. lld links it with SpeedBreaker's engine (x86-64-v4 on an AVX-512 CPU,
#      else v3) into <out>/SpeedBreaker
#
# Progress on stdout: "stage <name>", "progress <done> <total>"; the result:
# "built <path>". SB_LOADER and SB_LIBS (from AppRun) run the SDK's programs;
# without them, the system's loader.
set -euo pipefail
SDK="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
XEX="" OUT="" JOBS="" MARCH=native
while [ $# -gt 0 ]; do
    case "$1" in
        --xex) XEX="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --march) MARCH="$2"; shift 2 ;;
        *) echo "build-game.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
[ -f "$XEX" ] && [ -n "$OUT" ] || { echo "usage: build-game.sh --xex <default.xex> --out <dir>" >&2; exit 2; }
. <(grep -E '^[a-z_]+=' "$SDK/SDK")

# The supported executable only (its SHA-256, as the installer checks it).
want=922c7a1a2fc77405c43aac5159ad67fb0414c872084abc3335c68dbb4bec1815
have="$(sha256sum "$XEX" | cut -d' ' -f1)"
if [ "$have" != "$want" ]; then
    echo "error: $XEX isn't the USA Collector's Edition's default.xex (SHA-256 $have)" >&2
    exit 3
fi

flags=" $(grep -m1 '^flags' /proc/cpuinfo | cut -d: -f2-) "
LEVEL=v4
for f in avx512f avx512bw avx512cd avx512dq avx512vl; do
    case "$flags" in *" $f "*) ;; *) LEVEL=v3 ;; esac
done
[ -d "$SDK/engine-$LEVEL" ] || LEVEL=v3
for f in avx2 fma bmi2 movbe; do
    case "$flags" in *" $f "*) ;; *) echo "error: this CPU lacks $f (the game needs x86-64-v3: Intel Haswell / AMD Zen or later)" >&2; exit 4 ;; esac
done
if [ -z "$JOBS" ]; then
    # Each compile can take ~1.5 GB: no more at once than memory allows.
    mem_gb=$(( $(awk '/^MemAvailable:/ {print $2}' /proc/meminfo) / 1048576 ))
    JOBS=$(nproc)
    by_mem=$(( mem_gb * 2 / 3 )); [ "$by_mem" -lt 1 ] && by_mem=1
    [ "$JOBS" -gt "$by_mem" ] && JOBS=$by_mem
fi

run() {  # an SDK program, through the AppImage's loader when there is one
    if [ -n "${SB_LOADER:-}" ]; then
        "$SB_LOADER" --library-path "$SDK/lib:${SB_LIBS:-}" --argv0 "$1" "$SDK/bin/$1" "${@:2}"
    else
        LD_LIBRARY_PATH="$SDK/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$SDK/bin/$1" "${@:2}"
    fi
}
export -f run
export SDK SB_LOADER SB_LIBS

WORK="$OUT/work"
rm -rf "$WORK"
mkdir -p "$WORK/config" "$WORK/ppc" "$WORK/obj"
echo "stage translate"
cp "$SDK"/config/carbon/*.toml "$WORK/config/"
# (XenonRecomp takes paths relative to its config: the XEX linked beside it.)
ln -s "$(readlink -f "$XEX")" "$WORK/config/default.xex"
sed -i -e 's#^file_path = .*#file_path = "default.xex"#' -e 's#^out_directory_path = .*#out_directory_path = "../ppc"#' "$WORK/config/carbon.toml"
run XenonRecomp "$WORK/config/carbon.toml" "$SDK/include/XenonUtils/ppc_context.h" > "$WORK/xenonrecomp.log" 2>&1 || {
    tail -20 "$WORK/xenonrecomp.log" >&2; echo "error: XenonRecomp failed" >&2; exit 5; }

echo "stage compile"
SYS="$SDK/sysroot"
CXX="$SYS/usr/include/c++/$gcc_version"
export CFLAGS_SB="--driver-mode=g++ -c -O3 -DNDEBUG -std=gnu++20 -march=$MARCH -nostdinc
 -isystem $CXX -isystem $CXX/$gcc_triple -isystem $CXX/backward -isystem $SDK/lib/clang/$clang_version/include -isystem $SYS/usr/include
 -I$WORK/ppc -I$SDK/include/XenonUtils -I$SDK/include/simde
 -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -fno-strict-aliasing
 -include $SDK/include/runtime/cpu/indirect.h -include $SDK/include/runtime/cpu/guest_time.h"
sources=("$WORK"/ppc/*.cpp)
total=${#sources[@]}
echo "progress 0 $total"
compile() {
    run clang $CFLAGS_SB -o "$2" "$1" 2> "$2.log" || { cat "$2.log" >&2; return 1; }
    rm -f "$2.log"
}
export -f compile
done_count=0
printf '%s\n' "${sources[@]}" | xargs -P "$JOBS" -I{} bash -c 'f="{}"; compile "$f" "'"$WORK"'/obj/$(basename "$f" .cpp).o" && echo ok' | \
    while read -r _; do done_count=$((done_count + 1)); echo "progress $done_count $total"; done
built=$(ls "$WORK"/obj/*.o 2>/dev/null | wc -l)
[ "$built" -eq "$total" ] || { echo "error: $((total - built)) of $total files didn't compile" >&2; exit 6; }

echo "stage link"
E="$SDK/engine-$LEVEL" G="$SYS/usr/lib/gcc/$gcc_triple/$gcc_version" LK="$SDK/link"
ls "$WORK"/obj/*.o > "$WORK/objects.rsp"
run lld -flavor gnu --sysroot="$SYS" --hash-style=gnu --build-id --eh-frame-hdr -m elf_x86_64 -pie \
    -dynamic-linker /lib64/ld-linux-x86-64.so.2 -o "$OUT/SpeedBreaker.new" \
    "$SYS/usr/lib/Scrt1.o" "$SYS/usr/lib/crti.o" "$G/crtbeginS.o" \
    -L"$G" -L"$SYS/usr/lib" \
    --whole-archive "$E/libspeedbreaker.a" --no-whole-archive \
    @"$WORK/objects.rsp" \
    "$E/libXenonUtilsLite.a" "$E/libo1heap.a" "$E/libimgui.a" \
    "$LK/libSDL3.so.0" "$LK/libvulkan.so.1" "$LK/libglslang-default-resource-limits.so.16" \
    "$LK/libavcodec.a" "$LK/libavutil.a" \
    "$LK/libSPIRV.so.16" "$LK/libglslang.so.16" "$LK/libSPIRV-Tools-opt.so" "$LK/libSPIRV-Tools.so" \
    -lrt -lstdc++ -lm -lgcc_s -lgcc -lc -lgcc_s -lgcc \
    "$G/crtendS.o" "$SYS/usr/lib/crtn.o" > "$WORK/link.log" 2>&1 || { cat "$WORK/link.log" >&2; echo "error: link failed" >&2; exit 7; }
mv -f "$OUT/SpeedBreaker.new" "$OUT/SpeedBreaker"
cat > "$OUT/build-info" <<EOF
xex=$want
sdk=$(sed -n 's/^engine: //p' "$SDK/SDK")
level=$LEVEL
march=$MARCH
cpu=$(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')
built=$(date -u +%FT%TZ)
EOF
rm -rf "$WORK"
echo "built $OUT/SpeedBreaker"
