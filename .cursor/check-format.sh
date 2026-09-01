#!/usr/bin/env bash
# Reproduce the enforced clang-format CI check locally.
#
# Mirrors .github/workflows/clang-format.yml, which runs
# jidicula/clang-format-action (clang-format v20) with:
#   check-path:    OptiScaler
#   exclude-regex: external/|OptiScaler/include/
#
# Exits non-zero (and prints the required diff locations) if any tracked
# C/C++ source under OptiScaler/ is not formatted per .clang-format.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

mapfile -t files < <(
    find OptiScaler -type f \
        \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \
        -o -name '*.h' -o -name '*.hpp' -o -name '*.hh' -o -name '*.hxx' \) \
    | grep -Ev 'external/|OptiScaler/include/' \
    | sort
)

echo "Checking ${#files[@]} files with $(clang-format --version)"
clang-format --dry-run --Werror --style=file "${files[@]}"
echo "clang-format: all checked files are correctly formatted."
