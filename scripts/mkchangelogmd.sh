#! /bin/bash

project_root="`dirname $0`/.."
metainfo_xml="$project_root/metainfo.xml"

version_string=`xmllint --xpath 'string(/component/releases/release[1]/@version)' $metainfo_xml`
release_type=`xmllint --xpath 'string(/component/releases/release[1]/@type)' $metainfo_xml`

if test x$release_type = xstable; then
    release_type=`date +%Y-%m-%d`
else
    release_type="unreleased"
fi

echo "### $version_string ($release_type)"
echo ""

# Changelog items
xmllint --xpath '/component/releases/release[1]/description/ul/li' $metainfo_xml |
    sed 's/<li>/ - /g' |
    sed 's,</li>,\n,g'
