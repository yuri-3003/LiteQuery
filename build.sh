#!/usr/bin/env bash
# One-command build for LiteQuery (Linux / macOS).
#
#   ./build.sh            configure + build (Release) + run tests
#   ./build.sh --debug    Debug build
#   ./build.sh --asan     Debug build with AddressSanitizer + UBSan
#   ./build.sh --no-test  skip running ctest
set -euo pipefail

BUILD_TYPE=Release
ASAN=OFF
RUN_TESTS=1

for arg in "$@"; do
  case "$arg" in
    --debug)   BUILD_TYPE=Debug ;;
    --asan)    BUILD_TYPE=Debug; ASAN=ON ;;
    --no-test) RUN_TESTS=0 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 1 ;;
  esac
done

cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DLITEQUERY_ASAN="$ASAN"
cmake --build build --parallel

if [ "$RUN_TESTS" -eq 1 ]; then
  ctest --test-dir build --output-on-failure
fi

echo
echo "Built: build/liblitequery.a"
echo "Try:   ./build/lq_demo"
