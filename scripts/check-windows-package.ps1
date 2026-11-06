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
$ScopedComponents = $PathComponent, 'ContourInstallRoot', 'ContourStartMenuShortcut', 'ContourConnectShortcut', 'ContourDesktopShortcut'

# The SetProperty actions that settle INSTALL_ROOT, in the order they must run: the given folder is noted
# before AppSearch, the rest follow it (the last that applies wins).
$RootChain = 'ContourRootGiven', 'ContourRootDefault', 'ContourRootRecorded', 'ContourRootPrevious', 'ContourRootGivenRestore'

# The one condition under which the previous folder is kept: not an old release's versioned default.
$PreviousRootCondition = 'CONTOUR_PREVIOUS_ROOT AND NOT (CONTOUR_PREVIOUS_ROOT >< "\Contour Terminal Emulator ")'

# Where an upgrade looks for the previous install folder, per scope: the folder the product recorded for itself
# (HKLM or HKCU per scope) first, then the Add/Remove Programs entry, which is in HKLM for a per-user product too.
# Root is the registry hive's number (1 HKCU, 2 HKLM); all are 64-bit raw value searches.
$RegLocatorRaw64 = 18 # msidbLocatorTypeRawValue (2) + msidbLocatorType64bit (16)
$ArpLocator = @{ Signature = 'ContourPreviousRoot'; Root = '2'; Key = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\[WIX_UPGRADE_DETECTED]'; Name = 'InstallLocation' }
$RegLocators = @{
    machine = @(
        @{ Signature = 'ContourRecordedRoot'; Root = '2'; Key = 'Software\Contour\Contour'; Name = 'InstallRoot' }
        $ArpLocator)
    user    = @(
        @{ Signature = 'ContourRecordedRoot'; Root = '1'; Key = 'Software\Contour\Contour'; Name = 'InstallRoot' }
        $ArpLocator)
}

# The command-line switches of an unattended install (ADD_TO_PATH=0, DESKTOP_SHORTCUT=0), and the feature each
# drops when set to 0.
$FeatureGates = @(
    @{ Switch = 'ADD_TO_PATH'; Feature = 'CM_C_contour_path' }
    @{ Switch = 'DESKTOP_SHORTCUT'; Feature = 'ContourDesktopShortcutFeature' }
)
$Switches = $FeatureGates | ForEach-Object { $_.Switch }

# The links Add/Remove Programs shows next to the entry.
$ArpLinks = @{
    ARPURLINFOABOUT  = 'https://contour-terminal.org/'
    ARPHELPLINK      = 'https://github.com/contour-terminal/contour/issues'
    ARPURLUPDATEINFO = 'https://github.com/contour-terminal/contour/releases'
}

# The toolset's redistributable DLLs, installed app-local beside contour.exe (InstallRequiredSystemLibraries).
$VcRuntimeDlls = 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll'

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

function Get-MsiDirectoryPath {
    <# A directory of the Directory table as its path, from the standard folder it hangs from: "A/B/C". #>
    param([Parameter(Mandatory)][string]$Msi, [Parameter(Mandatory)][string]$Directory)
    $names = @()
    $current = $Directory
    while ($current) {
        $query = 'SELECT `Directory_Parent`, `DefaultDir` FROM `Directory` WHERE `Directory` = ''{0}''' -f $current
        $rows = Get-MsiRows -Msi $Msi -Query $query
        if ($rows.Count -eq 0) { return "no $current directory" }
        $parent, $defaultDir = $rows[0]
        if (-not $parent -or $parent -eq 'TARGETDIR') {
            $names = @($current) + $names   # a standard folder: named by its Id
            break
        }
        # A directory named '.' is the one it hangs from (ProgramFiles6432Folder is ProgramFiles64Folder here).
        $leaf = ($defaultDir -split '\|')[-1]
        if ($leaf -ne '.') { $names = @($leaf) + $names }
        $current = $parent
    }
    return $names -join '/'
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

function Get-CustomActionTarget {
    <# The Target of a CustomAction row (for a SetProperty, the value it sets), or a note that there is none. #>
    param([string]$Msi, [string]$Action)
    $rows = Get-MsiRows -Msi $Msi -Query ('SELECT `Target` FROM `CustomAction` WHERE `Action` = ''{0}''' -f $Action)
    if ($rows.Count -eq 0) { return "no $Action action" }
    return $rows[0][0]
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
    @{ Target = 'zip'; Name = 'carries the VC++ runtime beside contour.exe'; Check = {
        param($entries)
        $missing = @($VcRuntimeDlls | Where-Object { -not (Test-ZipHolds $entries "bin/$_") })
        if ($missing.Count -eq 0) { return $null }
        "expected $($VcRuntimeDlls -join ', ') in bin/, missing $($missing -join ', ')"
    } }
    @{ Target = 'msi'; Name = 'carries the VC++ runtime beside contour.exe'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `FileName` FROM `File`'
        $names = @($rows | ForEach-Object { ($_[0] -split '\|')[-1].ToLowerInvariant() })
        $missing = @($VcRuntimeDlls | Where-Object { $names -notcontains $_ })
        if ($missing.Count -eq 0) { return $null }
        "expected $($VcRuntimeDlls -join ', ') in the File table, missing $($missing -join ', ')"
    } }
    @{ Target = 'zip'; Name = 'carries no Universal CRT'; Check = {
        param($entries)
        # Windows 10 and later ship it; a copy beside the executable would pin an old one.
        $found = @($entries | Where-Object { $name = ($_ -split '/')[-1]; $name -eq 'ucrtbase.dll' -or $name -like 'api-ms-win-*.dll' })
        if ($found.Count -eq 0) { return $null }
        "expected no ucrtbase.dll or api-ms-win-*.dll, found $($found -join ', ')"
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
    @{ Target = 'machine'; Name = 'install folder'; Check = {
        param($msi)
        Test-Equal 'ProgramFiles64Folder/Contour' (Get-MsiDirectoryPath $msi 'INSTALL_ROOT')
    } }
    @{ Target = 'user'; Name = 'install folder'; Check = {
        # Never the Local AppData folder itself, which holds Contour's own state.
        param($msi)
        Test-Equal 'LocalAppDataFolder/Programs/Contour' (Get-MsiDirectoryPath $msi 'INSTALL_ROOT')
    } }
    @{ Target = 'msi'; Name = 'where the previous folder is searched'; Check = {
        param($msi)
        $problems = foreach ($locator in $RegLocators[$script:CurrentTarget]) {
            $query = 'SELECT `Root`, `Key`, `Name`, `Type` FROM `RegLocator` WHERE `Signature_` = ''{0}''' -f $locator.Signature
            $rows = Get-MsiRows -Msi $msi -Query $query
            $expected = '{0}|{1}|{2}|{3}' -f $locator.Root, $locator.Key, $locator.Name, $RegLocatorRaw64
            $found = if ($rows.Count -gt 0) { $rows[0] -join '|' } else { 'no such locator' }
            if ($found -ne $expected) { "$($locator.Signature) is $found, not $expected" }
        }
        if (-not $problems) { return $null }
        "expected the recorded and the ARP lookups pinned, found $($problems -join '; ')"
    } }
    @{ Target = 'msi'; Name = 'INSTALL_ROOT chain runs in order in both sequences'; Check = {
        param($msi)
        $problems = foreach ($table in 'InstallUISequence', 'InstallExecuteSequence') {
            $sequence = @{}
            $tableRows = Get-MsiRows -Msi $msi -Query ('SELECT `Action`, `Sequence` FROM `{0}`' -f $table)
            foreach ($row in $tableRows) {
                $sequence[$row[0]] = [int]$row[1]
            }
            $missing = @($RootChain + 'AppSearch' | Where-Object { -not $sequence.ContainsKey($_) })
            if ($missing) { "$table lacks $($missing -join ', ')"; continue }
            if ($sequence['ContourRootGiven'] -ge $sequence['AppSearch']) { "${table}: ContourRootGiven is not before AppSearch" }
            if ($sequence['ContourRootDefault'] -le $sequence['AppSearch']) { "${table}: ContourRootDefault is not after AppSearch" }
            foreach ($pair in 1..($RootChain.Count - 2)) {
                $earlier, $later = $RootChain[$pair], $RootChain[$pair + 1]
                if ($sequence[$earlier] -ge $sequence[$later]) { "${table}: $earlier is not before $later" }
            }
        }
        if (-not $problems) { return $null }
        "expected $($RootChain -join ' < ') around AppSearch, found $($problems -join '; ')"
    } }
    @{ Target = 'machine'; Name = 'default install folder'; Check = {
        param($msi)
        Test-Equal '[ProgramFiles64Folder]Contour\' (Get-CustomActionTarget $msi 'ContourRootDefault')
    } }
    @{ Target = 'user'; Name = 'default install folder'; Check = {
        param($msi)
        Test-Equal '[LocalAppDataFolder]Programs\Contour\' (Get-CustomActionTarget $msi 'ContourRootDefault')
    } }
    @{ Target = 'msi'; Name = 'previous folder is kept unless it is an old versioned default'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `Condition` FROM `InstallExecuteSequence` WHERE `Action` = ''ContourRootPrevious'''
        Test-Equal $PreviousRootCondition $(if ($rows.Count -gt 0) { $rows[0][0] } else { 'no such action' })
    } }
    @{ Target = 'msi'; Name = 'the recorded folder is resolved at uninstall and repair too'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `Condition` FROM `InstallExecuteSequence` WHERE `Action` = ''ContourRootRecorded'''
        Test-Equal 'CONTOUR_RECORDED_ROOT' $(if ($rows.Count -gt 0) { $rows[0][0] } else { 'no such action' })
    } }
    @{ Target = 'msi'; Name = 'unattended switches default to on and are public'; Check = {
        param($msi)
        $secure = ((Get-MsiProperty -Msi $msi -Name 'SecureCustomProperties') -split ';')
        $problems = foreach ($switch in $Switches) {
            $value = Get-MsiProperty -Msi $msi -Name $switch
            if ($value -ne '1') { "$switch is $(if ($null -eq $value) { 'absent' } else { $value })" }
            if ($secure -notcontains $switch) { "$switch is not in SecureCustomProperties" }
        }
        if (-not $problems) { return $null }
        "expected $($Switches -join ' and ') = 1 and secure, found $($problems -join '; ')"
    } }
    @{ Target = 'msi'; Name = 'a switch of 0 drops its feature'; Check = {
        param($msi)
        $problems = foreach ($gate in $FeatureGates) {
            $rows = Get-MsiRows -Msi $msi -Query ('SELECT `Level`, `Condition` FROM `Condition` WHERE `Feature_` = ''{0}''' -f $gate.Feature)
            $condition = '{0} = "0"' -f $gate.Switch
            $found = @($rows | Where-Object { $_[0] -eq '0' -and $_[1] -eq $condition })
            if ($found.Count -eq 0) { "$($gate.Feature) has no Level 0 condition $condition" }
        }
        if (-not $problems) { return $null }
        "expected a Level 0 condition per switch, found $($problems -join '; ')"
    } }
    @{ Target = 'msi'; Name = 'desktop shortcut is ours, not CPack''s'; Check = {
        param($msi)
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `Shortcut`, `Directory_`, `Component_` FROM `Shortcut`'
        $ours = @($rows | Where-Object { $_[2] -eq 'ContourDesktopShortcut' -and $_[1] -eq 'DesktopFolder' })
        $cpacks = @($rows | Where-Object { $_[0].StartsWith('CM_DSP_') } | ForEach-Object { $_[0] })
        if ($ours.Count -gt 0 -and $cpacks.Count -eq 0) { return $null }
        "expected one Shortcut in DesktopFolder of ContourDesktopShortcut and none named CM_DSP_*, found $($ours.Count) and $($cpacks -join ', ')"
    } }
    @{ Target = 'msi'; Name = 'Start menu shortcut is ours, not CPack''s'; Check = {
        param($msi)
        $components = Get-MsiRows -Msi $msi -Query 'SELECT `Component` FROM `Component`'
        $cpacks = @($components | Where-Object { $_[0].StartsWith('CM_SHORTCUT_') } | ForEach-Object { $_[0] })
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `Directory_` FROM `Shortcut` WHERE `Component_` = ''ContourStartMenuShortcut'''
        if ($rows.Count -eq 1 -and $rows[0][0] -eq 'ProgramMenuFolder' -and $cpacks.Count -eq 0) { return $null }
        "expected one Shortcut in ProgramMenuFolder of ContourStartMenuShortcut and no CM_SHORTCUT_* component, found $($rows.Count) and $($cpacks -join ', ')"
    } }
    @{ Target = 'msi'; Name = 'publisher'; Check = {
        param($msi)
        Test-Equal 'Contour Terminal' (Get-MsiProperty -Msi $msi -Name 'Manufacturer')
    } }
    @{ Target = 'msi'; Name = 'Add/Remove Programs icon'; Check = {
        param($msi)
        $name = Get-MsiProperty -Msi $msi -Name 'ARPPRODUCTICON'
        if (-not $name) { return 'expected an ARPPRODUCTICON property, found none' }
        $rows = Get-MsiRows -Msi $msi -Query ('SELECT `Name` FROM `Icon` WHERE `Name` = ''{0}''' -f $name)
        if ($rows.Count -gt 0) { return $null }
        "expected an Icon row named $name, found none"
    } }
    @{ Target = 'msi'; Name = 'Add/Remove Programs links'; Check = {
        param($msi)
        $problems = foreach ($link in $ArpLinks.GetEnumerator()) {
            $value = Get-MsiProperty -Msi $msi -Name $link.Key
            if ($value -ne $link.Value) { "$($link.Key) is $(if ($null -eq $value) { 'absent' } else { $value }), not $($link.Value)" }
        }
        if (-not $problems) { return $null }
        "expected the three ARP links, found $($problems -join '; ')"
    } }
    @{ Target = 'msi'; Name = 'contour.exe carries the product version'; Check = {
        param($msi)
        # A local build's ProductVersion has three fields where the file version always has four.
        $asFourFields = { param($v) (($v -split '\.') + '0', '0', '0', '0' | ForEach-Object { $_ })[0..3] -join '.' }
        $rows = Get-MsiRows -Msi $msi -Query 'SELECT `FileName`, `Version` FROM `File`'
        $exe = @($rows | Where-Object { ($_[0] -split '\|')[-1] -eq 'contour.exe' })
        if ($exe.Count -ne 1) { return "expected one File row named contour.exe, found $($exe.Count)" }
        $expected = & $asFourFields (Get-MsiProperty -Msi $msi -Name 'ProductVersion')
        $found = if ($exe[0][1]) { & $asFourFields $exe[0][1] } else { 'no version' }
        Test-Equal $expected $found
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
    $script:CurrentTarget = $subject.Target
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
