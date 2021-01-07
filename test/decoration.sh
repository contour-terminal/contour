#! /bin/bash

function decoration_color() {
    local color="${1:-250:250:70}"
    echo -ne "\033[58:2:${color}m"
}

function decoration() {
    local on="${1}"
    local name="${2}"
    local off="${3}"
    local color="${4:-250:250:70}"
    decoration_color "${color}"
    echo -ne "${on}\t| \033[${on}m${name}\033[${off}m\n"
}

decoration "4" "single underline"
decoration "4:0" "no underline"
decoration "4:1" "single underline"
decoration "4:2" "doubly underline"
decoration "4:3" "curly underline"
decoration "4:4" "dotted underline"
decoration "4:5" "dashed underline"
decoration 51 "framed" 54 "170:170:255"
decoration 53 "overline" 55 "250:30:250"

# Decoration on the same line right next to each other
decoration_color "250:250:50"
echo -ne "\033[4:1m"
echo -ne "Same"
echo -ne "\033[0;4:2m"
decoration_color "50:250:250"
echo -ne "Line"
echo -ne "\033[m\n"
