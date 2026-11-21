#!/bin/sh
# Rebuilds lletest.elf and bench.elf (committed, so the normal build needs no ARM toolchain).
# Needs clang and ld.lld.
set -e
cd "$(dirname "$0")"
clang --target=armv6k-none-eabi -mfloat-abi=softfp -mfpu=vfpv2 -c lletest.S -o lletest.o
ld.lld -m armelf -Ttext=0x00100000 -Tdata=0x00180000 -e _start --no-dynamic-linker -o ../lletest.elf lletest.o
clang --target=armv6k-none-eabi -c bench.S -o bench.o
ld.lld -m armelf -Ttext=0x00100000 -e _start --no-dynamic-linker -o ../bench.elf bench.o
rm -f lletest.o bench.o
