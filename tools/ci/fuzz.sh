#!/usr/bin/env bash
# Runs Forge's libFuzzer targets for a fixed time each (rule R24). Phase 8.2:
# the Kavach path rules; Phase 9.1: module specifier resolution; Phase 9.2:
# the JSON parser and forge.json; Phase 10.1: the text and base64 encodings. A crash or a broken invariant fails the job and
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

mkdir -p "$out/corpus-specifiers"
cp runtime/fuzz/corpus/* "$out/corpus-specifiers/"

"${CXX:-clang++}" -std=c++17 -g -O1 -fno-exceptions -fsanitize=fuzzer,address,undefined \
  -fno-sanitize-recover=all -iquote . \
  runtime/fuzz/ModuleSpecifierFuzz.cpp runtime/ModuleSpecifier.cpp \
  forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp \
  -o "$out/ModuleSpecifierFuzz"

"$out/ModuleSpecifierFuzz" -max_total_time="$seconds" -print_final_stats=1 "$out/corpus-specifiers"
echo "[fuzz] ModuleSpecifierFuzz: no crash in $seconds s"

for target in JsonFuzz:json ManifestFuzz:manifest EncodingFuzz:encoding; do
  name="${target%%:*}"
  corpus="$out/corpus-${target##*:}"
  mkdir -p "$corpus"
  cp "runtime/fuzz/${target##*:}-corpus/"* "$corpus/"
  extra=runtime/Json.cpp
  [ "$name" = "ManifestFuzz" ] && extra="runtime/Manifest.cpp runtime/Json.cpp"
  [ "$name" = "EncodingFuzz" ] && extra=runtime/Encoding.cpp
  # shellcheck disable=SC2086
  "${CXX:-clang++}" -std=c++17 -g -O1 -fno-exceptions -fsanitize=fuzzer,address,undefined \
    -fno-sanitize-recover=all -iquote . \
    "runtime/fuzz/$name.cpp" $extra \
    forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp \
    -o "$out/$name"
  "$out/$name" -max_total_time="$seconds" -print_final_stats=1 "$corpus"
  echo "[fuzz] $name: no crash in $seconds s"
done
