#!/bin/bash
# Builds each regression guest for the given architecture and runs it under
# the given ish against the given root, printing one line per test.
#
#   tests/regress/run.sh <ish binary> <rootfs dir> [i386|x86_64]
#
# The root is mounted with -r (realfs), so it is a plain directory: an
# unpacked Alpine minirootfs is enough. Needs LLVM's clang and lld (brew
# install llvm lld). Exits non-zero if any test fails.
set -uo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ish=$1 root=$2 arch=${3:-i386}
cc=${CC:-/opt/homebrew/opt/llvm/bin/clang}
out="$root/tmp/regress"
mkdir -p "$out"
failed=0
for src in "$here"/*.c; do
    name=$(basename "$src" .c)
    # _start is jumped to with the stack 16-aligned, where a called function
    # finds it 8 off, so realign it rather than have SSE spills fault
    "$cc" -target "$arch-linux" -nostdlib -static -ffreestanding -fno-stack-protector \
        -fno-pic -fuse-ld=lld -mstackrealign -O1 -o "$out/$name" "$src" || { echo "$name: did not build"; failed=1; continue; }
    result=$(timeout -s KILL 30 "$ish" -r "$root" "/tmp/regress/$name" 2>&1)
    rc=$?
    [ $rc -eq 0 ] && [ "$result" = ok ] || failed=1
    echo "$name: ${result:-no output} (rc $rc)"
done
exit $failed
