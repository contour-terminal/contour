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

if [[ "${GITHUB_HEAD_REF}" == "release" ]]; then
    IS_PRE='false';
    SUFFIX="";
    VERSION_STRING="${VERSION}"
else
    IS_PRE='true';
    SUFFIX="prerelease";
    VERSION_STRING="${VERSION}-${SUFFIX}"
fi

# TODO: pass "/path/to/version.txt" target filename via CLI param "${1}", and only write that if given.
echo "${VERSION_STRING}" >version.txt

# The first <p> is the release summary; every later <p> is a section heading for the <ul> that
# follows it. The union XPath yields all of them in document order.
# NB: the summary is stripped of its XML indentation, which markdown would render as a code block.
release_summary=$(xmllint --xpath 'string(/component/releases/release[1]/description/p[1])' $metainfo_xml |
                    sed -e 's/^[[:space:]]*//' -e '/^$/d')
RELEASEBODY=$(xmllint --xpath '/component/releases/release[1]/description/p[position()>1]|/component/releases/release[1]/description/ul/li' $metainfo_xml |
                    sed 's,<p>,\n### ,g' |
                    sed 's,</p>,\n,g' |
                    sed 's/<li>/ - /g' |
                    sed 's,</li>,,g')
if [[ "${release_summary}" != "" ]]; then
    RELEASEBODY="${release_summary}"$'\n'"${RELEASEBODY}"
fi
RELEASEBODY="${RELEASEBODY//\"/\\\"}"
RELEASEBODY="${RELEASEBODY//$'\r'/''}"

echo "version=${VERSION}" >> "$GITHUB_OUTPUT"
echo "VERSION_STRING=${VERSION_STRING}" >> "$GITHUB_OUTPUT"
echo "RUN_ID=${GITHUB_RUN_NUMBER}" >> "$GITHUB_OUTPUT"
echo "IS_PRERELEASE=${IS_PRE}" >> "$GITHUB_OUTPUT"
echo "RELEASENAME_SUFFIX=${SUFFIX}" >> "$GITHUB_OUTPUT"

echo "${RELEASEBODY}" >release-body.md
echo "${RELEASEBODY}"
