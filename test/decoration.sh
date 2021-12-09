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
    local bar="║" # │
    decoration_color "${color}"
    echo -ne "${on}\t${bar} \033[${on}m${name}\033[${off}m\n"
}

# Reference: https://en.wikipedia.org/wiki/ANSI_escape_code#SGR_(Select_Graphic_Rendition)_parameters

# decoration "0" "Reset or normal"
# decoration "1" "Bold or increased intensity"
# decoration "2" "Faint, decreased intensity, or dim"
# decoration "3" "Italic"
# decoration "4" "Single underline"
# decoration "4:0" "No underline"
decoration "4:1" "Single underline"
decoration "4:2" "Doubly underline"
decoration "4:3" "Curly underline"
decoration "4:4" "Dotted underline"
decoration "4:5" "Dashed underline"
# decoration "5" "Slow blink"
# decoration "6" "Rapid blink"
# decoration "7" "Reverse video or invert"
# decoration "8" "Conceal or hide"
# decoration "9" "Crossed-out or strike"
# decoration 21 "Doubly underline or not bold"
# decoration 22 "Normal intensity"
# decoration 23 "Neither italic, nor blackletter"
# decoration 24 "Not underlined"
# decoration 25 "Not blinking"
# decoration 26 "Proportional spacing"
# decoration 27 "Not reversed"
# decoration 28 "Reveal or not concealed"
# decoration 29 "Not crossed out"
# decoration 50 "Disable proportional spacing"
# decoration 51 "Framed" 54 "170:170:255"
# decoration 52 "Encircled"
# decoration 53 "Overlined" 55 "250:30:250"
# decoration 54 "Neither framed nor encircled"
# decoration 55 "Not overlined"
# #decoration 58 "Set underline color"
# decoration 59 "Default underline color"
# decoration 73 "Superscript"
# decoration 74 "Subscript"
# decoration 75 "Neither superscript nor subscript"

# Decoration on the same line right next to each other
decoration_color "250:250:50"
echo -ne "\033[4:1m"
echo -ne "Same"
echo -ne "\033[0;4:2m"
decoration_color "50:250:250"
echo -ne "Line"
echo -ne "\033[m\n"
read
