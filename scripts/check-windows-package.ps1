<#
.SYNOPSIS
    Checks what the Windows packages hold, as a table of rows.

.DESCRIPTION
    Read-only: the zip is opened for reading and each MSI through OpenDatabase(path, 0), so nothing is
    installed, elevated or modified. Every row of $Rows names the subject it applies to and a check; a row
    fails by returning the text "expected <x>, found <y>". Exit 0 when every row holds, else 1 with one line
    per failed row: "<target>: <row name>: expected <x>, found <y>". A path given that does not exist is a
    failed row, never a skip.

    To add a check, append a row to $Rows; nothing else changes.

.PARAMETER Zip
    The portable zip.

.PARAMETER MachineMsi
    The per-machine MSI.

.PARAMETER UserMsi
    The per-user MSI, when the build produces one.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Zip,
    [Parameter(Mandatory)][string]$MachineMsi,
    [string]$UserMsi
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# PID_WORDCOUNT of the MSI summary information; its bit 0x8 marks a package that needs no elevation.
$SummaryWordCountId = 15
$NoElevationBit = 0x8

$MachineUpgradeCode = '{0E736497-2B72-4117-95E9-54EC6D000000}'
$UserUpgradeCode = '{AFF353C7-C33A-474C-B9C7-EDD160677BC9}'

# What the zip may hold in the way of executables, and what it must hold. Loaded by name or file type at run
# time, none of these is noticed missing by a build: the Fluent row's controls style (QQuickStyle::setStyle,
# which windeployqt's --qmldir scan cannot see), Qt SVG and its image plugin (see cmake/DeployQt.cmake), and
# the bundled status-line extensions, which contour.exe looks for beside bin/.
$ShippedExecutables = 'contour.exe', 'OpenConsole.exe'
$RequiredZipEntries = @(
    'bin/qml/QtQuick/Controls/FluentWinUI3/qmldir'
    'bin/imageformats/qsvg.dll'
    'bin/Qt6Svg.dll'
) + ('agent-status', 'clock', 'git', 'plasma', 'sysload' | ForEach-Object { "share/contour/extensions/$_.wasm" })

# The components whose registry root and environment scope depend on the installer's scope (wix_machine.wxs
# and wix_user.wxs define both).
$PathComponent = 'ContourPathEnvironment'
$ScopedComponents = $PathComponent, 'ContourConnectShortcut'

$installer = New-Object -ComObject WindowsInstaller.Installer

function Invoke-Com {
    <# Calls a COM member by name; the Windows Installer objects are IDispatch-only. #>
    param([Parameter(Mandatory)]$Target, [Parameter(Mandatory)][string]$Member,
          [string]$Kind = 'InvokeMethod', $Arguments = $null)
    return $Target.GetType().InvokeMember($Member, $Kind, $null, $Target, $Arguments)
}

# Each package is opened once per run, however many rows ask about it.
$Databases = @{}

function Open-MsiDatabase {
    <# Opens the MSI read-only (mode 0), or returns the database already open for it. #>
    param([Parameter(Mandatory)][string]$Msi)
    $path = (Resolve-Path $Msi).Path
    if (-not $Databases.ContainsKey($path)) {
        $Databases[$path] = Invoke-Com $installer 'OpenDatabase' -Arguments @($path, 0)
    }
    return $Databases[$path]
}

function Test-MsiTable {
    <# Whether the package has a table of this name; a table without a row is absent from it. #>
    param([Parameter(Mandatory)]$Database, [Parameter(Mandatory)][string]$Table)
    $query = 'SELECT `Name` FROM `_Tables` WHERE `Name` = ''{0}''' -f $Table
    $view = Invoke-Com $Database 'OpenView' -Arguments @($query)
    Invoke-Com $view 'Execute' | Out-Null
    $found = $null -ne (Invoke-Com $view 'Fetch')
    Invoke-Com $view 'Close' | Out-Null
    return $found
}

function Get-MsiRows {
    <# Runs a SQL query against the MSI, read-only. Returns one string[] per row, one string per column. A table
       the package does not have has no rows; any other failure of the query (a misspelt column, say) is thrown,
       as an absence row would pass on it. #>
    param([Parameter(Mandatory)][string]$Msi, [Parameter(Mandatory)][string]$Query)
    $database = Open-MsiDatabase $Msi
    try {
        $view = Invoke-Com $database 'OpenView' -Arguments @($Query)
    } catch {
        $table = [regex]::Match($Query, 'FROM `(\w+)`').Groups[1].Value
        if ($table -and -not (Test-MsiTable -Database $database -Table $table)) {
            return , @()
        }
        throw
    }
    Invoke-Com $view 'Execute' | Out-Null
    $rows = @()
    while ($null -ne ($record = Invoke-Com $view 'Fetch')) {
        $count = Invoke-Com $record 'FieldCount' 'GetProperty'
        $rows += , @(1..$count | ForEach-Object { [string](Invoke-Com $record 'StringData' 'GetProperty' @([int]$_)) })
    }
    Invoke-Com $view 'Close' | Out-Null
    return , $rows
}

function Get-MsiSummaryWordCount {
    <# The MSI summary information's word count (PID_WORDCOUNT). #>
    param([Parameter(Mandatory)][string]$Msi)
    $summary = Invoke-Com (Open-MsiDatabase $Msi) 'SummaryInformation' 'GetProperty' @(0)
    return [int](Invoke-Com $summary 'Property' 'GetProperty' @($SummaryWordCountId))
}

function Get-MsiProperty {
    <# The value of one Property row, or $null when the package has none. #>
    param([Parameter(Mandatory)][string]$Msi, [Parameter(Mandatory)][string]$Name)
    $rows = Get-MsiRows -Msi $Msi -Query ('SELECT `Value` FROM `Property` WHERE `Property` = ''{0}''' -f $Name)
    if ($rows.Count -eq 0) { return $null }
    return $rows[0][0]
}

function Get-ZipEntries {
    <# Every entry name of the zip, with forward slashes. #>
    param([Parameter(Mandatory)][string]$Path)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $Path).Path)
    try {
        return @($archive.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
    } finally {
        $archive.Dispose()
    }
}

function Test-ZipHolds {
    <# Whether an entry equals $Path or ends in "/$Path". #>
    param([string[]]$Entries, [string]$Path)
    return $null -ne ($Entries | Where-Object { $_ -eq $Path -or $_ -like "*/$Path" } | Select-Object -First 1)
}

function Test-Equal {
    <# $null when equal, else the "expected x, found y" text of a failed row. #>
    param($Expected, $Found)
    if ($Found -eq $Expected) { return $null }
    $shownFound = if ($null -eq $Found) { 'nothing' } else { $Found }
    return "expected $Expected, found $shownFound"
}

function Test-RegistryRoots {
    <# $null when every Registry row of the scope-dependent components has the given Root. #>
    param([string]$Msi, [string]$Root)
    $found = foreach ($component in $ScopedComponents) {
        $rows = Get-MsiRows -Msi $Msi -Query ('SELECT `Root` FROM `Registry` WHERE `Component_` = ''{0}''' -f $component)
        if ($rows.Count -eq 0) { "$component has no Registry row" }
        else { $rows | Where-Object { $_[0] -ne $Root } | ForEach-Object { "$component has Root $($_[0])" } }
    }
    if (-not $found) { return $null }
    "expected Root $Root, found $($found -join '; ')"
}

# The rows. Target: 'zip' | 'msi' (every MSI given) | 'machine' | 'user'. Check receives the subject (the zip's
# entry names, or the MSI's path) and returns $null when the row holds, else "expected <x>, found <y>". The
# first zip row is the vacuity guard: the rows after it would pass on an empty listing.
$Rows = @(
    @{ Target = 'zip'; Name = 'contour.exe present'; Check = {
        param($entries)
        if (Test-ZipHolds $entries 'contour.exe') { return $null }
        'expected a contour.exe, found none (the rows below would pass vacuously)'
    } }
    @{ Target = 'zip'; Name = 'no executable but contour.exe and OpenConsole.exe'; Check = {
        param($entries)
        $strays = @($entries | Where-Object {
            $name = ($_ -split '/')[-1]
            $name -like '*.exe' -and $ShippedExecutables -notcontains $name
        })
        if ($strays.Count -eq 0) { return $null }
        "expected only $($ShippedExecutables -join ', '), found $($strays -join ', ')"
    } }
    @{ Target = 'zip'; Name = 'no build or test leftovers'; Check = {
        param($entries)
        $strays = @($entries | Where-Object {
            $name = ($_ -split '/')[-1]
            $name -like '*.ilk' -or $name -like '*.pdb' -or $name -eq 'Qt6Test.dll' -or $name -eq 'qoffscreen.dll'
        })
        if ($strays.Count -eq 0) { return $null }
        "expected no .ilk, .pdb, Qt6Test.dll or qoffscreen.dll, found $($strays -join ', ')"
    } }
    @{ Target = 'zip'; Name = 'holds what is loaded at run time'; Check = {
        param($entries)
        $missing = @($RequiredZipEntries | Where-Object { -not (Test-ZipHolds $entries $_) })
        if ($missing.Count -eq 0) { return $null }
        "expected $($RequiredZipEntries -join ', '), missing $($missing -join ', ')"
    } }
    @{ Target = 'msi'; Name = 'files-in-use dialog (MsiRMFilesInUse)'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `Dialog` FROM `Dialog` WHERE `Dialog` = ''MsiRMFilesInUse'''
        if ($rows.Count -gt 0) { return $null }
        'expected a Dialog row MsiRMFilesInUse, found none'
    } }
    @{ Target = 'msi'; Name = 'WixUIRMOption'; Check = {
        param($msi)
        Test-Equal 'UseRM' (Get-MsiProperty -Msi $msi -Name 'WixUIRMOption')
    } }
    @{ Target = 'msi'; Name = 'PATH environment entry'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query ('SELECT `Environment` FROM `Environment` WHERE `Component_` = ''{0}''' -f $PathComponent)
        if ($rows.Count -gt 0) { return $null }
        "expected an Environment row of $PathComponent, found none"
    } }
    @{ Target = 'machine'; Name = 'PATH entry is system-wide (Name holds *)'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query ('SELECT `Name` FROM `Environment` WHERE `Component_` = ''{0}''' -f $PathComponent)
        $found = if ($rows.Count -gt 0) { $rows[0][0] } else { 'no row' }
        if ($found.Contains('*')) { return $null }
        "expected a Name holding *, found $found"
    } }
    @{ Target = 'machine'; Name = 'registry roots are HKLM'; Check = {
        param($msi)
        Test-RegistryRoots $msi '2'
    } }
    @{ Target = 'user'; Name = 'registry roots are HKCU'; Check = {
        param($msi)
        Test-RegistryRoots $msi '1'
    } }
    @{ Target = 'machine'; Name = 'UpgradeCode'; Check = {
        param($msi)
        Test-Equal $MachineUpgradeCode (Get-MsiProperty -Msi $msi -Name 'UpgradeCode')
    } }
    @{ Target = 'machine'; Name = 'ALLUSERS'; Check = {
        param($msi)
        Test-Equal '1' (Get-MsiProperty -Msi $msi -Name 'ALLUSERS')
    } }
    @{ Target = 'machine'; Name = 'summary word count bit 0x8 clear (elevation required)'; Check = {
        param($msi)
        $count = Get-MsiSummaryWordCount -Msi $msi
        if (($count -band $NoElevationBit) -eq 0) { return $null }
        "expected word count bit 0x8 clear, found $count"
    } }
    @{ Target = 'user'; Name = 'UpgradeCode'; Check = {
        param($msi)
        Test-Equal $UserUpgradeCode (Get-MsiProperty -Msi $msi -Name 'UpgradeCode')
    } }
    @{ Target = 'user'; Name = 'ALLUSERS absent'; Check = {
        param($msi)
        $value = Get-MsiProperty -Msi $msi -Name 'ALLUSERS'
        if ($null -eq $value) { return $null }
        "expected no ALLUSERS property, found $value"
    } }
    @{ Target = 'user'; Name = 'summary word count bit 0x8 set (no elevation)'; Check = {
        param($msi)
        $count = Get-MsiSummaryWordCount -Msi $msi
        if (($count -band $NoElevationBit) -ne 0) { return $null }
        "expected word count bit 0x8 set, found $count"
    } }
    @{ Target = 'user'; Name = 'no registry row under HKLM or HKU'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `Registry`, `Root` FROM `Registry`'
        $machineWide = @($rows | Where-Object { $_[1] -eq '2' -or $_[1] -eq '3' } | ForEach-Object { $_[0] })
        if ($machineWide.Count -eq 0) { return $null }
        "expected no Registry row with Root 2 or 3, found $($machineWide -join ', ')"
    } }
    @{ Target = 'user'; Name = 'no system-wide environment row'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `Environment`, `Name` FROM `Environment`'
        $systemWide = @($rows | Where-Object { $_[1].Contains('*') } | ForEach-Object { $_[0] })
        if ($systemWide.Count -eq 0) { return $null }
        "expected no Environment row whose Name holds *, found $($systemWide -join ', ')"
    } }
)

# Which path each target's subject comes from, and how the subject is read.
$Subjects = @(
    @{ Target = 'zip'; Path = $Zip; Load = { param($path) , (Get-ZipEntries $path) } }
    @{ Target = 'machine'; Path = $MachineMsi; Load = { param($path) (Resolve-Path $path).Path } }
)
if ($UserMsi) {
    $Subjects += @{ Target = 'user'; Path = $UserMsi; Load = { param($path) (Resolve-Path $path).Path } }
}

$failures = [System.Collections.Generic.List[string]]::new()
foreach ($subject in $Subjects) {
    if (-not (Test-Path -LiteralPath $subject.Path -PathType Leaf)) {
        $failures.Add("$($subject.Target): exists: expected a file at $($subject.Path), found none")
        continue
    }
    $loaded = & $subject.Load $subject.Path
    $isMsi = $subject.Target -ne 'zip'
    foreach ($row in $Rows | Where-Object { $_.Target -eq $subject.Target -or ($isMsi -and $_.Target -eq 'msi') }) {
        $outcome = & $row.Check $loaded
        if ($outcome) {
            $failures.Add("$($subject.Target): $($row.Name): $outcome")
        }
    }
}

foreach ($line in $failures) {
    Write-Host $line
}
exit ($failures.Count -eq 0 ? 0 : 1)
