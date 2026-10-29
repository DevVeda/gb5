#!/bin/sh
set -e
cd "$(dirname "$0")"

if command -v brew >/dev/null; then pm=brew
elif command -v apt-get >/dev/null; then pm=apt
elif command -v dnf >/dev/null; then pm=dnf
elif command -v pacman >/dev/null; then pm=pacman
else pm=; fi

# pkg <brew> <apt> <dnf> <pacman>: install with whichever package manager this system has
pkg() {
    case $pm in
        brew) brew install $1 ;;
        apt) sudo apt-get update -qq && sudo apt-get install -y $2 ;;
        dnf) sudo dnf install -y $3 ;;
        pacman) sudo pacman -S --needed --noconfirm $4 ;;
        *) echo "!! no known package manager: install $2 yourself"; exit 1 ;;
    esac
}
has_python() { command -v python3 >/dev/null && python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))'; }

has_python || pkg python python3 python3 python
has_python || { echo "!! Python 3.10+ is still missing"; exit 1; }
echo "ok  $(python3 --version)"

if [ "$(uname)" = Darwin ]; then
    if ! xcode-select -p >/dev/null 2>&1; then  # /usr/bin/cc is only a shim until the Command Line Tools exist
        xcode-select --install || true
        echo "!! finish the Command Line Tools install, then run ./setup.sh again"; exit 1
    fi
elif ! command -v cc >/dev/null; then
    pkg - build-essential "gcc make" base-devel
fi
echo "ok  C compiler ($(command -v cc))"

if [ "${1:-}" = --runtime ]; then
    export PATH="/opt/homebrew/opt/rustup/bin:$HOME/.cargo/bin:$PATH"  # Homebrew's rustup is keg-only, as in build.sh
    command -v rustup >/dev/null || pkg rustup rustup rustup rustup \
        || { echo "!! install rustup from https://rustup.rs, then run ./setup.sh --runtime again"; exit 1; }
    rustup default >/dev/null 2>&1 || rustup default stable
    rustup target add x86_64-unknown-freebsd
    echo "ok  $(rustc --version) + x86_64-unknown-freebsd"

    # ponytail: dnf/pacman package names for LLVM 18 vary by release; those systems set LLVM_CONFIG by hand
    case $pm in
        brew) [ -x /opt/homebrew/opt/llvm@18/bin/llvm-config ] || pkg llvm@18 ;;
        apt) [ -x /usr/lib/llvm-18/bin/llvm-config ] || pkg - "llvm-18 clang-18 lld-18" ;;
        *) echo "--  LLVM 18: install it and set LLVM_CONFIG for make -C tools" ;;
    esac

    # These can't be installed from here: report what's missing.
    [ -f runtime/local.env ] && . runtime/local.env
    docker image inspect "${IMAGE:-ps5title}" >/dev/null 2>&1 && echo "ok  Docker image ${IMAGE:-ps5title}" \
        || echo "--  Docker image ${IMAGE:-ps5title} missing (Arch amd64 + clang 18; see README, Requirements)"
    [ -d "${PS5GPU:-$HOME/ps5gpu}/PS5_Vulkan" ] && echo "ok  PS5_Vulkan in ${PS5GPU:-$HOME/ps5gpu}" \
        || echo "--  PS5_Vulkan + PS5_VulkanTemplate missing in ${PS5GPU:-$HOME/ps5gpu} (https://github.com/mihawk-99/PS5_Vulkan; or PS5GPU= in runtime/local.env)"
    sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5-payload-dev/opt/ps5-payload-sdk}
    [ -d "$sdk" ] && echo "ok  ps5-payload-sdk in $sdk" \
        || echo "--  ps5-payload-sdk missing in $sdk (https://github.com/ps5-payload-dev/sdk; or set PS5_PAYLOAD_SDK)"
fi

python3 gb5.py --help >/dev/null && echo "ok  gb5.py runs. Next: python3 gb5.py serve"
