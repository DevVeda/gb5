#!/usr/bin/env bash
# Build the GBA title's eboot.bin -> runtime/dist/{eboot.bin, sce_module/libc.prx}
#
#   runtime/build.sh
#
# 1. On this machine: gba_rt (Rust, no_std) for x86_64-unknown-freebsd.
# 2. In the ps5title Docker image (Linux x86-64: the native tool is a Linux binary): compile ps5/main.c,
#    link with PS5_Vulkan's title recipe (CRT, platform layer, libc wraps) minus RADV, then
#    ps5-native-tool link + fake-SELF sign, as PS5_VulkanTemplate's ps5/tools/link-title.sh does.
#
#   PS5GPU   checkout holding PS5_Vulkan and PS5_VulkanTemplate (default ~/ps5gpu)
#   IMAGE    Docker image with clang 18 (default ps5title)
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ ${1:-} != --in-docker ]]; then
    # shellcheck source=/dev/null
    [[ -f $here/local.env ]] && source "$here/local.env"  # this machine's paths, kept out of git
    ps5gpu=${PS5GPU:-$HOME/ps5gpu}
    export PATH="/opt/homebrew/opt/rustup/bin:$HOME/.cargo/bin:$PATH"
    (cd "$here" && cargo build --release --target x86_64-unknown-freebsd)
    # GB5's version for RetroAchievements' User-Agent: the last tag (v0.2.1 -> 0.2.1)
    ver=$(git -C "$here" describe --tags --abbrev=0 2>/dev/null || echo 0.0.0)
    core=$(sed -n 's/^version = "\(.*\)"/\1/p' "$here/vendor/gba-core/Cargo.toml" | head -n 1)  # the core's version, same UA
    exec docker run --rm --platform linux/amd64 -e GB5_VERSION="${ver#v}" -e CORE_VERSION="${core:-0.0.0}" -v "$ps5gpu:/work" -v "$here:/rt" -w /rt "${IMAGE:-ps5title}" \
        bash /rt/build.sh --in-docker
fi

# ---- inside the container ----
vulkan=/work/PS5_Vulkan
sdk=/work/PS5_VulkanTemplate/.deps/native/ps5-payload-sdk  # the template's SDK pin, with libps5platform.a
tool=$vulkan/build/host/ps5-native-tool
native=$vulkan/tooling/native
out=/rt/dist
work=/rt/target/title
mkdir -p "$work" "$out/sce_module"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

rc=/rt/vendor/rcheevos
objs=()
for src in /rt/ps5/main.c /rt/ps5/ra.c /rt/ps5/card.c /rt/ps5/menu.c /rt/ps5/cheats.c /rt/ps5/rewind.c $rc/src/*.c $rc/src/rapi/*.c $rc/src/rcheevos/*.c $rc/src/rhash/md5.c; do
    obj="$work/obj/$(basename "$(dirname "$src")")_$(basename "$src" .c).o"
    mkdir -p "$work/obj"
    case $src in /rt/ps5/*) warn="-Wall -Wno-unused-function -DGB5_VERSION=\"${GB5_VERSION:-0.0.0}\" -DCORE_VERSION=\"${CORE_VERSION:-0.0.0}\"" ;; *) warn=-w ;; esac  # vendored code: no warnings
    cc -std=gnu11 -O2 $warn -I"$rc/include" -I"$rc/src" -I/rt/vendor/stb -c "$src" -o "$obj"
    objs+=("$obj")
done
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/app_crt.o"
"$sdk/bin/llvm-ar" rc "$work/none.a"  # the recipe wants a RADV archive; this title has none

# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$work/none.a"
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL --gc-sections \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/app_crt.o" "${objs[@]}" /rt/target/x86_64-unknown-freebsd/release/libgba_rt.a \
    "${radv_link_inputs[@]}" --as-needed "$sdk"/target/lib/*.so

# An import no module a title loads exports is null at run time (first call jumps to 0): refuse it here.
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for lib in "$sdk"/target/lib/*.so; do
        case ${lib##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$lib" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "build.sh: imports a title cannot resolve: ${null_imports//$'\n'/ }" >&2
    exit 1
fi
phdrs=$("$(dirname "$PS5_CLANG")/llvm-readelf" -lW "$work/llvm-pie.elf")
if grep -qE '^\s+TLS' <<<"$phdrs"; then
    echo "build.sh: the ELF has a TLS segment, which a title cannot load" >&2
    exit 1
fi

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$out/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$out/eboot.bin" > /dev/null
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$out/sce_module/libc.prx"
echo "==> $out/eboot.bin ($(stat -c %s "$out/eboot.bin") bytes)"
