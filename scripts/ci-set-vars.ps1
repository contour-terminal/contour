#!/usr/bin/env powershell

$VERSION = Select-String -Path .\Changelog.md -Pattern '^### ([^ ]+)' | % { "$($_.matches.groups[1])" } | Select-Object -First 1
$SUFFIX  = Select-String -Path .\Changelog.md -Pattern '^### .* \(([^\)]+)\)' | % { "$($_.matches.groups[1])" } | Select-Object -First 1

if ($Env:REPOSITORY -eq "master") {
    $IS_PRE = 'true';
    $SUFFIX = '';
    $DELIM = '';
} else {
    $IS_PRE = 'false';
    $SUFFIX = "prerelease-${Env:GITHUB_RUN_NUMBER}";
    $DELIM = "-";
}

$VERSION_STRING = "${VERSION}${DELIM}${SUFFIX}"

Set-Content -Path "version.txt" -Value "${VERSION_STRING}"
Write-Output "::set-output name=version::${VERSION}"
Write-Output "::set-output name=VERSION_STRING::${VERSION_STRING}"
Write-Output "::set-output name=RUN_ID::$Env:GITHUB_RUN_NUMBER"
Write-Output "::set-output name=IS_PRERELEASE::$IS_PRE"
Write-Output "::set-output name=RELEASENAME_SUFFIX::$SUFFIX"

# debug prints
Write-Output "set-output name=version::$VERSION"
Write-Output "set-output name=RUN_ID::$Env:GITHUB_RUN_NUMBER"
Write-Output "set-output name=IS_PRERELEASE::$IS_PRE"
Write-Output "set-output name=RELEASENAME_SUFFIX::$SUFFIX"
