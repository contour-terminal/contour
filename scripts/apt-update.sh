#! /bin/bash
# Runs `apt update` with retries and exponential backoff so a transiently-unreachable mirror does
# not flake the whole CI job. `apt update` exits non-zero (typically 100) if ANY configured
# repository fails to fetch, even when the packages the build actually needs are available; on
# GitHub's shared runners that happens intermittently, and an outage can outlast a couple of quick
# retries. Backing off (5s, 10s, 20s, 40s) rides out a longer hiccup.
#
# If the update still cannot complete after all attempts, WARN but exit successfully rather than
# failing the job: GitHub's runner images ship with pre-populated apt lists, so the subsequent
# `apt install` normally still resolves from cache. If a genuinely-needed package cannot be found,
# that install step fails with a precise, real error — a far more actionable failure than a blanket
# "apt update failed" caused by one unrelated, momentarily-unreachable mirror.
#
# Tunable via APT_UPDATE_ATTEMPTS (default 5) and APT_UPDATE_RETRY_DELAY (default 5s, the first
# backoff; each subsequent wait doubles).
#
# Usage: sudo ./scripts/apt-update.sh   (needs root to write /var/lib/apt/lists)
set -uo pipefail

attempts="${APT_UPDATE_ATTEMPTS:-5}"
delay="${APT_UPDATE_RETRY_DELAY:-5}"

for i in $(seq 1 "$attempts"); do
    if apt -q update; then
        exit 0
    fi
    if [ "$i" -lt "$attempts" ]; then
        echo "::warning::apt update failed (attempt ${i}/${attempts}), retrying in ${delay}s…"
        sleep "$delay"
        delay=$((delay * 2))
    fi
done

echo "::warning::apt update did not complete after ${attempts} attempts (likely a transient mirror" \
     "outage); continuing with the existing package lists. A later 'apt install' will fail loudly if" \
     "a required package is genuinely unavailable."
exit 0
