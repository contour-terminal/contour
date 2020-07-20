#! /bin/sh
# Copies stdin to operating-system clipboard.

contents=`cat | base64`
exec echo -ne "\033]52;c;$contents\033\\"
