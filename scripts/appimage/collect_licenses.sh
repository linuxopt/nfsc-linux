#!/bin/bash
# The licenses of everything an AppDir carries from this machine's packages
# (as scripts/steamos/make_bundle.sh does for the SteamOS bundle):
#
#   scripts/appimage/collect_licenses.sh <AppDir> <licenses dir>
#
# For each shared library, program and header set copied in, the package it
# came from (pacman), its version and licenses, its own license files
# (/usr/share/licenses/<package>) and the SPDX texts its licenses name, in
# <licenses dir>/packages/; README-packages.txt lists them, with where each
# package's exact source is (for the LGPL and GPL ones: glibc, the GCC
# runtime, the Linux headers). Arch Linux (or a derivative) only.
set -euo pipefail
APPDIR="$1"
OUT="$2"
command -v pacman >/dev/null || { echo "collect_licenses.sh: needs pacman (Arch Linux)" >&2; exit 1; }
mkdir -p "$OUT/packages" "$OUT/spdx"
declare -A pkgs

owner() { pacman -Qqo "$1" 2>/dev/null | head -1; }
# Copied libraries and programs: the package that owns the same file name.
while IFS= read -r f; do
    name="$(basename "$f")"
    for cand in "/usr/lib/$name" "/usr/lib64/$name" "/usr/bin/$name" "/usr/lib/gcc/x86_64-pc-linux-gnu/"*/"$name"; do
        [ -e "$cand" ] || continue
        p="$(owner "$cand")"
        [ -n "$p" ] && { pkgs[$p]=1; break; }
    done
done < <(find "$APPDIR" -type f \( -name '*.so*' -o -name '*.a' -o -name '*.o' -o -perm -u+x \) ! -path '*/include/*' ! -name AppRun)
# The SDK's compiler, linker and header sets (copied under other names).
for p in clang lld llvm-libs glibc linux-api-headers gcc gcc-libs; do pacman -Q "$p" >/dev/null 2>&1 && pkgs[$p]=1; done
[ -d "$APPDIR/usr/lib/sdk/sysroot/usr/include/tbb" ] && pkgs[onetbb]=1
[ -d "$APPDIR/usr/lib/sdk/sysroot/usr/include/oneapi" ] && pkgs[onetbb]=1

{
    echo "Packages this AppImage carries files from (Arch Linux packages, as built)."
    echo "Each one's source: the upstream release its version names, built with the recipe at"
    echo "https://gitlab.archlinux.org/archlinux/packaging/packages/<package> (the tag of that version)."
    echo "The license texts: packages/<package>/ (the package's own files) and spdx/ (the texts its"
    echo "license identifiers name)."
    echo
} > "$OUT/README-packages.txt"
for p in $(printf '%s\n' "${!pkgs[@]}" | sort); do
    version="$(pacman -Q "$p" | cut -d' ' -f2)"
    licenses="$(pacman -Qi "$p" | sed -n 's/^Licenses *: //p' | sed 's/  */ /g')"
    url="$(pacman -Qi "$p" | sed -n 's/^URL *: //p')"
    printf '%-20s %-34s %s\n    %s\n' "$p" "$version" "$licenses" "$url" >> "$OUT/README-packages.txt"
    if [ -d "/usr/share/licenses/$p" ]; then
        mkdir -p "$OUT/packages/$p"
        cp -L /usr/share/licenses/"$p"/* "$OUT/packages/$p/" 2>/dev/null || true
    fi
    for id in $licenses; do
        id="${id#(}"; id="${id%)}"
        case "$id" in AND|OR|WITH) continue ;; esac
        for dir in /usr/share/licenses/spdx /usr/share/licenses/spdx/exceptions; do
            [ -f "$dir/$id.txt" ] && cp "$dir/$id.txt" "$OUT/spdx/"
        done
    done
done
echo "collect_licenses.sh: ${#pkgs[@]} packages" >&2
