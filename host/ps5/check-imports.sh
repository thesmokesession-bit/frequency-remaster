#!/usr/bin/env bash
# Perfect Eye's tools/check-imports.sh for this title: lists the imports no usable system module
# provides (names only libScePosixForWebKit's or libkernel_sys's stubs define are null at run time;
# AGC's come from the RADV recipe's stubs). Prints the image's end with the 0x400000 load base.
#   check-imports.sh <eboot ELF> <PS5_Vulkan checkout>
set -euo pipefail
elf=$1
sdk=$2/.deps/native/ps5-payload-sdk
nm="$sdk/bin/llvm-nm"
good=$(for f in "$sdk"/target/lib/*.so; do
    case $(basename "$f" .so) in libScePosixForWebKit|libkernel_sys) continue ;; esac
    "$nm" -D --defined-only "$f" | awk '{print $3}'
done | sort -u)
bad=$(comm -23 <("$nm" -D --undefined-only "$elf" | awk '{print $2}' | sort -u) <(echo "$good") | grep -v '^sceAgc' || true)
end=$("$nm" "$elf" | awk '$3 == "_end" {print $1}')
printf 'check-imports.sh: image end 0x%x at load base 0x400000 (heap base 0x08000000)\n' $((0x400000 + 0x$end))
if [[ -n $bad ]]; then
    echo "check-imports.sh: imports that are null at run time:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "check-imports.sh: every import comes from a usable module"
