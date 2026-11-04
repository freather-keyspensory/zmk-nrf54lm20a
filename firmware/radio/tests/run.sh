#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
test_bin="$(mktemp "${TMPDIR:-/tmp}/minew-frame-test.XXXXXX")"
trap 'rm -f "$test_bin"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$root/frame_test.c" -o "$test_bin"
"$test_bin"
echo 'Radio diagnostic codec tests passed'
