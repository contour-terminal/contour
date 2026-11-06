#!/usr/bin/env powershell

$VERSION = (Select-Xml -Path .\metainfo.xml -XPath '/component/releases/release[1]/@version').Node.Value

if ("${Env:GITHUB_RUN_NUMBER}" -ne "") {
    # Guard, just in case it's run from outside the CI (e.g. for testing).
    $VERSION = "${VERSION}.${Env:GITHUB_RUN_NUMBER}"
}

# CONTOUR_BUILD_CHANNEL is set at workflow level by .github/workflows/build.yml; anything but
# "release" (or unset, as in a local run) is a prerelease.
if ($Env:CONTOUR_BUILD_CHANNEL -eq 'release') {
    $VERSION_STRING = "${VERSION}"
} else {
    $VERSION_STRING = "${VERSION}-prerelease"
}

Set-Content -Path "version.txt" -Value "${VERSION_STRING}"
echo "version=${VERSION}" >> "${Env:GITHUB_OUTPUT}"
echo "VERSION_STRING=${VERSION_STRING}" >> "${Env:GITHUB_OUTPUT}"

# debug prints
Get-Content -Path "${Env:GITHUB_OUTPUT}"
