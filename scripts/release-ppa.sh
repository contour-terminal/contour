#!/usr/bin/env bash

set -ex

KEY_ID="5E39E916156361EA1021D2B5427FBA118140755D"
EMAIL="christian@parpart.family"
SETMARK="\033[>M"

# bionic    : Ubuntu 18.04
# focal     : Ubuntu 20.04
# groovy    : Ubuntu 20.10
DISTRIBUTIONS=(groovy focal bionic)

function einfo()
{
    echo -ne " \033[1;32;4m|> ${*}\033[m\n"
}

function main()
{
    # Setup properties
    local ORIGIN_DIR="`pwd`"
    local BRANCH="$(git rev-parse --abbrev-ref HEAD)"
    local BASE_DIR="/tmp/contour-ppa"
    local SOURCE_DIR="${BASE_DIR}/src"
    local BIN_DIR="${BASE_DIR}/bin"
    local DIST_DIR="${BASE_DIR}/dist"

    # Cleanup and initialize work base work-directory
    rm -rf "${SOURCE_DIR}" "${BIN_DIR}"
    mkdir -p "${BASE_DIR}"

    einfo "Prepare source directory for branch ${BRANCH}"
    git clone -b "${BRANCH}" https://github.com/christianparpart/contour.git ${SOURCE_DIR}
    cd "${SOURCE_DIR}"

    # {{{ set source dir related properties
    local COMMIT_HASH=$(git rev-parse --short=8 HEAD)
    local COMMIT_DATE=$(date +%Y-%m-%d-%H-%M -d @$(git show --format=%at HEAD | head -n 1))
    local VERSION=$(grep '^### ' Changelog.md | head -n1 | awk '{print $2}')
    local SUFFIX=$(grep '^### ' Changelog.md | head -n1 | awk '{print $3}' | tr -d '()' | sed 's/ /_/g')
    if [[ "${BRANCH}" = "master" ]]
    then
       SUFFIX="";
       VERSION_STRING="${VERSION}"
    else
       SUFFIX=""                    # ="prerelease-${GITHUB_RUN_NUMBER:-0}";
       VERSION_STRING="${VERSION}"  # ="${VERSION}-${SUFFIX}"
    fi
    # }}}

    einfo "Fetching embedded 3rdparty dependencies." # by running cmake configure step.
    mkdir "${BIN_DIR}"
    cmake -D3rdparty_DOWNLOAD_DIR="${DIST_DIR}" -B "${BIN_DIR}" -S "${SOURCE_DIR}"
    cp -rvp "${DIST_DIR}" "${SOURCE_DIR}/_3rdparty"

    einfo "Prepare source tarball."
    local debversion="$VERSION~develop-$COMMIT_DATE-$COMMIT_HASH"
    local TARFILE="../contour_${debversion}.orig.tar.gz"
    if [[ ! -e "${TARFILE}" ]]
    then
        # create tar archive, but without the project's /debian directory.
        mv -v "${SOURCE_DIR}/debian" "${BASE_DIR}/debian"
        tar --exclude-vcs --exclude-vcs-ignores -czf "${TARFILE}" .
        mv "${BASE_DIR}/debian" "${SOURCE_DIR}/debian"
    fi
    einfo "debversion: ${debversion}"

    for distribution in ${DISTRIBUTIONS[*]}
    do
        local versionsuffix=0ubuntu1~${distribution}

        if [[ "${distribution}" = "bionic" ]]; then
            # g++ would default to version 7 and that's not good enough
            sed -i -e 's/g++,/g++-8,/' debian/control
        fi

        einfo "${SETMARK}Preparing upload for distribution: ${distribution}"
        dch --controlmaint -v 1:${debversion}-${versionsuffix} "git build of ${COMMIT_HASH}"
        #dch -v 1:${VERSION}-${versionsuffix} "${RELEASEBODY}"
        sed -i -e "s/UNRELEASED/${distribution}/g" debian/changelog

        einfo "|> ${distribution}: Create contour source package."
        debuild -S -sa -uc -us
        debsign --re-sign -k ${KEY_ID} ../contour_${debversion}-${versionsuffix}_source.changes
        dput ppa:christianparpart/contour-dev ../contour_${debversion}-${versionsuffix}_source.changes

        einfo "|> ${distribution}: Create contour-cli source package."
        if [[ ! -e "../contour-cli_${debversion}.orig.tar.gz" ]]; then
            cp -vp "${TARFILE}" "../contour-cli_${debversion}.orig.tar.gz"
        fi
        sed -i -e 's/contour/contour-cli/g' debian/changelog
        sed -i -e 's/DEB_BUILD_OPTIONS =/DEB_BUILD_OPTIONS = nogui/' debian/rules
        # we use ORIGIN_DIR so it takes the local one during development (works on CI).
        cp "${ORIGIN_DIR}/debian/contour-cli.control" debian/control
        debuild -S -sa -uc -us
        debsign --re-sign -k ${KEY_ID} ../contour-cli_${debversion}-${versionsuffix}_source.changes
        dput ppa:christianparpart/contour-dev ../contour-cli_${debversion}-${versionsuffix}_source.changes

        git reset --hard HEAD
    done
}

main
