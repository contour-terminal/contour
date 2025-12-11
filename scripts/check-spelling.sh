#! /usr/bin/env bash
set -e

ROOT_DIR="$(dirname "$0")/.."
DIRECTORIES_TO_CHECK="${1:-**}"

# Define the command to run cspell
if command -v cspell >/dev/null 2>&1; then
    CSPELL="cspell"
elif command -v npx >/dev/null 2>&1; then
    CSPELL="npx --yes cspell"
else
    echo "Error: cspell not found and npx not available. Please install Node.js and npm."
    exit 1
fi

echo "Running spellcheck with $CSPELL..."
$CSPELL lint --config "${ROOT_DIR}/cspell.json" "$DIRECTORIES_TO_CHECK"
