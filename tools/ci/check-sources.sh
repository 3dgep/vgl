#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten
#
# Checks the repository rules that clang-format and clang-tidy do not cover:
#   - every source file carries an SPDX license identifier and a copyright line
#   - no text file contains an em dash (U+2014)
#
# Run from the repository root: tools/ci/check-sources.sh

set -euo pipefail

status=0

while IFS= read -r file; do
    header=$(head -n 8 "$file")
    if ! grep -q "SPDX-License-Identifier: MIT" <<<"$header" ||
        ! grep -q "Copyright (c)" <<<"$header"; then
        echo "error: $file: missing SPDX-License-Identifier or copyright line"
        status=1
    fi
done < <(git ls-files \
    '*.c' '*.h' '*.cpp' '*.hpp' '*.inl' '*.glsl' '*.hlsl' \
    '*.cmake' '*.cmake.in' '*.h.in' '*.pc.in' '*.sh' 'CMakeLists.txt' '*/CMakeLists.txt' \
    '.clang-format' '.clang-tidy')

em_dash=$'\xe2\x80\x94'
if git grep -n -I -F "$em_dash" -- . ; then
    echo "error: em dashes are not allowed; rephrase with a comma, colon or parentheses"
    status=1
fi

exit $status
