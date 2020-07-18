#! /bin/bash

function decoration() {
    local on="${1}"
    local name="${2}"
    local off="${3}"
    echo -ne "\033[58:2:250:250:70m"
    echo -ne "${on}\t| \033[${on}m${name}\033[${off}m\n"
}

decoration "4" "single underline"
decoration "4:0" "no underline"
decoration "4:1" "single underline"
decoration "4:2" "doubly underline"
decoration "4:3" "curly underline"
decoration "4:4" "dotted underline"
decoration "4:5" "dashed underline"
decoration 53 "overline" 55
