#! /bin/bash

clear

echo -ne "\033[20l"

echo -ne "\033[5;5H"
for i in `seq 1 5`; do
    echo -ne "${i}\n"
done

echo -ne "\033[20h"
