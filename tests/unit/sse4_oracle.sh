#!/bin/bash
# Rewrites tests/unit/sse4_expected.h from what this machine's x86-64 CPU
# gives for every case in sse4_cases.h: natively on x86-64, under Rosetta 2
# on Apple silicon. Run it after changing the cases.
#
#   tests/unit/sse4_oracle.sh            regenerate sse4_expected.h
#   tests/unit/sse4_oracle.sh <case>...  print those cases' inputs and outputs
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
out=$(mktemp -d)
trap '/bin/rm -rf "$out"' EXIT
if [ "$(uname -s)" = Darwin ]; then
    clang -arch x86_64 -O1 -o "$out/oracle" "$here/sse4_oracle.c"
else
    cc -O1 -o "$out/oracle" "$here/sse4_oracle.c"
fi
if [ $# -gt 0 ]; then
    "$out/oracle" "$@"
else
    "$out/oracle" > "$out/expected.h"
    /bin/mv "$out/expected.h" "$here/sse4_expected.h"
fi
