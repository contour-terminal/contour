#! /bin/bash

DIMMED="DIMMED █"
NORMAL="█ NORMAL █"
BOLD_OR_BRIGHT="█ BOLD/BRIGHT"

for i in {0..8} ; do
    printf "${i}: \e[0;2;3${i}m${DIMMED}\e[0;3${i}m${NORMAL}\e[0;1;3${i}m${BOLD_OR_BRIGHT}\e[0m\n"
done
