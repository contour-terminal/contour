#! /bin/bash
# Adds an apt PPA with retries and exponential backoff. `add-apt-repository` fetches the PPA's
# signing key from Launchpad's API, which on GitHub's shared runners intermittently times out or
# answers 504 and fails the whole job before anything is built. Backing off (5s, 10s, 20s, 40s)
# rides out a short outage.
#
# A longer one (the API timed out on and off for six hours on 2026-11-11) does not end in time. The
# packages themselves come from another host, ppa.launchpadcontent.net, so for a PPA whose public
# key is kept in scripts/ppa-keys/ the source is then written directly, signed by that key, with no
# call to the API. Each kept key is checked against the fingerprint Launchpad publishes for the PPA
# (`signing_key_fingerprint` at https://api.launchpad.net/devel/~OWNER/+archive/ubuntu/NAME) when it
# is added or replaced.
#
# Unlike apt-update.sh, a PPA that still cannot be added fails the step: what it provides (a newer
# GCC) is genuinely needed, and the install that follows could not succeed without it.
#
# Tunable via ADD_PPA_ATTEMPTS (default 5) and ADD_PPA_RETRY_DELAY (default 5s, the first backoff;
# each subsequent wait doubles).
#
# Usage: sudo ./scripts/add-ppa.sh ppa:ubuntu-toolchain-r/test
set -uo pipefail

if [ "$#" -ne 1 ] || [[ "$1" != ppa:*/* ]]; then
    echo "usage: $0 ppa:OWNER/NAME" >&2
    exit 2
fi

ppa="${1#ppa:}"
owner="${ppa%%/*}"
name="${ppa#*/}"
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

key="$(dirname "$0")/ppa-keys/${owner}_${name}.asc"
if [ ! -f "$key" ]; then
    echo "::error::could not add $1 after ${attempts} attempts, and no key for it is kept in ${key}"
    exit 1
fi

echo "::warning::Launchpad's API stayed unreachable; adding $1 with the key kept in ${key}"
codename="$(sed -n 's/^VERSION_CODENAME=//p' /etc/os-release)"
install -D -m 0644 "$key" "/etc/apt/keyrings/${owner}-${name}.asc"
cat > "/etc/apt/sources.list.d/${owner}-${name}.sources" <<EOF
Types: deb
URIs: https://ppa.launchpadcontent.net/${owner}/${name}/ubuntu/
Suites: ${codename}
Components: main
Signed-By: /etc/apt/keyrings/${owner}-${name}.asc
EOF
