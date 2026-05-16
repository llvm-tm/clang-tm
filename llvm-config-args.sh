#!/bin/bash

# Try versioned llvm-config first (apt.llvm.org), then bare
for cfg in llvm-config-22 llvm-config-22.1 llvm-config; do
    if command -v "$cfg" &>/dev/null; then
        exec "$cfg" --cxxflags --ldflags --libs core passes
    fi
done

echo "llvm-config not found" >&2
exit 1
