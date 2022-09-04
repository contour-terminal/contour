#! /bin/bash

set -e

project_root="`dirname $0`/.."
metainfo_xml="$project_root/metainfo.xml"

error_count=0

error() {
    echo 1>&2 "[RELEASE CHECK FAILED] ${*}"
    error_count=$[error_count + 1]
}

version_string=`xmllint --xpath 'string(/component/releases/release[1]/@version)' $metainfo_xml`

release_date=`xmllint --xpath 'string(/component/releases/release[1]/@date)' $metainfo_xml`
[[ "${release_date}" == "" ]] && error "Release date must be present."

release_type=`xmllint --xpath 'string(/component/releases/release[1]/@type)' $metainfo_xml`
[[ "${release_type}" = "development" ]] && error "Release type must not be 'development'"

if [[ $error_count -ne 0 ]]; then
    echo 1>&2 "Please fix them."
    exit 1
fi
