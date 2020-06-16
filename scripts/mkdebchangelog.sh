#! /bin/bash

set -ex

ROOTDIR="$(readlink -f $(dirname $0)/..)"
VERSION="${1:-0.0.0}"
INCR="${2:-$(date +%s)}"

sed -i -e "s/0.0.0-1/${VERSION}-${INCR}/g" "${ROOTDIR}/debian/changelog"

# cat "${ROOTDIR}/Changelog.md" \
#     | sed 's/^### \([^ ]\+\).*$/contour (\1-1) focal; urgency=low/' \
#     | while read LINE; do
#     if [[ "${LINE}" != "" ]] && [[ "${LINE}" != "^contour" ]]; then
#     fi
# done
