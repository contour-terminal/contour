<#
.SYNOPSIS
    Checks that one Contour MSI installs over another without uninstalling first.

.DESCRIPTION
    Always runs a read-only preflight on the two packages (no elevation, no installation). Unless
    -PreflightOnly is given, it then installs -From, installs -To over it, asserts the upgrade took, and
    removes -To again. The install flow changes the machine: run it on a CI runner, never on a box that
    has Contour installed (it refuses to start there). Whatever it installed or started is removed again,
    also when a row fails.

.PARAMETER From
    The MSI installed first.

.PARAMETER To
    The MSI installed over it.

.PARAMETER PreflightOnly
    Stop after the read-only package checks.

.PARAMETER LogDir
    Where the msiexec logs go. Created when missing.

.PARAMETER FromInstallRoot
    A folder passed as INSTALL_ROOT to the From install. An upgrade that names none keeps it.

.PARAMETER ToInstallRoot
    A folder passed as INSTALL_ROOT to the To install, which moves the product there.

.PARAMETER ToProperties
    More properties for the To install (DESKTOP_SHORTCUT=0, say). The checks follow them: a desktop shortcut is
    expected exactly where DESKTOP_SHORTCUT is not 0.

.PARAMETER ServiceLabel
    Registers a daemon task of this label with the From install, and after To starts it with the new install's
    contour.exe: a CLI verb, with no daemon running, which is what repairs a task whose program the upgrade removed.

.PARAMETER ForegroundDaemon
    Starts a daemon of this label by hand from the From install, hosting a program, and expects it to live through
    the upgrade on its staged copy. The From package must carry `daemon relocate` (the Relocates marker).

.PARAMETER RepairAfterUpgrade
    Repairs the To install (`msiexec /fa`) once it is in place, and expects the folder, the PATH entry and the
    recorded InstallRoot to be as they were, and the daemon relocator to name the installed folder.

.PARAMETER SideBySide
    The MSI of the other scope, installed beside To once it is in place, to see that each scope keeps its own
    folder, PATH and Start menu when the other installer comes and goes.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$From,
    [Parameter(Mandatory)][string]$To,
    [switch]$PreflightOnly,
    [string]$LogDir = (Join-Path ([System.IO.Path]::GetTempPath()) 'msi-upgrade'),
    [string]$FromInstallRoot,
    [string]$ToInstallRoot,
    [string[]]$ToProperties = @(),
    [string]$ServiceLabel,
    [string]$ForegroundDaemon,
    [switch]$RepairAfterUpgrade,
    [string]$SideBySide
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# msiexec outcomes. Only success passes: 3010 (reboot required) is a failure, because an upgrade must not
# need a reboot.
$ExitSuccess = 0

# How long one msiexec call may take: far past a real install, short of the step's timeout.
$MsiexecTimeoutMilliseconds = 600000

# How long one contour.exe call may take, as in the e2e script for the same verbs.
$ContourTimeoutMilliseconds = 60000

# One Windows Installer COM object serves every function below.
$installer = New-Object -ComObject WindowsInstaller.Installer

function Invoke-Com {
    <# Calls a COM member by name; the Windows Installer objects are IDispatch-only. #>
    param([Parameter(Mandatory)]$Target, [Parameter(Mandatory)][string]$Member,
          [string]$Kind = 'InvokeMethod', $Arguments = $null)
    return $Target.GetType().InvokeMember($Member, $Kind, $null, $Target, $Arguments)
}

function Read-MsiRows {
    <# Runs a SQL query against an open MSI database, read-only. One string[] per row; a table the package does not
       have has no rows. #>
    param([Parameter(Mandatory)]$Database, [Parameter(Mandatory)][string]$Query)
    try {
        $view = Invoke-Com $Database 'OpenView' -Arguments @($Query)
    } catch {
        return , @()
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

function Get-OptionalMsiProperty {
    <# One Property row of an open MSI database, or $null when the package has none. #>
    param([Parameter(Mandatory)]$Database, [Parameter(Mandatory)][string]$Name)
    $rows = Read-MsiRows -Database $Database -Query ('SELECT `Value` FROM `Property` WHERE `Property` = ''{0}''' -f $Name)
    if ($rows.Count -eq 0) { return $null }
    return $rows[0][0]
}

function Get-ShortcutNames {
    <# The long names of the package's shortcuts in a standard folder, as the files they make. #>
    param([Parameter(Mandatory)]$Database, [Parameter(Mandatory)][string]$Directory)
    $rows = Read-MsiRows -Database $Database -Query ('SELECT `Name` FROM `Shortcut` WHERE `Directory_` = ''{0}''' -f $Directory)
    return , @($rows | ForEach-Object { (($_[0] -split '\|')[-1]) + '.lnk' } | Sort-Object)
}

function Get-PackageFacts {
    <# What the checks need to know of a package, read once, read-only: its product, its scope (ALLUSERS = 1 is the
       per-machine package) and the shortcuts it makes. #>
    param([Parameter(Mandatory)][string]$Path)
    $database = Invoke-Com $installer 'OpenDatabase' -Arguments @((Resolve-Path $Path).Path, 0)
    $facts = @{}
    foreach ($name in 'ProductCode', 'UpgradeCode', 'ProductVersion') {
        $value = Get-OptionalMsiProperty -Database $database -Name $name
        if ($null -eq $value) { throw "$Path has no $name property" }
        $facts[$name] = $value
    }
    $facts.Scope = if ((Get-OptionalMsiProperty -Database $database -Name 'ALLUSERS') -eq '1') { 'machine' } else { 'user' }
    $facts.StartMenuShortcuts = Get-ShortcutNames -Database $database -Directory 'ProgramMenuFolder'
    $facts.DesktopShortcuts = Get-ShortcutNames -Database $database -Directory 'DesktopFolder'
    return $facts
}

function ConvertTo-ProductList {
    <# What RelatedProducts handed back, as the product codes in it. The host makes of the StringList whatever it likes:
       $null where no product shares the code, the string itself where one does, an array of strings for several, the
       COM list itself on another host. Unrolled, like the rest: a caller wraps it in @(...). #>
    param($List)
    if ($null -eq $List) { return @() }
    if ($List -is [string]) { return @($List) }
    if ($List -is [System.__ComObject]) {
        $count = [int](Invoke-Com $List 'Count' 'GetProperty')
        if ($count -eq 0) { return @() }
        return @(foreach ($index in 0..($count - 1)) { [string](Invoke-Com $List 'Item' 'GetProperty' @($index)) })
    }
    if ($List -is [System.Collections.IEnumerable]) { return @($List | ForEach-Object { [string]$_ }) }
    # Anything else that is a list by Count and Item. 0..-1 counts down, so an empty one is answered first.
    if ([int]$List.Count -eq 0) { return @() }
    return @(foreach ($index in 0..([int]$List.Count - 1)) { [string]$List.Item($index) })
}

function Get-RelatedProduct {
    <# The installed product codes sharing the UpgradeCode. #>
    param([Parameter(Mandatory)][string]$UpgradeCode)
    return ConvertTo-ProductList (Invoke-Com $installer 'RelatedProducts' 'GetProperty' @($UpgradeCode))
}

function Get-InstalledVersion {
    <# The VersionString Windows Installer recorded for an installed product. #>
    param([Parameter(Mandatory)][string]$ProductCode)
    return Invoke-Com $installer 'ProductInfo' 'GetProperty' @($ProductCode, 'VersionString')
}

function Test-Elevated {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Format-Root {
    <# A folder as the checks compare it: no trailing separator. #>
    param([string]$Path)
    return $Path.TrimEnd('\')
}

function Get-ScopeFacts {
    <# What differs between the two installers: where they install by default, which registry hive and
       environment key they write, and which Start menu and desktop they put shortcuts in. #>
    param([Parameter(Mandatory)][ValidateSet('machine', 'user')][string]$Scope)
    if ($Scope -eq 'machine') {
        return @{
            DefaultRoot = Format-Root (Join-Path $env:ProgramFiles 'Contour')
            SoftwareKey = 'HKLM:\SOFTWARE'
            EnvironmentKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
            StartMenu = [Environment]::GetFolderPath('CommonPrograms')
            Desktop = [Environment]::GetFolderPath('CommonDesktopDirectory')
        }
    }
    return @{
        DefaultRoot = Format-Root (Join-Path $env:LOCALAPPDATA 'Programs\Contour')
        SoftwareKey = 'HKCU:\Software'
        EnvironmentKey = 'HKCU:\Environment'
        StartMenu = [Environment]::GetFolderPath('Programs')
        Desktop = [Environment]::GetFolderPath('DesktopDirectory')
    }
}

function Get-InstallLocation {
    <# The InstallLocation of an installed product's Add/Remove Programs entry, through the installer itself, which
       knows the registry view and the scope; $null when there is none. #>
    param([Parameter(Mandatory)][string]$ProductCode)
    $value = Invoke-Com $installer 'ProductInfo' 'GetProperty' @($ProductCode, 'InstallLocation')
    if (-not $value) { return $null }
    return Format-Root $value
}

function Get-PathEntries {
    <# The entries of the scope's registry PATH, as stored (variables unexpanded), without trailing separators. #>
    param([Parameter(Mandatory)][string]$EnvironmentKey)
    $path = (Get-Item $EnvironmentKey).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    return @($path -split ';' | Where-Object { $_ } | ForEach-Object { Format-Root $_ })
}

function Get-StartMenuShortcuts {
    <# The names of Contour's shortcuts directly in a Start menu or desktop folder. A pipeline result: an empty
       folder gives nothing and one shortcut gives a bare string, so callers go through Get-ShortcutList. #>
    param([Parameter(Mandatory)][string]$Folder)
    Get-ChildItem -LiteralPath $Folder -Filter '*.lnk' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'Contour*' } | ForEach-Object { $_.Name } | Sort-Object
}

function Get-ShortcutList {
    <# Get-StartMenuShortcuts as an array whatever it found: PowerShell unrolls a returned array, and under strict
       mode `.Count` of nothing or of a string is an error. The comma keeps the array whole. #>
    param([Parameter(Mandatory)][string]$Folder)
    return , @(Get-StartMenuShortcuts -Folder $Folder)
}

function Get-Footprint {
    <# What an installation of a scope puts on the machine, as rows of (what, kind, path): Path is a file or folder,
       Registry a key, PathEntry a folder of the scope's registry PATH. #>
    param([Parameter(Mandatory)]$Facts, [Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)]$Package)
    $rows = @(
        @{ What = 'the install folder'; Kind = 'Path'; Path = $Root }
        @{ What = 'contour.exe'; Kind = 'Path'; Path = (Join-Path $Root 'bin\contour.exe') }
        @{ What = 'the PATH entry'; Kind = 'PathEntry'; Path = (Join-Path $Root 'bin') }
        @{ What = 'the Start menu key'; Kind = 'Registry'; Path = (Join-Path $Facts.SoftwareKey 'Contour\Contour') }
    )
    foreach ($name in $Package.StartMenuShortcuts) {
        $rows += @{ What = "the Start menu shortcut $name"; Kind = 'Path'; Path = (Join-Path $Facts.StartMenu $name) }
    }
    return , $rows
}

function Get-DesktopFootprint {
    <# The desktop shortcuts of an installation, which DESKTOP_SHORTCUT=0 leaves out. #>
    param([Parameter(Mandatory)]$Facts, [Parameter(Mandatory)]$Package)
    return , @($Package.DesktopShortcuts | ForEach-Object {
            @{ What = "the desktop shortcut $_"; Kind = 'Path'; Path = (Join-Path $Facts.Desktop $_) }
        })
}

function Get-Remnants {
    <# What an installation of Contour must never leave once it is gone, or its upgrade behind it: the keys the
       installers share a parent of, and the Start menu folder 0.8.0's installer made. #>
    param([Parameter(Mandatory)]$Facts)
    return , @(
        @{ What = 'the Software\Contour key'; Kind = 'Registry'; Path = (Join-Path $Facts.SoftwareKey 'Contour') }
        @{ What = 'the Software\Contour Terminal key'; Kind = 'Registry'; Path = (Join-Path $Facts.SoftwareKey 'Contour Terminal') }
        @{ What = 'the Start menu folder of 0.8.0'; Kind = 'Path'; Path = (Join-Path $Facts.StartMenu 'Contour') }
    )
}

function Test-FootprintRow {
    <# Whether one row of a footprint is on the machine. #>
    param([Parameter(Mandatory)]$Row, [Parameter(Mandatory)]$Facts)
    switch ($Row.Kind) {
        'Path' { return (Test-Path -LiteralPath $Row.Path) }
        'Registry' { return (Test-Path -LiteralPath $Row.Path) }
        'PathEntry' { return (@(Get-PathEntries -EnvironmentKey $Facts.EnvironmentKey | Where-Object { $_ -ieq $Row.Path }).Count -eq 1) }
    }
    throw "unknown footprint kind $($Row.Kind)"
}

function Assert-Footprint {
    <# Throws unless every row is present (-Present) or none is (-Absent). #>
    param([Parameter(Mandatory)]$Rows, [Parameter(Mandatory)]$Facts, [Parameter(Mandatory)][ValidateSet('Present', 'Absent')][string]$Expect)
    $wrong = foreach ($row in $Rows) {
        $there = Test-FootprintRow -Row $row -Facts $Facts
        if (($Expect -eq 'Present') -ne $there) { "$($row.What) ($($row.Path)) is $(if ($there) { 'there' } else { 'missing' })" }
    }
    if ($wrong) { throw "expected $($Expect.ToLower()): $(@($wrong) -join '; ')" }
}

function Start-BoundedProcess {
    <# Runs a program and waits for it, at most $TimeoutMilliseconds. Returns @{ ExitCode; Output; Stdout; Stderr; TimedOut }.
       Through System.Diagnostics.Process, not Start-Process -PassThru: that hands back an object which opens its
       handle only when asked, and a program that has ended by then leaves ExitCode null. Both streams are read
       asynchronously, so that a program that fills a pipe cannot block while this waits for it, and decoded as
       UTF-8. #>
    param([Parameter(Mandatory)][string]$FilePath, [Parameter(Mandatory)][string]$Arguments, [Parameter(Mandatory)][int]$TimeoutMilliseconds)
    $info = [System.Diagnostics.ProcessStartInfo]::new($FilePath)
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $info.StandardOutputEncoding = $utf8
    $info.StandardErrorEncoding = $utf8
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit($TimeoutMilliseconds)
    if ($timedOut) { $process.Kill($true) }
    # The wait for the process itself, which is still needed after a Kill before ExitCode is read. The streams are
    # waited for separately and with a bound: a descendant that holds the inherited pipe would keep them open.
    $process.WaitForExit()
    $null = [System.Threading.Tasks.Task]::WaitAll([System.Threading.Tasks.Task[]]@($stdout, $stderr), 10000)
    $outText = if ($stdout.IsCompleted) { $stdout.Result } else { '' }
    $errText = if ($stderr.IsCompleted) { $stderr.Result } else { '' }
    return @{ ExitCode = $process.ExitCode; Output = ($outText + $errText); Stdout = $outText; Stderr = $errText; TimedOut = $timedOut }
}

function Invoke-Msiexec {
    <# Runs `msiexec <Action> <Target> <Properties>` quietly with a verbose log named after LogName, in $LogDir;
       fails unless it exits 0 and prints the log tail on failure. #>
    param(
        [Parameter(Mandatory)][ValidateSet('/i', '/x', '/fa')][string]$Action,
        [Parameter(Mandatory)][string]$Target,
        [Parameter(Mandatory)][string]$LogName,
        [Parameter(Mandatory)][string]$What,
        [string[]]$Properties = @()
    )
    $Log = Join-Path $LogDir $LogName
    # Bounded: an install that never ends must fail in minutes, not hold the runner for hours.
    $process = Start-BoundedProcess -FilePath msiexec.exe -TimeoutMilliseconds $MsiexecTimeoutMilliseconds `
        -Arguments ((@($Action, $Target, '/qn', '/norestart', '/l*v', "`"$Log`"") + $Properties) -join ' ')
    if ($process.TimedOut) {
        throw "$What did not end within $($MsiexecTimeoutMilliseconds / 1000) s: msiexec was ended"
    }
    if ($process.ExitCode -ne $ExitSuccess) {
        if (Test-Path $Log) {
            Write-Host "--- last 60 lines of $Log ---"
            Get-Content $Log -Tail 60 | Write-Host
        }
        throw "$What failed: msiexec exited $($process.ExitCode) (only 0 passes; 3010 means a reboot was required)"
    }
}

function Invoke-Contour {
    <# Runs an installed contour.exe with arguments and waits for it, at most a minute (it is a GUI-subsystem
       program, which a plain call would not wait for). Returns @{ ExitCode; Output }. #>
    param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string[]]$Arguments)
    $out = Join-Path $LogDir 'contour.out'
    $err = Join-Path $LogDir 'contour.err'
    # An argument with a space or a quote reaches the program as one, as the e2e script for the same verbs does it.
    $quoted = $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }
    $process = Start-BoundedProcess -FilePath $Executable -Arguments ($quoted -join ' ') -TimeoutMilliseconds $ContourTimeoutMilliseconds
    if ($process.TimedOut) {
        throw "contour $($Arguments -join ' ') did not end within $($ContourTimeoutMilliseconds / 1000) s: it was ended"
    }
    [System.IO.File]::WriteAllText($out, $process.Stdout)
    [System.IO.File]::WriteAllText($err, $process.Stderr)
    Write-Host "     $Executable $($Arguments -join ' ') -> $($process.ExitCode)"
    return @{ ExitCode = $process.ExitCode; Output = "$($process.Output)" }
}

function Assert-Contour {
    <# Invoke-Contour, failing on a nonzero exit; returns the output. #>
    param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string[]]$Arguments)
    $result = Invoke-Contour -Executable $Executable -Arguments $Arguments
    if ($result.ExitCode -ne 0) { throw "contour $($Arguments -join ' ') exited $($result.ExitCode): $($result.Output)" }
    return $result.Output
}

function Wait-Until {
    <# Polls a condition once a second, for at most $Seconds. #>
    param([Parameter(Mandatory)][scriptblock]$Condition, [int]$Seconds = 30)
    foreach ($second in 0..$Seconds) {
        if (& $Condition) { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

function Get-DaemonSocket {
    <# Where a daemon of the label listens: <temp>\contour-<USERNAME>\<label>. #>
    param([Parameter(Mandatory)][string]$Label)
    return Join-Path (Join-Path ([System.IO.Path]::GetTempPath()) "contour-$env:USERNAME") $Label
}

function Get-ProcessIdOfPing {
    <# The id of the long ping a test hosts in a daemon's session, or $null. #>
    $ping = Get-CimInstance Win32_Process -Filter "Name = 'PING.EXE'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match '-n 3600' } | Select-Object -First 1
    if ($ping) { return [int]$ping.ProcessId }
    return $null
}

function Get-DaemonPid {
    <# The process id a `daemon status` text names; throws where it names none, so that a comparison against it
       cannot pass vacuously. #>
    param([Parameter(Mandatory)][string]$Status)
    $id = [int]([regex]::Match($Status, '\(pid (\d+)\)').Groups[1].Value)
    if ($id -eq 0) { throw "the daemon status names no process id: $Status" }
    return $id
}

# Each check is one row: a name and a scriptblock that throws on failure. The install-flow rows run in order
# and share $context; later series add rows instead of editing logic.
$fromPackage = Get-PackageFacts -Path $From
$toPackage = Get-PackageFacts -Path $To
$scope = $toPackage.Scope
$facts = Get-ScopeFacts -Scope $scope
$desktopWanted = -not ($ToProperties -contains 'DESKTOP_SHORTCUT=0')
$context = @{
    From         = $fromPackage
    To           = $toPackage
    Scope        = $scope
    Facts        = $facts
    # Where the later package ends up: the folder asked for, else the one the earlier install had, else the default.
    ExpectedRoot = if ($ToInstallRoot) { Format-Root $ToInstallRoot } elseif ($FromInstallRoot) { Format-Root $FromInstallRoot } else { $facts.DefaultRoot }
    FromRoot     = $null
    # Whether the machine was free of Contour when this started: only then is what is found afterwards this run's.
    OwnsProducts = $false
    # Daemons this run started, to be ended by it: label, and the process ids it saw.
    Labels       = @()
    Pids         = @()
}
if ($SideBySide) {
    $context.Other = Get-PackageFacts -Path $SideBySide
    $context.OtherFacts = Get-ScopeFacts -Scope $context.Other.Scope
    $context.OtherRoot = $context.OtherFacts.DefaultRoot
}

$preflight = @(
    @{ Name = 'both packages belong to one product family'; Run = {
            if ($context.From.UpgradeCode -ne $context.To.UpgradeCode) {
                throw "$From has UpgradeCode $($context.From.UpgradeCode) but $To has $($context.To.UpgradeCode)"
            }
        } }
    @{ Name = 'the packages have different ProductCodes'; Run = {
            if ($context.From.ProductCode -eq $context.To.ProductCode) {
                throw "$From and $To share ProductCode $($context.From.ProductCode): Windows Installer will refuse the second as 'another version of this product is already installed' (1638)"
            }
        } }
    @{ Name = 'both packages install for the same scope'; Run = {
            if ($context.From.Scope -ne $context.Scope) { throw "$From installs for $($context.From.Scope) but $To for $($context.Scope)" }
        } }
    @{ Name = 'the package beside it is of the other scope'; Run = {
            if (-not $SideBySide) { return }
            if ($context.Other.Scope -eq $context.Scope) { throw "$SideBySide installs for the same scope as $To" }
            if ($context.Other.UpgradeCode -eq $context.To.UpgradeCode) { throw "$SideBySide shares $To's UpgradeCode: it would upgrade it" }
        } }
    @{ Name = 'the package makes the shortcuts the checks look for'; Run = {
            if ($context.To.StartMenuShortcuts.Count -eq 0) { throw "$To makes no Start menu shortcut (the Shortcut table names none in ProgramMenuFolder)" }
            if ($context.To.DesktopShortcuts.Count -eq 0) { throw "$To makes no desktop shortcut (the Shortcut table names none in DesktopFolder)" }
        } }
    @{ Name = 'the helpers hold on an empty folder and on a folder of one file, under strict mode'; Run = {
            # The class of mistake that failed every install run once: a helper's result unrolled to nothing or to a
            # string, and `.Count` of that an error under strict mode. Nothing here installs, so a preflight
            # catches it.
            $folder = Join-Path ([System.IO.Path]::GetTempPath()) ('contour-probe-' + [guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Force $folder | Out-Null
            try {
                $none = Get-ShortcutList -Folder $folder
                if ($none.Count -ne 0) { throw "an empty folder lists $($none.Count) shortcuts" }
                New-Item -ItemType File (Join-Path $folder 'Contour Probe.lnk') | Out-Null
                New-Item -ItemType File (Join-Path $folder 'Other.lnk') | Out-Null
                $one = Get-ShortcutList -Folder $folder
                if ($one.Count -ne 1 -or $one[0] -ne 'Contour Probe.lnk') { throw "a folder of one Contour shortcut lists $($one.Count)" }
                $rows = @(@{ What = 'the probe'; Kind = 'Path'; Path = (Join-Path $folder 'Contour Probe.lnk') })
                Assert-Footprint -Rows $rows -Facts $context.Facts -Expect Present
                $absent = @(@{ What = 'the probe'; Kind = 'Path'; Path = (Join-Path $folder 'Gone.lnk') })
                Assert-Footprint -Rows $absent -Facts $context.Facts -Expect Absent
            } finally {
                Remove-Item -Recurse -Force $folder -ErrorAction SilentlyContinue
            }
        } }
    @{ Name = 'a product list is read whatever shape the host hands it back in'; Run = {
            # $null (none installed), a plain string (one), an array (several) and a list by Count and Item.
            $list = [pscustomobject]@{ Count = 2 }
            $list | Add-Member -MemberType ScriptMethod -Name Item -Value { param($index) @('{X}', '{Y}')[$index] }
            $emptyList = [pscustomobject]@{ Count = 0 }
            $emptyList | Add-Member -MemberType ScriptMethod -Name Item -Value { param($index) @('{X}', '{Y}')[$index] }
            $shapes = [ordered]@{
                'null'          = @($null, @())
                'one string'    = @('{A}', @('{A}'))
                'several'       = @(@('{A}', '{B}'), @('{A}', '{B}'))
                'Count and Item' = @($list, @('{X}', '{Y}'))
                'Count and Item, empty' = @($emptyList, @())
            }
            foreach ($name in $shapes.Keys) {
                $got = @(ConvertTo-ProductList $shapes[$name][0])
                if (($got -join '|') -ne (@($shapes[$name][1]) -join '|')) { throw "a product list of the shape '$name' reads as '$($got -join '|')', expected '$(@($shapes[$name][1]) -join '|')'" }
            }
        } }
    @{ Name = 'the cleanup reads a context that has no second package'; Run = {
            # Without -SideBySide there is no OtherRoot: a hashtable key read as a property throws under strict mode.
            $roots = @(Get-CleanupRoot -Context @{ ExpectedRoot = 'C:\a'; FromRoot = $null })
            if (($roots -join '|') -ne 'C:\a') { throw "the cleanup roots are '$($roots -join '|')', expected C:\a" }
            $roots = @(Get-CleanupRoot -Context @{ ExpectedRoot = 'C:\a'; FromRoot = 'C:\b'; OtherRoot = 'C:\c' })
            if (($roots -join '|') -ne 'C:\a|C:\b|C:\c') { throw "the cleanup roots are '$($roots -join '|')', expected C:\a|C:\b|C:\c" }
        } }
    @{ Name = 'a program is run with its exit code and its output, and ended when it overstays'; Run = {
            # Fast programs, many times: the exit code of one that ended before it was asked for is what once came back null.
            foreach ($round in 1..40) {
                $ran = Start-BoundedProcess -FilePath cmd.exe -Arguments '/c echo done& exit 3' -TimeoutMilliseconds 30000
                if ($null -eq $ran.ExitCode -or $ran.ExitCode -ne 3 -or $ran.Output.Trim() -ne 'done' -or $ran.TimedOut) {
                    throw "round ${round}: a program that printed done and exited 3 came back as exit code '$($ran.ExitCode)', output '$($ran.Output.Trim())', timed out $($ran.TimedOut)"
                }
            }
            $slow = Start-BoundedProcess -FilePath cmd.exe -Arguments '/c ping -n 30 127.0.0.1 > nul' -TimeoutMilliseconds 500
            if (-not $slow.TimedOut) { throw 'a program that overstayed its time was not ended' }
            $split = Start-BoundedProcess -FilePath cmd.exe -Arguments '/c echo out& echo err 1>&2' -TimeoutMilliseconds 30000
            if ($split.Stdout.Trim() -ne 'out' -or $split.Stderr.Trim() -ne 'err') { throw "the two streams came back as '$($split.Stdout.Trim())' and '$($split.Stderr.Trim())', expected out and err" }
        } }
    @{ Name = 'an UpgradeCode no installed product shares lists none'; Run = {
            # The empty answer must be a list of none, whatever Windows Installer hands back for it (the runner's
            # handed back $null, which once ended the first row of every run with a null Target).
            $none = @(Get-RelatedProduct -UpgradeCode ('{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}'))
            if ($none.Count -ne 0) { throw "an unused UpgradeCode lists $($none.Count) products" }
        } }
)

$installFlow = @(
    @{ Name = 'no Contour is installed yet'; Run = {
            $codes = @($context.To.UpgradeCode) + $(if ($SideBySide) { $context.Other.UpgradeCode })
            $installed = @($codes | ForEach-Object { Get-RelatedProduct -UpgradeCode $_ })
            if ($installed.Count -gt 0) {
                $names = $installed | ForEach-Object { "$_ ($(Get-InstalledVersion -ProductCode $_))" }
                throw "refusing to install: Contour is already installed on this machine: $($names -join ', ')"
            }
            $context.OwnsProducts = $true
        } }
    @{ Name = 'the check runs elevated'; Run = {
            if (-not (Test-Elevated)) { throw 'the install flow needs an elevated shell' }
        } }
    @{ Name = 'the earlier package installs'; Run = {
            $properties = if ($FromInstallRoot) { @("INSTALL_ROOT=`"$FromInstallRoot`"") } else { @() }
            Invoke-Msiexec -What "installing $From" -LogName 'from.log' -Action /i -Target "`"$((Resolve-Path $From).Path)`"" -Properties $properties
            $context.FromRoot = Get-InstallLocation -ProductCode $context.From.ProductCode
            if (-not $context.FromRoot) { throw 'the earlier package left no InstallLocation in its Add/Remove Programs entry' }
            Write-Host "     the earlier package is in $($context.FromRoot)"
            if ($FromInstallRoot -and $context.FromRoot -ne (Format-Root $FromInstallRoot)) {
                throw "the earlier package went to $($context.FromRoot), not to $FromInstallRoot"
            }
        } }
    @{ Name = 'a daemon task is registered with the earlier package'; Run = {
            if (-not $ServiceLabel) { return }
            Assert-Contour -Executable (Join-Path $context.FromRoot 'bin\contour.exe') -Arguments @('daemon-service', 'install', '--label', $ServiceLabel) | Out-Null
            $context.Labels += $ServiceLabel
        } }
    @{ Name = 'a daemon started by hand from the earlier package hosts a program'; Run = {
            if (-not $ForegroundDaemon) { return }
            $exe = Join-Path $context.FromRoot 'bin\contour.exe'
            $context.Labels += $ForegroundDaemon
            Start-Process -FilePath $exe -ArgumentList @('daemon', '--label', $ForegroundDaemon) -WindowStyle Hidden | Out-Null
            $socket = Get-DaemonSocket -Label $ForegroundDaemon
            if (-not (Wait-Until { (Invoke-Contour -Executable $exe -Arguments @('daemon', 'status', '--label', $ForegroundDaemon)).ExitCode -eq 0 })) {
                throw "the daemon $ForegroundDaemon did not come up"
            }
            Assert-Contour -Executable $exe -Arguments @('new', 'daemon', $socket, 'json', 'ping', '-n', '3600', '127.0.0.1') | Out-Null
            $context.Ping = $null
            if (-not (Wait-Until { $context.Ping = Get-ProcessIdOfPing; [bool]$context.Ping })) { throw 'the hosted ping did not start' }
            $context.DaemonPid = Get-DaemonPid -Status (Assert-Contour -Executable $exe -Arguments @('daemon', 'status', '--label', $ForegroundDaemon))
            $context.Pids += @($context.DaemonPid, $context.Ping)
            Write-Host "     daemon $($context.DaemonPid) hosts ping $($context.Ping)"
        } }
    @{ Name = 'the later package installs over it'; Run = {
            $properties = @(if ($ToInstallRoot) { "INSTALL_ROOT=`"$ToInstallRoot`"" }) + $ToProperties
            Invoke-Msiexec -What "upgrading to $To" -LogName 'to.log' -Action /i -Target "`"$((Resolve-Path $To).Path)`"" -Properties $properties
        } }
    @{ Name = 'the installer asked the earlier install to relocate daemons, before it validated anything'; Run = {
            # The custom action is a type 50 one whose command is built from a path with a space in the default
            # folder: its failure is ignored, so only the log shows that it ran and what it returned.
            if (-not $ForegroundDaemon) { return }
            $log = Get-Content -LiteralPath (Join-Path $LogDir 'to.log') -Raw
            # What the log must show, as rows: each line must be there (a Return value 3, or a missing line, fails
            # the row), and each must come before InstallValidate starts.
            $mustShow = @(
                @{ What = 'ContourRelocateDaemons starts'; Pattern = 'Action start [\d:]+: ContourRelocateDaemons\.' }
                @{ What = 'ContourRelocateDaemons ends with Return value 1'; Pattern = 'Action ended [\d:]+: ContourRelocateDaemons\. Return value 1\.' }
            )
            $validate = [regex]::Match($log, 'Action start [\d:]+: InstallValidate\.')
            if (-not $validate.Success) { throw 'the log shows no start of InstallValidate' }
            $problems = foreach ($row in $mustShow) {
                $found = [regex]::Match($log, $row.Pattern)
                if (-not $found.Success) { "the log does not show that $($row.What)" }
                elseif ($found.Index -gt $validate.Index) { "$($row.What) only after InstallValidate" }
            }
            if ($problems) { throw @($problems) -join '; ' }
        } }
    @{ Name = 'exactly the later package is installed, at its full version'; Run = {
            $installed = @(Get-RelatedProduct -UpgradeCode $context.To.UpgradeCode)
            if ($installed.Count -ne 1 -or $installed[0] -ne $context.To.ProductCode) {
                throw "expected only $($context.To.ProductCode) installed, found: $($installed -join ', ')"
            }
            $version = Get-InstalledVersion -ProductCode $context.To.ProductCode
            if ($version -ne $context.To.ProductVersion) {
                throw "installed version is '$version', expected '$($context.To.ProductVersion)'"
            }
        } }
    @{ Name = 'the later package is in the expected folder'; Run = {
            $location = Get-InstallLocation -ProductCode $context.To.ProductCode
            if ($location -ne $context.ExpectedRoot) { throw "InstallLocation is '$location', expected '$($context.ExpectedRoot)'" }
        } }
    @{ Name = "the later package has what it installs: folder, PATH entry, Start menu, and a desktop shortcut unless declined"; Run = {
            Assert-Footprint -Rows (Get-Footprint -Facts $context.Facts -Root $context.ExpectedRoot -Package $context.To) -Facts $context.Facts -Expect Present
            $desktop = Get-DesktopFootprint -Facts $context.Facts -Package $context.To
            Assert-Footprint -Rows $desktop -Facts $context.Facts -Expect $(if ($desktopWanted) { 'Present' } else { 'Absent' })
        } }
    @{ Name = 'nothing of the earlier package or of 0.8.0 is left beside it'; Run = {
            # The earlier folder (when it is another), its PATH entry, and the Start menu folder of 0.8.0.
            $remnants = Get-Remnants -Facts $context.Facts
            $rows = @($remnants | Where-Object { $_.What -like '*0.8.0*' })
            if ($context.FromRoot -ne $context.ExpectedRoot) {
                $rows += @{ What = 'the earlier install folder'; Kind = 'Path'; Path = $context.FromRoot }
                $rows += @{ What = 'the earlier PATH entry'; Kind = 'PathEntry'; Path = (Join-Path $context.FromRoot 'bin') }
            }
            Assert-Footprint -Rows $rows -Facts $context.Facts -Expect Absent
        } }
    @{ Name = 'the daemon task is repaired by starting it, and runs the later package from a staged copy'; Run = {
            if (-not $ServiceLabel) { return }
            # A CLI verb, with no daemon running: the repair of a GUI start runs only while one is served, so
            # this is the way to reach it from here (daemon-service start repairs a task whose program is gone).
            $exe = Join-Path $context.ExpectedRoot 'bin\contour.exe'
            Assert-Contour -Executable $exe -Arguments @('daemon-service', 'start', '--label', $ServiceLabel) | Out-Null
            $status = Assert-Contour -Executable $exe -Arguments @('daemon-service', 'status', '--label', $ServiceLabel)
            if ($status.IndexOf($exe, [StringComparison]::OrdinalIgnoreCase) -lt 0) { throw "the task does not run $exe`: $status" }
            # The daemon the task starts serves, and from a staged copy: a repaired task of 0.8.0 once started it in
            # the foreground, from the folder it was pointed at, and held that folder at every logon.
            if (-not (Wait-Until { (Invoke-Contour -Executable $exe -Arguments @('daemon', 'status', '--label', $ServiceLabel)).ExitCode -eq 0 })) {
                throw "the daemon of the task $ServiceLabel did not come up"
            }
            $daemon = Assert-Contour -Executable $exe -Arguments @('daemon', 'status', '--label', $ServiceLabel)
            $staged = Join-Path $env:LOCALAPPDATA 'contour\bin\'
            $executable = [regex]::Match($daemon, '(?m)^executable:\s+(.*?)\s*$').Groups[1].Value
            if (-not $executable.StartsWith($staged, [StringComparison]::OrdinalIgnoreCase)) { throw "the task's daemon runs $executable, not from under $staged" }
            $context.Pids += Get-DaemonPid -Status $daemon
            # Stops the daemon over its socket, too.
            Assert-Contour -Executable $exe -Arguments @('daemon-service', 'uninstall', '--label', $ServiceLabel) | Out-Null
            $context.Labels = @($context.Labels | Where-Object { $_ -ne $ServiceLabel })
        } }
    @{ Name = 'the hand-started daemon lived through the upgrade, on its staged copy'; Run = {
            if (-not $ForegroundDaemon) { return }
            $exe = Join-Path $context.ExpectedRoot 'bin\contour.exe'
            if (-not (Get-Process -Id $context.Ping -ErrorAction SilentlyContinue)) { throw "the hosted ping $($context.Ping) is gone" }
            $status = Assert-Contour -Executable $exe -Arguments @('daemon', 'status', '--label', $ForegroundDaemon)
            $staged = Join-Path $env:LOCALAPPDATA 'contour\bin\'
            $executable = [regex]::Match($status, '(?m)^executable:\s+(.*?)\s*$').Groups[1].Value
            if (-not $executable.StartsWith($staged, [StringComparison]::OrdinalIgnoreCase)) { throw "the daemon runs $executable, not from under $staged" }
            $daemonPid = Get-DaemonPid -Status $status
            if ($daemonPid -eq $context.DaemonPid) { throw "the daemon is still process ${daemonPid}: it was not moved" }
            $context.Pids += $daemonPid
            # Handed over, not copied: the old daemon is gone, and the successor hosts the one session.
            if (-not (Wait-Until { -not (Get-Process -Id $context.DaemonPid -ErrorAction SilentlyContinue) })) { throw "the old daemon $($context.DaemonPid) still runs" }
            if ($status -notmatch '(?m)^sessions:\s+1\s*$') { throw "the successor does not host exactly the one session: $status" }
            Assert-Contour -Executable $exe -Arguments @('daemon', 'stop', '--label', $ForegroundDaemon, '--yes') | Out-Null
            Stop-Process -Id $context.Ping -Force -ErrorAction SilentlyContinue
            $context.Labels = @($context.Labels | Where-Object { $_ -ne $ForegroundDaemon })
        } }
    @{ Name = 'a repair leaves the folder, PATH entry and InstallRoot alone, and the relocator names the installed folder'; Run = {
            if (-not $RepairAfterUpgrade) { return }
            $key = Join-Path $context.Facts.SoftwareKey 'Contour\Contour'
            $recorded = Format-Root (Get-ItemProperty -LiteralPath $key).InstallRoot
            if ($recorded -ne $context.ExpectedRoot) { throw "InstallRoot is '$recorded' before the repair, expected '$($context.ExpectedRoot)'" }
            Invoke-Msiexec -What "repairing $($context.To.ProductCode)" -LogName 'repair.log' -Action /fa -Target $context.To.ProductCode
            $after = Format-Root (Get-ItemProperty -LiteralPath $key).InstallRoot
            if ($after -ne $context.ExpectedRoot) { throw "the repair changed InstallRoot to '$after'" }
            $location = Get-InstallLocation -ProductCode $context.To.ProductCode
            if ($location -ne $context.ExpectedRoot) { throw "the repair changed InstallLocation to '$location'" }
            Assert-Footprint -Rows (Get-Footprint -Facts $context.Facts -Root $context.ExpectedRoot -Package $context.To) -Facts $context.Facts -Expect Present
            $log = Get-Content -LiteralPath (Join-Path $LogDir 'repair.log') -Raw
            $relocator = Join-Path $context.ExpectedRoot 'bin\contour.exe'
            if (-not [regex]::IsMatch($log, 'ContourRelocator\b[^\r\n]*' + [regex]::Escape($relocator))) {
                throw "the repair's log does not show the relocator pointed at $relocator"
            }
        } }
    @{ Name = 'the other scope installs beside it, and both have what they install'; Run = {
            if (-not $SideBySide) { return }
            Invoke-Msiexec -What "installing $SideBySide" -LogName 'other.log' -Action /i -Target "`"$((Resolve-Path $SideBySide).Path)`""
            Assert-Footprint -Rows (Get-Footprint -Facts $context.OtherFacts -Root $context.OtherRoot -Package $context.Other) -Facts $context.OtherFacts -Expect Present
            Assert-Footprint -Rows (Get-Footprint -Facts $context.Facts -Root $context.ExpectedRoot -Package $context.To) -Facts $context.Facts -Expect Present
        } }
    @{ Name = 'removing the other scope takes what it installed and no more'; Run = {
            if (-not $SideBySide) { return }
            Invoke-Msiexec -What "uninstalling $($context.Other.ProductCode)" -LogName 'other-remove.log' -Action /x -Target $context.Other.ProductCode
            $gone = (Get-Footprint -Facts $context.OtherFacts -Root $context.OtherRoot -Package $context.Other) + (Get-DesktopFootprint -Facts $context.OtherFacts -Package $context.Other)
            Assert-Footprint -Rows $gone -Facts $context.OtherFacts -Expect Absent
            Assert-Footprint -Rows (Get-Footprint -Facts $context.Facts -Root $context.ExpectedRoot -Package $context.To) -Facts $context.Facts -Expect Present
            Invoke-Msiexec -What "installing $SideBySide again" -LogName 'other-again.log' -Action /i -Target "`"$((Resolve-Path $SideBySide).Path)`""
        } }
    @{ Name = 'the later package uninstalls cleanly'; Run = {
            Invoke-Msiexec -What "uninstalling $($context.To.ProductCode)" -LogName 'remove.log' -Action /x -Target $context.To.ProductCode
            $left = @(Get-RelatedProduct -UpgradeCode $context.To.UpgradeCode)
            if ($left.Count -gt 0) { throw "still installed after uninstall: $($left -join ', ')" }
        } }
    @{ Name = 'nothing of the later package is left'; Run = {
            $rows = (Get-Footprint -Facts $context.Facts -Root $context.ExpectedRoot -Package $context.To) + (Get-DesktopFootprint -Facts $context.Facts -Package $context.To) + (Get-Remnants -Facts $context.Facts)
            if ($context.FromRoot -ne $context.ExpectedRoot) { $rows += @{ What = 'the earlier install folder'; Kind = 'Path'; Path = $context.FromRoot } }
            Assert-Footprint -Rows $rows -Facts $context.Facts -Expect Absent
        } }
    @{ Name = 'the other scope is untouched by it, and leaves nothing either when it uninstalls'; Run = {
            if (-not $SideBySide) { return }
            Assert-Footprint -Rows (Get-Footprint -Facts $context.OtherFacts -Root $context.OtherRoot -Package $context.Other) -Facts $context.OtherFacts -Expect Present
            Invoke-Msiexec -What "uninstalling $($context.Other.ProductCode)" -LogName 'other-last.log' -Action /x -Target $context.Other.ProductCode
            $rows = (Get-Footprint -Facts $context.OtherFacts -Root $context.OtherRoot -Package $context.Other) + (Get-DesktopFootprint -Facts $context.OtherFacts -Package $context.Other) + (Get-Remnants -Facts $context.OtherFacts)
            Assert-Footprint -Rows $rows -Facts $context.OtherFacts -Expect Absent
        } }
)

function Get-CleanupRoot {
    <# The install folders a run may have used, those it has: a context without a second package has no OtherRoot. #>
    param([hashtable]$Context)
    return @('ExpectedRoot', 'FromRoot', 'OtherRoot' | Where-Object { $Context.ContainsKey($_) -and $Context[$_] } | ForEach-Object { $Context[$_] })
}

function Remove-Leftovers {
    <# Whatever this run installed or started, removed again, each step on its own: the daemons it started (by their
       label, and by the process ids it saw), and the products of both upgrade codes, but only where the machine
       had none of them when it began. Never throws. #>
    foreach ($step in @(
            { foreach ($label in $context.Labels) {
                    foreach ($root in @(Get-CleanupRoot -Context $context)) {
                        $exe = if ($root) { Join-Path $root 'bin\contour.exe' }
                        if ($exe -and (Test-Path -LiteralPath $exe)) {
                            Invoke-Contour -Executable $exe -Arguments @('daemon-service', 'uninstall', '--label', $label) | Out-Null
                            Invoke-Contour -Executable $exe -Arguments @('daemon', 'stop', '--label', $label, '--yes') | Out-Null
                            break
                        }
                    }
                } },
            { foreach ($id in $context.Pids) { if ($id) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue } } },
            { if ($context.OwnsProducts) {
                    $codes = @($context.To.UpgradeCode) + $(if ($SideBySide) { $context.Other.UpgradeCode })
                    foreach ($product in @($codes | ForEach-Object { Get-RelatedProduct -UpgradeCode $_ })) {
                        # Each on its own: one that will not go must not keep the others installed.
                        try { Invoke-Msiexec -What "removing $product" -LogName "cleanup-$($product.Trim('{}')).log" -Action /x -Target $product }
                        catch { Write-Host "     cleanup: $($_.Exception.Message)" }
                    }
                } })) {
        try { $null = & $step } catch { Write-Host "     cleanup: $($_.Exception.Message)" }
    }
    # What is still installed is said here, by name: the next run would otherwise refuse to start with nothing but the
    # fact that Contour is installed.
    if ($context.OwnsProducts) {
        try {
            $codes = @($context.To.UpgradeCode) + $(if ($SideBySide) { $context.Other.UpgradeCode })
            $left = @($codes | ForEach-Object { Get-RelatedProduct -UpgradeCode $_ })
            foreach ($product in $left) { Write-Host "     cleanup: STILL INSTALLED: $product ($(Get-InstalledVersion -ProductCode $product)); remove it before the next run" }
            return $left.Count
        } catch { Write-Host "     cleanup: could not list what is left: $($_.Exception.Message)" }
    }
    return 0
}

Write-Host "from: $From  ProductCode $($context.From.ProductCode)  version $($context.From.ProductVersion)"
Write-Host "to:   $To  ProductCode $($context.To.ProductCode)  version $($context.To.ProductVersion)  ($scope)"

if (-not $PreflightOnly) {
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
}

$checks = if ($PreflightOnly) { $preflight } else { $preflight + $installFlow }
$failed = $false
try {
    foreach ($check in $checks) {
        try {
            & $check.Run
            Write-Host "ok   $($check.Name)"
        } catch {
            Write-Host "FAIL $($check.Name): $($_.Exception.Message)"
            $failed = $true
            break
        }
    }
} finally {
    if (-not $PreflightOnly) { if ((Remove-Leftovers) -gt 0) { $failed = $true } }
}
exit $(if ($failed) { 1 } else { 0 })
