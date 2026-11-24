#!/usr/bin/env bash
# Builds the PS5 title PPSA64010 "Frequency" from this tree in Perfect Eye's build container:
#   host/ps5/build.sh            configure (once) and build build-ps5/host/resonance_host (the eboot)
#   host/ps5/build.sh package    then assemble out/PPSA64010 (eboot, sce_sys, sce_module)
# Needs P8 (default: two levels above the worktree's p8 parent) with docker/run.sh, PS5_Vulkan at
# probe/src/PS5_Vulkan with RADV built (P8/docker/build-radv.sh) and the PacBrew sysroot.
# RADV's source tree there takes host/ps5/radv-videoout-120hz.patch before it is built (patch -p1 in
# .deps/work/radv-src): the 119.88 Hz check measures again while the TV settles into the mode.
set -euo pipefail
tree=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
: "${P8:=$(cd -- "$tree/../.." && pwd)}"
radv=$P8/probe/src/PS5_Vulkan
pacbrew=$P8/ref/perfect-eye/.deps/pacbrew/v0.40.2/sysroot/user/homebrew
env="export PS5_PAYLOAD_SDK=$radv/.deps/native/ps5-payload-sdk RADV_ROOT=$radv PACBREW=$pacbrew \
RADV_INCLUDE_ROOT=$radv/.deps/native/radv-release;"
"$P8/docker/run.sh" "$env
    cd $tree && { [[ -f build-ps5/build.ninja ]] || cmake -S . -B build-ps5 -G Ninja -DRESONANCE_HOST=ON \
        -DENABLE_PATCHES=OFF -DCMAKE_TOOLCHAIN_FILE=$tree/cmake/ps5-toolchain.cmake > /dev/null; } &&
    cmake build-ps5 -DFREQ_VERSION=$(git -C "$tree" rev-parse --short=9 HEAD) > /dev/null &&
    cmake --build build-ps5 -j2 -- -k 0"
[[ ${1:-} == package ]] || exit 0
bash "$tree/host/ps5/check-imports.sh" "$tree/build-ps5/host/resonance_host.elf" "$radv"
id=PPSA64010
app=$P8/out/$id
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
cp "$tree/build-ps5/host/resonance_host" "$app/eboot.bin"
cp "$tree/host/ps5/sce_sys/"* "$app/sce_sys/"
cp "$radv/runtime/libc.prx" "$app/sce_module/libc.prx"
cp "$tree/host/frequency.ini.example" "$app/frequency.ini.example"
cp "$P8/ref/perfect-eye/assets/cacert.pem" "$app/cacert.pem"   # the updater's HTTPS (Perfect Eye's bundle)
cp "$tree/build-ps5/host/resonance_host.elf" "$P8/out/eboot-$id.elf"   # symbols, for crash addresses
echo "build.sh: $app ($(du -sh "$app" | cut -f1))"
