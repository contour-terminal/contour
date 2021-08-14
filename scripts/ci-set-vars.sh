#! /bin/bash

set -ex

VERSION=$(grep '^### ' Changelog.md | head -n1 | awk '{print $2}')
SUFFIX=$(grep '^### ' Changelog.md | head -n1 | awk '{print $3}' | tr -d '()' | sed 's/ /_/g')

if [[ "${GITHUB_RUN_NUMBER}" != "" ]]; then
    VERSION="${VERSION}.${GITHUB_RUN_NUMBER}"
fi

case "${GITHUB_REF}" in
    refs/heads/master|refs/heads/release)
        IS_PRE='false';
        SUFFIX="";
        VERSION_STRING="${VERSION}"
        ;;
    *)
        IS_PRE='true';
        SUFFIX="prerelease";
        VERSION_STRING="${VERSION}-${SUFFIX}"
        ;;
esac

# TODO: pass "/path/to/version.txt" target filename via CLI param "${1}", and only write that if given.
echo "${VERSION_STRING}" >version.txt

RELEASEBODY=$(awk -v RS='^### ' '/^'$VERSION'/ {print $0}' Changelog.md | tail -n+3)
RELEASEBODY="${RELEASEBODY//'%'/'%25'}"
RELEASEBODY="${RELEASEBODY//$'\n'/'%0A'}"
RELEASEBODY="${RELEASEBODY//$'\r'/'%0D'}"

echo "::set-output name=version::${VERSION}"
echo "::set-output name=VERSION_STRING::${VERSION_STRING}"
echo "::set-output name=RUN_ID::${GITHUB_RUN_NUMBER}"
echo "::set-output name=IS_PRERELEASE::${IS_PRE}"
echo "::set-output name=RELEASENAME_SUFFIX::${SUFFIX}"
echo "::set-output name=RELEASEBODY::${RELEASEBODY}"

