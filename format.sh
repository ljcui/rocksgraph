#!/bin/bash
find . -type f \( -name "*.h" -o -name "*.cc" \) | xargs clang-format -i -style=file
