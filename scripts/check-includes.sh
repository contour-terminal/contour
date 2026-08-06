#! /bin/bash

grep -R --include '*.cpp' -E '^#.*include ".*"$' src/
rv1=$?

# `*.h` alongside `*.hpp`: src/vtrasterizer/shared_defines.h keeps that extension because it is
# shared with GLSL, and the quoted-include rule applies to it just the same.
grep -R --include '*.hpp' --include '*.h' -E '^#.*include ".*"$' src/
rv2=$?

if [[ $rv1 -eq 0 || $rv2 -eq 0 ]]; then
    echo 1>&2 "Error: found #include \"...\" in C++ files."
    exit 1
else
    echo "All good. ;-)"
fi
