#! /bin/bash

echo -ne "\033[65;1\"p" # VT level 4, 7-bit controls (needed for L/R margin)
echo -ne "\033[?69h"    # allow L/R margin mode
echo -ne "\033[5;15s"   # left/right margin

echo -ne "\033[5;15r"   # top/bottom margin

echo -ne "\033[?6h"     # enable Origin mode
