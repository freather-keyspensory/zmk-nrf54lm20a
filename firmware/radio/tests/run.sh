#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
test_bin="$(mktemp "${TMPDIR:-/tmp}/minew-frame-test.XXXXXX")"
trap 'rm -f "$test_bin"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$root/frame_test.c" -o "$test_bin"
"$test_bin"
echo 'Radio diagnostic codec tests passed'
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$root/reliable_test.c" -o "$test_bin"
"$test_bin"
echo 'Reliable codec, backpressure, ACK-loss deduplication and session tests passed'
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DARQ_BATCH=10 -DARQ_WIRE_VERSION=3 "$root/reliable_test.c" -o "$test_bin"
"$test_bin"
echo 'Ten-sample secure inner codec bounds and wrap tests passed'
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DARQ_BATCH=10 -DARQ_WIRE_VERSION=4 "$root/reliable_test.c" -o "$test_bin"
"$test_bin"
echo 'Cumulative window ACK, selective suffix retry, duplicate history and poll tests passed'
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$root/dtls_record_test.c" -o "$test_bin"
"$test_bin"
echo 'DTLS datagram framing, truncation and coalesced-reconnect gates passed'
python3 "$root/test_telemetry.py"
