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
    local BRANCH="$(git rev-parse --abbrev-ref HEAD)"
    local BASEDIR="/tmp/contour-ppa"
    local SRCDIR="${BASEDIR}/src"
    local BINDIR="${BASEDIR}/bin"
    local DISTDIR="${BASEDIR}/dist"

    # Cleanup and initialize work base work-directory
    rm -rf "${SRCDIR}" "${BINDIR}"
    mkdir -p "${BASEDIR}"

    einfo "Prepare source directory for branch ${BRANCH}"
    git clone -b "${BRANCH}" https://github.com/contour-terminal/contour.git ${SRCDIR}
    cd "${SRCDIR}"

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
    mkdir "${BINDIR}"
    cmake -D3rdparty_DOWNLOAD_DIR="${DISTDIR}" -B "${BINDIR}" -S "${SRCDIR}"
    cp -rvp "${DISTDIR}" "${SRCDIR}/_3rdparty"

    # Also preserve the expected version information, because
    # the Ubuntu PPA server's won't have git information anymore.
    cp -vp "${BINDIR}/version.txt" "${SRCDIR}"

    einfo "Prepare source tarball."
    local debversion="$VERSION~develop-$COMMIT_DATE-$COMMIT_HASH"
    local TARFILE="../contour_${debversion}.orig.tar.gz"
    if [[ ! -e "${TARFILE}" ]]
    then
        mv -v "${SRCDIR}/debian" "${BASEDIR}/debian" # but without debian/ dir
        tar --exclude-vcs --exclude-vcs-ignores -czf "${TARFILE}" .
        mv "${BASEDIR}/debian" "${SRCDIR}/debian"
    fi
    einfo "debversion: ${debversion}"

    for distribution in ${DISTRIBUTIONS[*]}
    do
        if [[ "${distribution}" = "bionic" ]]; then
            # g++ would default to version 7 and that's not good enough
            sed -i -e 's/g++,/g++-8,/' debian/control
        fi

        einfo "${SETMARK}Preparing upload for distribution: ${distribution}"
        einfo "- Updating /debian/changelog."
        local versionsuffix=0ubuntu1~${distribution}
        dch --controlmaint -v 1:${debversion}-${versionsuffix} "git build of ${COMMIT_HASH}"
        #dch -v 1:${VERSION}-${versionsuffix} "${RELEASEBODY}"
        sed -i -e "s/UNRELEASED/${distribution}/g" debian/changelog

        einfo "- Create source package and related files."
        debuild -S -sa -uc -us

        einfo "- Digitally sign the .changes file."
        debsign --re-sign -k ${KEY_ID} ../contour_${debversion}-${versionsuffix}_source.changes

        einfo "- Upload the package to the PPA."
        dput ppa:contour-terminal/contour-dev ../contour_${debversion}-${versionsuffix}_source.changes

        git status
        git diff | cat
        git reset --hard HEAD
    done
}

main
