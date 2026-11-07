#! /bin/bash
#
# Installs Komac, the tool that renders winget manifests and opens the pull request on
# microsoft/winget-pkgs, from its release's .deb on an Ubuntu runner.
#
# Bumping Komac means changing the next two lines: the version, and the SHA-256 of
# komac_<version>-1_amd64.deb as the release's SHA256SUMS lists it (check it against a download).
set -euo pipefail

KOMAC_VERSION=2.17.1
KOMAC_SHA256=6a44816c5414263ab4d517668d8a8ff1b2cbbc656a3a2b1e4be8ecba5a37c441

deb="${RUNNER_TEMP:-/tmp}/komac_${KOMAC_VERSION}-1_amd64.deb"
curl -fsSL --retry 3 --max-time 300 -o "${deb}" \
    "https://github.com/russellbanks/Komac/releases/download/v${KOMAC_VERSION}/komac_${KOMAC_VERSION}-1_amd64.deb"
echo "${KOMAC_SHA256}  ${deb}" | sha256sum -c -
sudo apt-get install -y "${deb}"
komac --version
