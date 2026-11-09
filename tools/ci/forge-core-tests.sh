#!/usr/bin/env bash
# Builds and runs forge-core's portable unit tests with g++ under
# AddressSanitizer and UndefinedBehaviorSanitizer (Phase 7.9). Windows-only
# components (File, Socket, threads, IOCP) are covered by
# forge-core-tests.ps1 on a Windows runner instead.
#
# Usage (from the repository root): tools/ci/forge-core-tests.sh
set -euo pipefail

cd "$(dirname "$0")/../../forge-core"
out="${TMPDIR:-/tmp}/forge-core-tests"
mkdir -p "$out"

flags=(-std=c++17 -fno-exceptions -g -O1 -Wall -Wextra
       -fsanitize=address,undefined -fno-sanitize-recover=all)
mem=(memory/DefaultAllocator.cpp memory/detail/AllocationBackend.cpp)

# OverflowTest deliberately asks for impossible allocations; ASan must
# return null for those instead of aborting.
export ASAN_OPTIONS=allocator_may_return_null=1

failures=0

for t in ArrayTest ErasedCallableTest HashMapTest HashSetTest IpAddressTest \
         OverflowTest PathTest QueueTest StackTest StringTest; do
  if g++ "${flags[@]}" "$t.cpp" "${mem[@]}" -o "$out/$t" && "$out/$t" > "$out/$t.log" 2>&1; then
    echo "[forge-core] $t PASS"
  else
    echo "[forge-core] $t FAIL"
    cat "$out/$t.log" 2>/dev/null || true
    failures=$((failures + 1))
  fi
done

if [ "$failures" -ne 0 ]; then
  echo "[forge-core] $failures FAILED"
  exit 1
fi

echo "[forge-core] ALL PASSED"
