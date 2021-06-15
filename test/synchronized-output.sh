#! /bin/bash

echo -ne "The following text should be printed after a short sleep \e[1;31minstantly\e[m.\n"
echo -ne "On non-conforming terminals, the text will be written in two chunks.\n\n"
sleep 0.01 # make sure that's actually flushed out

echo -ne "\033[?2026h"

echo -ne "Hello "
for i in `seq 1 10`; do
    echo -ne "."
    sleep 0.1
done
echo -ne " World\n"

echo -ne "\033[?2026l"
