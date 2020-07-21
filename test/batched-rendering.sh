#! /bin/bash

if [[ "$1" == "sync" ]]; then
    synchronous=1
else
    synchronous=0
fi

batch_start() {
    echo -ne "\033[?2001h"
}

batch_end() {
    echo -ne "\033[?2001l"
}

trapped() {
    echo -ne "\nTrapped.\n"
    batch_end
    exit 0
}

trap trapped INT

echo -ne "\033[1;1H"
echo -ne "\033[2J"

echo "prepare me"
sleep 5

i=0
while [[ i -lt 5 ]]; do
    i=$[i + 1]

    test $synchronous -ne 0 && batch_start

    echo -ne "${i}: .... "
    sleep 1
    echo -ne "... done\n"
    sleep 1

    test $synchronous -ne 0 && batch_end
done
