#!/bin/bash
  exit 0
#
# Runs "clang-format" on the commit before committing and prohibits commit if
# changes happened.

command -v clang-format &>/dev/null
if [[ $? -ne 0 ]]
then
  echo "'clang-format' binary not found in PATH..." >&2
  exit 2
fi

command -v git-clang-format &>/dev/null
if [[ $? -ne 0 ]]
then
  echo "'git-clang-format' script not found in PATH..." >&2
  exit 2
fi

STATUS_CMD="git status --porcelain=1 --untracked-files=no --ignored=no"

FILES_ADDED_IN_INDEX=$(${STATUS_CMD} | grep "^[MARC]" | awk '{ print $2; }')

STATUS=$(${STATUS_CMD})

git-clang-format ${FILES_ADDED_IN_INDEX}
if [[ $? -ne 0 ]]
then
  exit 3
fi

STATUS_AFTER=$(${STATUS_CMD})

# Calculate the difference git-clang-format's call made, and then cut to the
# the new (inserted) lines only. Also remove the insertion marker "+".
DIFF=$(diff -U 0 <(echo "${STATUS}") <(echo "${STATUS_AFTER}") |
    grep -v "^+++ \|^--- \|^@@" |
    grep "^\+" |
    cut -c2-)

# Get the second status character from the changes.
# First letter would be the index status, second letter is the workdir->index
# diff. The latter one interests us.
LETTERS=$(echo "${DIFF}" |
  awk -F='\n' '{ print substr($0,0,2); }' |
  cut -c2)

echo "${LETTERS}" | grep "M" >/dev/null
if [[ $? -ne 0 ]]
then
  # grep did not match any locally modified files in the diff, this means
  # clang-format did not modify anything.
  exit 0
else
  echo "Modified files were detected after clang-format."
  echo "Please either add the changes, or unstage them."
  exit 1
fi
