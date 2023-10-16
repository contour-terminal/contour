#! /bin/bash

grep -R --include '*.cpp' -E '^#.*include ".*"$' src/
rv1=$?

grep -R --include '*.h' -E '^#.*include ".*"$' src/
rv2=$?

if [[ $rv1 -eq 0 || $rv2 -eq 0 ]]; then
    echo 1>&2 "Error: found #include \"...\" in C++ files."
    exit 1
else
    echo "All good. ;-)"
fi
