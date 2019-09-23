#! /bin/bash

for i in `seq 0 7`; do
    for ground in 3 4; do
        for b in "" "1;"; do # bold, non-bold
            echo -ne "\033[${b}${ground}${i}m HELLO \033[m "
        done
    done
    echo -ne "\n"
done
