#! /bin/bash

set -e

if ! which xmllint; then
    echo "No xmllint installed"
    exit 1
fi

project_root="`dirname $0`/.."
metainfo_xml="$project_root/metainfo.xml"

VERSION_TRIPLE=`xmllint --xpath 'string(/component/releases/release[1]/@version)' $metainfo_xml`
#RELEASE_TYPE=`xmllint --xpath 'string(/component/releases/release[1]/@type)' $metainfo_xml`

if [[ "${GITHUB_RUN_NUMBER}" != "" ]]; then
    VERSION="${VERSION_TRIPLE}.${GITHUB_RUN_NUMBER}"
else
    VERSION="${VERSION_TRIPLE}"
fi

if [[ -z "${GITHUB_OUTPUT}" ]]; then
    GITHUB_OUTPUT="/dev/stdout"
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

RELEASEBODY=$(xmllint --xpath '/component/releases/release[1]/description/ul/li' $metainfo_xml |
                    sed 's/<li>/ - /g' |
                    sed 's,</li>,,g')
RELEASEBODY="${RELEASEBODY//\"/\\\"}"
RELEASEBODY="${RELEASEBODY//$'\r'/''}"

echo "version=${VERSION}" >> "$GITHUB_OUTPUT"
echo "VERSION_STRING=${VERSION_STRING}" >> "$GITHUB_OUTPUT"
echo "RUN_ID=${GITHUB_RUN_NUMBER}" >> "$GITHUB_OUTPUT"
echo "IS_PRERELEASE=${IS_PRE}" >> "$GITHUB_OUTPUT"
echo "RELEASENAME_SUFFIX=${SUFFIX}" >> "$GITHUB_OUTPUT"

echo "${RELEASEBODY}" >release-body.md
echo "${RELEASEBODY}"

