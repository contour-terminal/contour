#!/bin/bash
#
# Runs "clang-format" on the commit before committing and prohibits commit if
# changes happened.

dir_list="$PWD"  # Add the directories you want here
cmd="git diff -U0 --no-color --staged HEAD -- $dir_list | clang-format-diff -p1"

echo ""
echo "Running clang-format on this commit"
echo ""

diff=$(eval "$cmd")
if [[ $? -ne 0 ]]
then
    echo "Command failed to execute."
    exit 1
fi

if [[ -z "$diff" ]]
then
    echo "Everything is clean"
    exit 0
else
    echo 1>&2 "$diff"
    echo 1>&2 ""
    echo 1>&2 "Commit aborted due to code format inconsistencies."
    exit 1
fi
