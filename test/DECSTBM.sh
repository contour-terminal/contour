#! /bin/bash

clear
echo -ne "\033[5;10r"
for i in `seq 1 15`; do
    echo "${i}: Hello, World"
    sleep 1
done
sleep 5
echo -ne "\033[r"
echo "ENDING..."
echo -ne "\033[10;1H"
