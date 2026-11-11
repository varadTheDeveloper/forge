#!/usr/bin/env bash
# Runs Forge's libFuzzer targets for a fixed time each (rule R24). Phase 8.2:
# the Kavach path rules. A crash or a broken invariant fails the job and
# leaves the input that caused it in the working folder ("crash-...").
#
# Needs clang with compiler-rt (libFuzzer, ASan, UBSan).
# Usage (from the repository root): tools/ci/fuzz.sh [seconds per target]
set -euo pipefail

seconds="${1:-60}"
cd "$(dirname "$0")/../.."
out="${TMPDIR:-/tmp}/forge-fuzz"
mkdir -p "$out/corpus-pathrules"
cp kavach/fuzz/corpus/* "$out/corpus-pathrules/"

"${CXX:-clang++}" -std=c++17 -g -O1 -fno-exceptions -fsanitize=fuzzer,address,undefined \
  -fno-sanitize-recover=all -iquote . \
  kavach/fuzz/PathRulesFuzz.cpp kavach/PathRules.cpp \
  forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp \
  -o "$out/PathRulesFuzz"

"$out/PathRulesFuzz" -max_total_time="$seconds" -dict=kavach/fuzz/pathrules.dict \
  -print_final_stats=1 "$out/corpus-pathrules"
echo "[fuzz] PathRulesFuzz: no crash in $seconds s"
