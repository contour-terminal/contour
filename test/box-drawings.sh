#! /bin/bash

FRAMED="\e[51m"
YELLOW="\e[1;33m"
GREEN="\e[1;34m"
RESET="\e[m"

for i in `seq 1 5`; do echo -ne "║│╎┆┊┃╏┇┋\n"; done
for i in `seq 1 5`; do echo -ne "|──|╌╌|┄┄|┈┈|\n"; done

echo -ne "${FRAMED}${YELLOW}"

# echo -ne "\t\u256D\u2500\n"
# echo -ne "\t\u2570\u2500\n"
echo -ne "  \u256D\u2500\u256E\n"
echo -ne "  \u2570\u2500\u256F\n"

echo -ne "${RESET}"

read
