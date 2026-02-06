#!/bin/bash
set -euo pipefail

FIX_ARGS=()
if [[ "${1:-}" == "--fix" ]]; then
  FIX_ARGS=(-fix -format)
fi

rm build -rf
cmake -S . -B build -DROCKSGRAPH_ENABLE_CLANG_TIDY=ON
cmake --build build -j8

run-clang-tidy -p build/ -j 8 "${FIX_ARGS[@]}" "^(?!.*ast/cypher/).*$"

find . -type f \( -name "*.h" -o -name "*.cc" \) | xargs clang-format -i -style=file
