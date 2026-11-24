#!/usr/bin/env bash
# Links resonance_host into a PS5 title executable with the PS5_Vulkan title recipe
# (tools/radv-link.sh there): the static RADV driver, the payload SDK fork's platform layer and C++
# runtime, the PacBrew archives, then converts and signs it, as Perfect Eye's ps5/link-title.sh does.
#
# CMake calls it as the executable link rule (cmake/ps5-toolchain.cmake):
#   link-title.sh <output> <objects, archives and flags as CMake passes them>
# It writes <output> (the signed eboot), <output>.elf (the unsigned ELF, with symbols) and
# <output>.map (the link map).
set -euo pipefail

: "${RADV_ROOT:?set RADV_ROOT to the PS5_Vulkan checkout}"
: "${PACBREW:?set PACBREW to the PacBrew sysroot (user/homebrew)}"
out=$1
shift
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$RADV_ROOT
sdk_root="$root/.deps/native/ps5-payload-sdk"
archive="${RADV_ARCHIVE:-$root/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}"
native="$root/tooling/native"
tool="$root/build/host/ps5-native-tool"
[[ -x $tool ]] || tool="$root/build/runtime-shim/ps5-native-tool"
work="$(dirname "$out")/ps5-link-$(basename "$out")"
mkdir -p "$work/obj" "$work/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" "$@"; }

# CMake's arguments in their order: -Wl, flags become linker flags (whole archives, groups, the
# HostBreadcrumb wrap and version script), PacBrew -l names become archives, and the system
# modules come from the SDK's stub libraries below.
inputs=()
for arg in "$@"; do
    case $arg in
        # host/gfx/local-symbols.map is Linux's: here title-local.map keeps every name local
        -Wl,--version-script=*) ;;
        -Wl,*) IFS=, read -r -a parts <<< "${arg#-Wl,}"; inputs+=("${parts[@]}") ;;
        # zlib is in the RADV archive, which is linked whole
        -lz|*/libz.a|-lm|-latomic|-pthread|-ldl|-lpthread|-lSce*) ;;
        -l*)
            lib="$PACBREW/lib/lib${arg#-l}.a"
            [[ -f $lib ]] || { echo "link-title.sh: no archive for $arg" >&2; exit 2; }
            inputs+=("$lib") ;;
        -*) ;;
        *) inputs+=("$arg") ;;
    esac
done
inputs+=("$PACBREW/lib/libiconv.a")

# The game defines C library names for itself (host/gfx/local-symbols.map: src/os/filelog.cpp's
# read, write, close and isatty, host/io/stdio.cpp's fopen). On Linux the version script keeps them
# from SDL2 and Mesa, which call the C library's; here those are static archives too, so the game's
# own objects and archives (the build tree's, relative paths) get the names renamed apart instead.
# scePadInit and scePadRead: host/io/pad.cpp's PS2 libpad, which share their names with the
# console's libScePad; SDL2's controller driver calls the console's (2026-11-23: no controller).
rename=()
for name in close fopen isatty read write scePadInit scePadRead; do
    rename+=(--redefine-sym "$name=resonance_$name")
done
for i in "${!inputs[@]}"; do
    input=${inputs[$i]}
    # host/ps5/ps5.c is the C library's side: its isatty is the one the libraries import
    if [[ $input != /* && $input == *.[oa] && $input != */ps5/ps5.c.o ]]; then
        mkdir -p "$work/game/$(dirname "$input")"
        "$sdk_root/bin/llvm-objcopy" "${rename[@]}" "$input" "$work/game/$input"
        inputs[$i]=$work/game/$input
    fi
done

cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
# The game's operator new and delete (src/os/mem.cpp) come first; the runtime's fill in the rest.
"$sdk_root/bin/llvm-objcopy" --wildcard --weaken-symbol='_Zn*' --weaken-symbol='_Zd*' "$work/obj/app_cpp_runtime.o"

# AGC comes from system modules; these host-link stubs only name the imports.
stub() {
    cc -std=c11 -O2 -fPIC -c "$root/$2" -o "$work/obj/$1_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "$1.prx" -o "$work/stubs/$1.so" "$work/obj/$1_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk_root" "$archive" || exit 2
radv_linker_script=(-T "$here/title.ld" -L "$root/tooling/psbc" -L "$root/tooling/native")
# The low heap is host/core/heap.c's own malloc family (HostMapGameMemory); the platform's title
# heap is what heap.c calls the system heap, so its wraps stay but are renamed for heap.c to call.
platform="$work/libps5platform.a"
redefine=()
for name in malloc calloc realloc free posix_memalign aligned_alloc memalign malloc_usable_size \
        reallocf reallocarray; do
    redefine+=(--redefine-sym "__wrap_$name=ps5_title_$name")
done
"$sdk_root/bin/llvm-objcopy" "${redefine[@]}" "$sdk_root/target/lib/libps5platform.a" "$platform"
for i in "${!radv_link_inputs[@]}"; do
    [[ ${radv_link_inputs[$i]} == "$sdk_root/target/lib/libps5platform.a" ]] && radv_link_inputs[$i]=$platform
done

"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    --version-script "$here/title-local.map" -Map "$out.map" \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" \
    "${inputs[@]}" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
cp "$work/llvm-pie.elf" "$out.elf"
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$out" --magic 0x1D3D154F
"$tool" self --inspect --file "$out" > /dev/null
echo "link-title.sh: $out ($(stat -c %s "$out") bytes)"
