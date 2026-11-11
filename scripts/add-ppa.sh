#! /bin/bash
# Adds an apt PPA with retries and exponential backoff. `add-apt-repository` fetches the PPA's
# signing key from Launchpad's API, which on GitHub's shared runners intermittently times out or
# answers 504 and fails the whole job before anything is built. Backing off (5s, 10s, 20s, 40s)
# rides out such an outage.
#
# Unlike apt-update.sh, a PPA that still cannot be added fails the step: what it provides (a newer
# GCC) is genuinely needed, and the install that follows could not succeed without it.
#
# Tunable via ADD_PPA_ATTEMPTS (default 5) and ADD_PPA_RETRY_DELAY (default 5s, the first backoff;
# each subsequent wait doubles).
#
# Usage: sudo ./scripts/add-ppa.sh ppa:ubuntu-toolchain-r/test
set -uo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 ppa:OWNER/NAME" >&2
    exit 2
fi

attempts="${ADD_PPA_ATTEMPTS:-5}"
delay="${ADD_PPA_RETRY_DELAY:-5}"

for i in $(seq 1 "$attempts"); do
    if add-apt-repository -y "$1"; then
        exit 0
    fi
    if [ "$i" -lt "$attempts" ]; then
        echo "::warning::add-apt-repository $1 failed (attempt ${i}/${attempts}), retrying in ${delay}s…"
        sleep "$delay"
        delay=$((delay * 2))
    fi
done

echo "::error::could not add $1 after ${attempts} attempts"
exit 1
