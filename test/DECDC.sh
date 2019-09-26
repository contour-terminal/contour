#! /bin/bash


clear

echo -ne "\033[4;5Hx\033[4;15Hx"

echo -ne "\033[65;1\"p" # VT level 4, 7-bit controls (needed for L/R margin)
echo -ne "\033[?69h"    # allow L/R margin mode
echo -ne "\033[5;15s"   # horizontal margin
echo -ne "\033[5;15r"   # vertical margin

echo -ne "\033[44;37m"  # set SGR

echo -ne "\033[5;5H"   # move cursor to row 5 col 5
echo -ne "1234567890"  # fill line

echo -ne "\033[6;5H"   # move cursor to row 6 col 5
echo -ne "1234567890"  # fill line
echo -ne "\033[6;6H"   # move cursor to 6;10
echo -ne "\033[5'~"    # DECDC: delete 5 columns

# reset
echo -ne "\033[m" # reset SGR
echo -ne "\033[?69l" # disable horizontal margin
#echo -ne "\033[r"
#echo -ne "\033[s"

echo -ne "\033[15;1H"
