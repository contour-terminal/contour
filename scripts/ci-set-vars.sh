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
                    sed 's,</li>,\n,g')
RELEASEBODY="${RELEASEBODY//'%'/'%25'}"
RELEASEBODY="${RELEASEBODY//$'\n'/'%0A'}"
RELEASEBODY="${RELEASEBODY//$'\r'/'%0D'}"

echo "::set-output name=version::${VERSION}"
echo "::set-output name=VERSION_STRING::${VERSION_STRING}"
echo "::set-output name=RUN_ID::${GITHUB_RUN_NUMBER}"
echo "::set-output name=IS_PRERELEASE::${IS_PRE}"
echo "::set-output name=RELEASENAME_SUFFIX::${SUFFIX}"
echo "::set-output name=RELEASEBODY::${RELEASEBODY}"

