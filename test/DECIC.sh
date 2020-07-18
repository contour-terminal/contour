#! /bin/bash

clear

echo -ne "1234567890\n"
echo -ne "ABCDEFGHIJ\n"
echo -ne "ABCDEFGHIJ\n"
echo -ne "ABCDEFGHIJ\n"
echo -ne "ABCDEFGHIJ"

echo -ne "\033[?69h"    # DECLRMM enable
echo -ne "\033[2;8s"    # DECSLRM - set left/right margin
echo -ne "\033[2;4r"    # DECSTBM - set top/bottom margin
sleep 1

echo -ne "\033[2;2H"
sleep 1

for i in `seq 1 5`; do
    echo -ne "\033[1'}"     # DECIC - insert column 1
    sleep 1
done

echo -ne "\033[?69l"    # DECLRMM disable
echo -ne "\033[r"       # DECSTBM - set top/bottom margin
sleep 1

echo -ne "\033[6;1H"
echo "<CR>"
read
