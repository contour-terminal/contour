#! /bin/bash
clear

# draw left line
for i in `seq 1 30`; do
    echo -ne "\033[${i};1H"
    echo -n "| $i ..."
done

# draw top line
echo -ne "\033[4;10H"
echo -ne "========================================================="

# draw bottom line
echo -ne "\033[16;10H"
echo -ne "========================================================="

echo -ne "\033[5;15r"
echo -ne "\033[15;1H"

for i in a b c d; do
    sleep 1
    echo -ne "\t${i}\n"
done

# restore default
sleep 1
echo -ne "\033[r"
echo -ne "\033[28H"
