#! /bin/bash

wchar="$(printf '\u26a1')"

screen_goto() {
    local row=$1
    local col=$2
    echo -ne "\033[${row};${col}H"
}

screen_goto_column() {
    local col=$1
    echo -ne "\033[${col}G"
}

screen_clr() {
    clear
}

screen_write() {
    echo -ne "${@}"
}

clear
screen_write "ABCDEFG\n"
screen_write "${wchar}XYZ\n"

screen_write "${wchar}XYZ"
screen_goto_column 2
screen_write "AB"
#screen_goto_column 1
#screen_write "X"
screen_write "\n"
