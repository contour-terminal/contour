#!/usr/bin/env powershell

$VERSION = (Select-Xml -Path .\metainfo.xml -XPath '/component/releases/release[1]/@version').Node.Value

if ("${Env:GITHUB_RUN_NUMBER}" -ne "") {
    # Guard, just in case it's run from outside the CI (e.g. for testing).
    $VERSION = "${VERSION}.${Env:GITHUB_RUN_NUMBER}"
}

switch -Regex ($Env:GITHUB_REF) {
    "^refs/heads/(master|release)$" {
        $IS_PRE = 'false';
        $SUFFIX = '';
        $DELIM = '';
    }
    Default {
        $IS_PRE = 'true';
        $SUFFIX = "prerelease";
        $DELIM = "-";
    }
}

$VERSION_STRING = "${VERSION}${DELIM}${SUFFIX}"

Set-Content -Path "version.txt" -Value "${VERSION_STRING}"
echo "version=${VERSION}" >> "${Env:GITHUB_OUTPUT}"
echo "VERSION_STRING=${VERSION_STRING}" >> "${Env:GITHUB_OUTPUT}"
echo "RUN_ID=$Env:GITHUB_RUN_NUMBER" >> "${Env:GITHUB_OUTPUT}"
echo "IS_PRERELEASE=$IS_PRE" >> "${Env:GITHUB_OUTPUT}"
echo "RELEASENAME_SUFFIX=$SUFFIX" >> "${Env:GITHUB_OUTPUT}"

# debug prints
Get-Content -Path "${Env:GITHUB_OUTPUT}"
