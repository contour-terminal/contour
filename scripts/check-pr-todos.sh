#! /bin/bash

if ! git grep "TODO(pr)" | grep -v "scripts/check-pr-todos.sh" | cat; then
    exit 0
fi

echo 1>&2 "This PR still contains PR-related TODO itmes that must be resolved."
exit 1
