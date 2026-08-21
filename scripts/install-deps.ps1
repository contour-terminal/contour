#! /usr/bin/env pwsh

# Let's assume for now, that this script is only invoked from within Windows
# But in the future, I'd like it to support all the others, too.

class ThirdParty {
    [ValidateNotNullOrEmpty()] [string] $Folder
    [ValidateNotNullOrEmpty()] [string] $Archive
    [ValidateNotNullOrEmpty()] [string] $URI
    [string] $Macro
}

# Single source of truth: the version lives in cmake/ContourThirdParties.cmake
# (LIBUNICODE_MINIMAL_VERSION), exactly as install-deps.sh reads it. Hard-coding it here too would let
# the vendored source drift below the CMake requirement, which then fails to configure -- and it did:
# raising the floor updated every platform except Windows, which kept unpacking the older archive.
$ThirdPartiesCMakeFile = Join-Path (Join-Path $PSScriptRoot "..") "cmake/ContourThirdParties.cmake"
$libunicode_match = Select-String -Path $ThirdPartiesCMakeFile `
    -Pattern '^set\(LIBUNICODE_MINIMAL_VERSION "([0-9.]+)"'
# Say what went wrong here rather than 404 on ".../tags/v.zip" further down: no match means that line
# changed shape, and the download error names neither the file nor the reason. Without this, the
# missing match surfaces as a null-reference on .Matches[0] at best, and as an empty version at worst.
if (-not $libunicode_match) {
    Write-Error "Cannot read LIBUNICODE_MINIMAL_VERSION from ${ThirdPartiesCMakeFile}.`nExpected a line of the form: set(LIBUNICODE_MINIMAL_VERSION `"<version>`")"
    exit 1
}
$libunicode_version = $libunicode_match.Matches[0].Groups[1].Value
$reflection_cpp_version="0.4.0"

# Take care, order matters, at least as much as dependencies are of concern.
$ThirdParties =
@(
    [ThirdParty]@{
        Folder  = "reflection-cpp-${reflection_cpp_version}";
        Archive = "reflection-cpp-${reflection_cpp_version}.zip";
        URI     = "https://github.com/contour-terminal/reflection-cpp/archive/refs/tags/v${reflection_cpp_version}.zip";
        Macro   = "reflection_cpp"
    };
    [ThirdParty]@{
        Folder  = "libunicode-${libunicode_version}";
        Archive = "libunicode-${libunicode_version}.zip";
        URI     = "https://github.com/contour-terminal/libunicode/archive/refs/tags/v${libunicode_version}.zip";
        Macro   = "libunicode"
    };
    [ThirdParty]@{
        Folder  = "termbench-pro-3a39a4ad592047dee3038d8bfcce84215ac55032";
        Archive = "termbench-pro-3a39a4ad592047dee3038d8bfcce84215ac55032.zip";
        URI     = "https://github.com/contour-terminal/termbench-pro/archive/3a39a4ad592047dee3038d8bfcce84215ac55032.zip";
        Macro   = "termbench_pro"
    }
    [ThirdParty]@{
        Folder  = "boxed-cpp-1.4.3";
        Archive = "boxed-cpp-1.4.3.zip";
        URI     = "https://github.com/contour-terminal/boxed-cpp/archive/refs/tags/v1.4.3.zip";
        Macro   = "boxed_cpp"
    }
)

function Fetch-And-Add {
    param (
        [Parameter(Mandatory)] [string] $Target,
        [Parameter(Mandatory)] [string] $Folder,
        [Parameter(Mandatory)] [string] $Archive,
        [Parameter(Mandatory)] [string] $URI,
        [string] $Macro,
        [Parameter(Mandatory)] [string] $CMakeListsFile
    )

    $DistfilesDir = "${Target}/distfiles"
    if (! [System.IO.Directory]::Exists($DistfilesDir)) {
        New-Item -ItemType Directory -Force -Path $DistfilesDir
    }

    $ArchivePath = "${DistfilesDir}/${Archive}"
    if (! [System.IO.File]::Exists($ArchivePath)) {
        Write-Host "Downloading $Archive to $ArchivePath"
        Invoke-WebRequest -Uri $URI -OutFile $ArchivePath
    }
    else {
        Write-Host "Already there: $ArchivePath"
    }

    if (! [System.IO.Directory]::Exists("$Target/sources/$Folder")) {
        Write-Host "Populating ${Folder}"
        Expand-Archive $ArchivePath -DestinationPath "${Target}/sources/"
    }
    else {
        Write-Host "Already there ${Folder}"
    }

    if ($Macro -ne "") {
        Add-Content $CMakeListsFile "macro(ContourThirdParties_Embed_${Macro})"
        Add-Content $CMakeListsFile "    add_subdirectory(`${ContourThirdParties_SRCDIR}/${Folder} EXCLUDE_FROM_ALL)"
        Add-Content $CMakeListsFile "endmacro()"
    }
    else {
        Add-Content $CMakeListsFile "add_subdirectory(${Folder} EXCLUDE_FROM_ALL)"
    }
}

$option = $args[0]
Write-Host "a) arg0: $option"

function Run {
    $ProjectRoot = "${PSScriptRoot}/.."
    $ThirdPartiesDir = "${ProjectRoot}/_deps"
    $DistfilesDir = "${ThirdPartiesDir}/distfiles"
    $SourcesDir = "${ThirdPartiesDir}/sources"
    $CMakeListsFile = "${SourcesDir}/CMakeLists.txt"

    if (! [System.IO.Directory]::Exists($DistfilesDir)) {
        New-Item -ItemType Directory -Force -Path $DistfilesDir
    }

    if (! [System.IO.Directory]::Exists($SourcesDir)) {
        New-Item -ItemType Directory -Force -Path $SourcesDir
    }

    if ([System.IO.File]::Exists($CMakeListsFile)) {
        Clear-Content $CMakeListsFile
    }

    foreach ($TP in $ThirdParties) {
        Fetch-And-Add `
            -Folder $TP.Folder `
            -Archive $TP.Archive `
            -URI $TP.URI `
            -Macro $TP.Macro `
            -Target $ThirdPartiesDir `
            -CMakeListsFile $CMakeListsFile
    }

    if ($option -ne "--skip-vcpkg") {
        vcpkg install --triplet x64-windows
        # qt5-base
    }
}

Run
