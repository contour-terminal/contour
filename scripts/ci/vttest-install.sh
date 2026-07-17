#! /bin/bash
# SPDX-License-Identifier: Apache-2.0
set -ex

# Installs vttest, the VT test program the conformance harness drives.
#
# vttest is a TEST-ONLY dependency: nothing of it is compiled, linked or shipped. The harness spawns
# it as an external program over a PTY (see src/vtconformance/), exactly as it does esctest.
#
# THE VERSION MUST MATCH EXACTLY, and that is not fussiness. The Replay-mode scenarios drive vttest
# with `-c` against recorded command files (src/vtconformance/test/cmd/*.cmd), and such a file carries
# one entry per read vttest performs. A build that reads a different number of times puts the replay
# out of step: vttest runs off the end of the file, never reaches its exit, and the harness reports a
# truncated transcript. So "some vttest" is not good enough -- it has to be the one the files were
# recorded against.
#
# This used to accept whatever vttest happened to be on PATH, then whatever the distro packaged, and
# only fell back to the pinned source build if neither existed. The pin was therefore decorative: CI
# installed Ubuntu's older package and all four Replay scenarios failed. Check the version instead of
# assuming it.

VTTEST_VERSION="${VTTEST_VERSION:-20251205}"

# The patch date of the vttest on PATH, e.g. 20251205. Empty if there is none, or if it does not
# report one. `vttest -V` prints "VT100 test program, version 2.7 (20251205)".
installed_version() {
    command -v vttest >/dev/null 2>&1 || return 0
    vttest -V 2>/dev/null | sed -n 's/.*(\([0-9]\{8\}\)).*/\1/p'
}

have="$(installed_version)"

if [ "${have}" = "${VTTEST_VERSION}" ]; then
    echo "vttest ${VTTEST_VERSION} is already installed: $(command -v vttest)"
    exit 0
fi

if [ -z "${have}" ] && sudo apt-get install -y vttest; then
    have="$(installed_version)"
    if [ "${have}" = "${VTTEST_VERSION}" ]; then
        echo "vttest ${VTTEST_VERSION} installed from the distro package: $(command -v vttest)"
        exit 0
    fi
fi

echo "need vttest ${VTTEST_VERSION}, found '${have:-none}'; building it from source ..."

sudo apt-get install -y build-essential curl libncurses-dev

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

curl -fsSL "https://invisible-mirror.net/archives/vttest/vttest-${VTTEST_VERSION}.tgz" \
    -o "${WORKDIR}/vttest.tgz"
tar -xzf "${WORKDIR}/vttest.tgz" -C "${WORKDIR}"

cd "${WORKDIR}/vttest-${VTTEST_VERSION}"
./configure --prefix=/usr
make -j"$(nproc)"
sudo make install

# Prove the build is what was asked for, rather than trusting that it must be: a mismatch here means
# the replay files would run out of step, and failing now says so plainly.
hash -r
have="$(installed_version)"
if [ "${have}" != "${VTTEST_VERSION}" ]; then
    echo "vttest reports '${have:-none}' after building ${VTTEST_VERSION} -- refusing to run the gate against it" >&2
    exit 1
fi

echo "vttest ${VTTEST_VERSION} built and installed: $(command -v vttest)"
