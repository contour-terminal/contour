<#
.SYNOPSIS
    Checks that one Contour MSI installs over another without uninstalling first.

.DESCRIPTION
    Always runs a read-only preflight on the two packages (no elevation, no installation). Unless
    -PreflightOnly is given, it then installs -From, installs -To over it, asserts the upgrade took, and
    removes -To again. The install flow changes the machine: run it on a CI runner, never on a box that
    has Contour installed (it refuses to start there).

.PARAMETER From
    The MSI installed first.

.PARAMETER To
    The MSI installed over it.

.PARAMETER PreflightOnly
    Stop after the read-only package checks.

.PARAMETER LogDir
    Where the msiexec logs go. Created when missing.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$From,
    [Parameter(Mandatory)][string]$To,
    [switch]$PreflightOnly,
    [string]$LogDir = (Join-Path ([System.IO.Path]::GetTempPath()) 'msi-upgrade')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# msiexec outcomes. Only success passes: 3010 (reboot required) is a failure, because an upgrade must not
# need a reboot.
$ExitSuccess = 0

# One Windows Installer COM object serves every function below.
$installer = New-Object -ComObject WindowsInstaller.Installer

function Get-MsiProperty {
    <# Reads ProductCode, UpgradeCode and ProductVersion from the MSI's Property table, read-only. #>
    param([Parameter(Mandatory)][string]$Path)
    $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @((Resolve-Path $Path).Path, 0))
    $result = @{}
    foreach ($name in 'ProductCode', 'UpgradeCode', 'ProductVersion') {
        $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database,
            @('SELECT `Value` FROM `Property` WHERE `Property` = ''{0}''' -f $name))
        $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
        $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if ($null -eq $record) {
            throw "$Path has no $name property"
        }
        $result[$name] = $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @(1))
        $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
    }
    return $result
}

function Get-RelatedProduct {
    <# The installed product codes sharing the UpgradeCode. #>
    param([Parameter(Mandatory)][string]$UpgradeCode)
    $list = $installer.GetType().InvokeMember('RelatedProducts', 'GetProperty', $null, $installer, @($UpgradeCode))
    # A StringList COM object, not an array: read it by Count and Item.
    $count = $list.GetType().InvokeMember('Count', 'GetProperty', $null, $list, $null)
    if ($count -eq 0) { return @() }
    return @(foreach ($index in 0..($count - 1)) {
            $list.GetType().InvokeMember('Item', 'GetProperty', $null, $list, @($index))
        })
}

function Get-InstalledVersion {
    <# The VersionString Windows Installer recorded for an installed product. #>
    param([Parameter(Mandatory)][string]$ProductCode)
    return $installer.GetType().InvokeMember('ProductInfo', 'GetProperty', $null, $installer, @($ProductCode, 'VersionString'))
}

function Test-Elevated {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-Msiexec {
    <# Runs `msiexec <Action> <Target>` quietly with a verbose log named after LogName, in $LogDir; fails unless it
       exits 0 and prints the log tail on failure. #>
    param(
        [Parameter(Mandatory)][ValidateSet('/i', '/x')][string]$Action,
        [Parameter(Mandatory)][string]$Target,
        [Parameter(Mandatory)][string]$LogName,
        [Parameter(Mandatory)][string]$What
    )
    $Log = Join-Path $LogDir $LogName
    $process = Start-Process -FilePath msiexec.exe -Wait -PassThru -NoNewWindow `
        -ArgumentList @($Action, $Target, '/qn', '/norestart', '/l*v', "`"$Log`"")
    if ($process.ExitCode -ne $ExitSuccess) {
        if (Test-Path $Log) {
            Write-Host "--- last 60 lines of $Log ---"
            Get-Content $Log -Tail 60 | Write-Host
        }
        throw "$What failed: msiexec exited $($process.ExitCode) (only 0 passes; 3010 means a reboot was required)"
    }
}

# Each check is one row: a name and a scriptblock that throws on failure. The install-flow rows run in order
# and share $context; later series add rows (install folder, PATH, per-user MSI) instead of editing logic.
$context = @{
    From = Get-MsiProperty -Path $From
    To   = Get-MsiProperty -Path $To
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
)

$installFlow = @(
    @{ Name = 'no Contour is installed yet'; Run = {
            $installed = @(Get-RelatedProduct -UpgradeCode $context.To.UpgradeCode)
            if ($installed.Count -gt 0) {
                $names = $installed | ForEach-Object { "$_ ($(Get-InstalledVersion -ProductCode $_))" }
                throw "refusing to install: Contour is already installed on this machine: $($names -join ', ')"
            }
        } }
    @{ Name = 'the check runs elevated'; Run = {
            if (-not (Test-Elevated)) { throw 'the install flow needs an elevated shell' }
        } }
    @{ Name = 'the earlier package installs'; Run = {
            Invoke-Msiexec -What "installing $From" -LogName 'from.log' -Action /i -Target "`"$((Resolve-Path $From).Path)`""
        } }
    @{ Name = 'the later package installs over it'; Run = {
            Invoke-Msiexec -What "upgrading to $To" -LogName 'to.log' -Action /i -Target "`"$((Resolve-Path $To).Path)`""
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
    @{ Name = 'the later package uninstalls cleanly'; Run = {
            Invoke-Msiexec -What "uninstalling $($context.To.ProductCode)" -LogName 'remove.log' -Action /x -Target $context.To.ProductCode
            $left = @(Get-RelatedProduct -UpgradeCode $context.To.UpgradeCode)
            if ($left.Count -gt 0) { throw "still installed after uninstall: $($left -join ', ')" }
        } }
)

Write-Host "from: $From  ProductCode $($context.From.ProductCode)  version $($context.From.ProductVersion)"
Write-Host "to:   $To  ProductCode $($context.To.ProductCode)  version $($context.To.ProductVersion)"

if (-not $PreflightOnly) {
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
}

$checks = if ($PreflightOnly) { $preflight } else { $preflight + $installFlow }
foreach ($check in $checks) {
    try {
        & $check.Run
        Write-Host "ok   $($check.Name)"
    } catch {
        Write-Host "FAIL $($check.Name): $($_.Exception.Message)"
        exit 1
    }
}
exit 0
