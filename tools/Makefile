# register.elf for elfldr: make -C tools
PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-dev/opt/ps5-payload-sdk
# LLVM 18: Homebrew (Mac), apt's llvm-18 (Debian/Ubuntu/WSL), else whatever llvm-config is on PATH
LLVM_CONFIG     ?= $(firstword $(wildcard /opt/homebrew/opt/llvm@18/bin/llvm-config /usr/lib/llvm-18/bin/llvm-config) llvm-config)
export LLVM_CONFIG
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

register.elf: register.c
	$(CC) -O2 -Wall -o $@ $<
