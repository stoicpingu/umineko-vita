#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/upstream/onscripter-ru" \
  "$root/platform/vita/tests/test_gpu_tiling.cpp" -o "$out/tiling"
"$out/tiling"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/vendor/sdl-gpu/src" \
  "$root/platform/vita/tests/test_gpu_rows.c" -o "$out/rows"
"$out/rows"
