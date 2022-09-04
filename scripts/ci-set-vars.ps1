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
Write-Output "::set-output name=version::${VERSION}"
Write-Output "::set-output name=VERSION_STRING::${VERSION_STRING}"
Write-Output "::set-output name=RUN_ID::$Env:GITHUB_RUN_NUMBER"
Write-Output "::set-output name=IS_PRERELEASE::$IS_PRE"
Write-Output "::set-output name=RELEASENAME_SUFFIX::$SUFFIX"

# debug prints
Write-Output "set-output name=version::${VERSION}"
Write-Output "set-output name=VERSION_STRING::${VERSION_STRING}"
Write-Output "set-output name=RUN_ID::$Env:GITHUB_RUN_NUMBER"
Write-Output "set-output name=IS_PRERELEASE::$IS_PRE"
Write-Output "set-output name=RELEASENAME_SUFFIX::$SUFFIX"
