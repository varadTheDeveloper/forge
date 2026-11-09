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

# Built twice: C++17 (the current 142 engine build) and C++20 (what the
# ESR 153 engine requires of embedders, Phase 7.95). Code must pass both.
common=(-fno-exceptions -g -O1 -Wall -Wextra
        -fsanitize=address,undefined -fno-sanitize-recover=all)
mem=(memory/DefaultAllocator.cpp memory/detail/AllocationBackend.cpp)

# OverflowTest deliberately asks for impossible allocations; ASan must
# return null for those instead of aborting.
export ASAN_OPTIONS=allocator_may_return_null=1

failures=0

for std in c++17 c++20; do
  for t in ArrayTest ErasedCallableTest HashMapTest HashSetTest IpAddressTest \
           OverflowTest PathTest QueueTest StackTest StringTest; do
    exe="$out/$t-$std"
    if g++ -std=$std "${common[@]}" "$t.cpp" "${mem[@]}" -o "$exe" && "$exe" > "$exe.log" 2>&1; then
      echo "[forge-core] $t ($std) PASS"
    else
      echo "[forge-core] $t ($std) FAIL"
      cat "$exe.log" 2>/dev/null || true
      failures=$((failures + 1))
    fi
  done
done

if [ "$failures" -ne 0 ]; then
  echo "[forge-core] $failures FAILED"
  exit 1
fi

echo "[forge-core] ALL PASSED"
