#! /usr/bin/env pwsh

# Let's assume for now, that this script is only invoked from within Windows
# But in the future, I'd like it to support all the others, too.

class ThirdParty {
    [ValidateNotNullOrEmpty()] [string] $Folder
    [ValidateNotNullOrEmpty()] [string] $Archive
    [ValidateNotNullOrEmpty()] [string] $URI
    [string] $Macro
}

# Take care, order matters, at least as much as dependencies are of concern.
$ThirdParties =
@(
    [ThirdParty]@{
        Folder  = "libunicode-23d7b30166a914b10526bb8fe7a469a9610c07dc";
        Archive = "libunicode-23d7b30166a914b10526bb8fe7a469a9610c07dc.zip";
        URI     = "https://github.com/contour-terminal/libunicode/archive/23d7b30166a914b10526bb8fe7a469a9610c07dc.zip";
        Macro   = "libunicode"
    };
    [ThirdParty]@{
        Folder  = "termbench-pro-7f86c882b2dab88a0cceeffd7e3848f55fa5f6f2";
        Archive = "termbench-pro-7f86c882b2dab88a0cceeffd7e3848f55fa5f6f2.zip";
        URI     = "https://github.com/contour-terminal/termbench-pro/archive/7f86c882b2dab88a0cceeffd7e3848f55fa5f6f2.zip";
        Macro   = "termbench_pro"
    }
    [ThirdParty]@{
        Folder  = "boxed-cpp-1.2.2";
        Archive = "boxed-cpp-1.2.2.zip";
        URI     = "https://github.com/contour-terminal/boxed-cpp/archive/refs/tags/v1.2.2.zip";
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
    $ThirsPartiesDir = "${ProjectRoot}/_deps"
    $DistfilesDir = "${ThirsPartiesDir}/distfiles"
    $SourcesDir = "${ThirsPartiesDir}/sources"
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
            -Target $ThirsPartiesDir `
            -CMakeListsFile $CMakeListsFile
    }

    if ($option -ne "--skip-vcpkg") {
        vcpkg install --triplet x64-windows
        # qt5-base
    }
}

Run
