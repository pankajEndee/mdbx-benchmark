#!/bin/bash
set -euo pipefail
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
