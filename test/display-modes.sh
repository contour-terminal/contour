#!/usr/bin/env bash
# Test DEC private mode queries (DECRQM)
# Queries modes and displays responses with interpreted values
set -e
old_settings=$(stty -g)

cleanup() {
    stty "$old_settings"
}
trap cleanup EXIT INT TERM

stty raw -echo min 0 time 0
for mode in {1..10}; do
    # Send mode query
    printf 'Query mode %d: \033[?%d$p' "$mode" "$mode"

    # Read response until '$y' pattern (end of response) or timeout
    response=""
    start_ms=$(($(date +%s%3N)))
    timeout_ms=1000

    while true; do
        elapsed_ms=$(($(date +%s%3N) - start_ms))
        if [ "$elapsed_ms" -ge "$timeout_ms" ]; then
            # timeout
            break
        fi

        # Read one character at a time with short timeout
        if IFS= read -r -t 0.1 -n 1 char; then
            response="${response}${char}"
            # Check if we got the terminator
            if [[ "$response" == *'$y' ]]; then
                break
            fi
        fi
    done

    elapsed_ms=$(($(date +%s%3N) - start_ms))
    printf 'response=%q after %dms\r\n' "$response" "$elapsed_ms"
done
