#! /bin/bash

clear

echo -ne "123456789012345\n"
echo -ne "ABCDEFGHIJKLMNO\n"
echo -ne "ABCDEFGHIJKLMNO\n"
echo -ne "ABCDEFGHIJKLMNO\n"
echo -ne "ABCDEFGHIJKLMNO"

echo -ne "\033[?69h"    # DECLRMM enable
echo -ne "\033[2;8s"    # DECSLRM - set left/right margin
echo -ne "\033[2;4r"    # DECSTBM - set top/bottom margin
#sleep 1

# print from inside margins
echo -ne "\033[2;2H"
sleep 0.5
for i in `seq 1 9`; do
    echo -ne "${i}"
    sleep 0.5
done

# print from outside margins
echo -ne "\033[3;10H"
sleep 0.5
for i in `seq 1 9`; do
    echo -ne "${i}"
    sleep 0.5
done


# cleanup
echo -ne "\033[?69l"    # DECLRMM disable
echo -ne "\033[r"       # DECSTBM - set top/bottom margin
sleep 0.5

echo -ne "\033[6;1H"
echo "<CR>"
read
