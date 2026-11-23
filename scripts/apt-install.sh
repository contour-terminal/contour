#! /bin/bash
# Runs `apt-get install -y` with retries and exponential backoff, the install-time counterpart of
# apt-update.sh. A repository can list a package and then refuse to serve it: on 2026-11-23 the
# ubuntu-toolchain-r PPA answered 503 to a share of its downloads for hours, and every job installing a
# GCC from it failed on whichever .deb drew one. apt's own retries come too fast to ride that out.
#
# What a failed attempt did download stays in apt's archive cache, so each retry fetches only what is
# still missing, and an outage that fails one request in a few converges in a couple of attempts.
#
# Tunable via APT_INSTALL_ATTEMPTS (default 6) and APT_INSTALL_RETRY_DELAY (default 10s, the first
# backoff; each subsequent wait doubles).
#
# Usage: sudo ./scripts/apt-install.sh <package>...
set -uo pipefail

attempts="${APT_INSTALL_ATTEMPTS:-6}"
delay="${APT_INSTALL_RETRY_DELAY:-10}"

for i in $(seq 1 "$attempts"); do
    if apt-get install -y -o Acquire::Retries=3 "$@"; then
        exit 0
    fi
    if [ "$i" -lt "$attempts" ]; then
        echo "::warning::apt install failed (attempt ${i}/${attempts}), retrying in ${delay}s…"
        sleep "$delay"
        delay=$((delay * 2))
    fi
done

echo "::error::apt install of $* failed after ${attempts} attempts"
exit 1
