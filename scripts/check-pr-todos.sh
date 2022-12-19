#! /bin/bash
FOUND=$(git grep "TODO(pr)" | grep -v "scripts/check-pr-todos.sh")
if [[ "${FOUND}" == "" ]]; then
    exit 0
fi

FOUND=$(git grep "crispy::todo(" | grep -v "scripts/check-pr-todos.sh")
if [[ "${FOUND}" == "" ]]; then
    exit 0
fi

echo "This PR still contains PR-related TODO itmes that must be resolved."
echo
echo "${FOUND}"
exit 1
