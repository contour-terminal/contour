<#
.SYNOPSIS
    Windows code signing for the release packages.

.DESCRIPTION
    Usage, from the checkout's root (`pwsh -NoProfile -File scripts/sign-windows.ps1 <verb> ...`):
      sign-windows.ps1 plan   -Configured <name> -Builds <setting> -Build release|master|other
      sign-windows.ps1 tools  -Signer <name>
      sign-windows.ps1 sign   -Kind Payload|Packages -Root <dir> [-Files <a>,<b>] -Signer <name> [-Manifest <tsv>]
                              [-Trust System|Pinned] [-Thumbprint <sha1>]
      sign-windows.ps1 export -Kind Payload|Packages -Root <dir> [-Files <a>,<b>] -Signer <name> -Destination <dir> -Manifest <tsv>
      sign-windows.ps1 import -Root <dir> -From <dir> -Manifest <tsv> -Signer <name> [-Trust System|Pinned] [-Thumbprint <sha1>]
      sign-windows.ps1 stage  -BuildDirectory <build dir> -Destination <stage dir>
      sign-windows.ps1 dry-run -Root <stage dir> [-Packages <a.msi>,<b.msi>]
      sign-windows.ps1 verify -Signer <name> -Manifest <tsv> -Zip <zip> -Msi <a.msi>,<b.msi>

    plan    Decides which signer a build uses (from what the repository is configured for, which builds sign, and
            what kind of build this is) and prints the decision; in a workflow it is also appended to
            $env:GITHUB_OUTPUT as signer=, transport= and dry-run=.
    tools   Resolves every tool the signer's row names, downloading the pinned ones, and starts each the way a signing
            run does (its probe): a check that the signer's tools run here.
    sign    Signs the batch with the signer and checks the result.
    export  Copies the files a signer that signs elsewhere (SignPath) would sign out of the tree.
    import  Puts the files signed elsewhere back into the tree, after checking them against the signer's publisher.
    stage   Installs a build into a folder with the components CPack packages: the tree that is signed.
    dry-run Signs a copy of the tree and the packages with a throwaway certificate, and checks the result.
    verify  Checks that a ZIP and installers are what the signer should ship (nothing signed by a dry run, the signed
            payload in the ZIP and in each installer, signatures that Windows trusts).

    Every path may be relative to the current location. -Files of Packages are relative to -Root, or else to the
    current location. Common: -WorkDirectory (default out/signing/work), -ToolsDirectory (default out/signing/tools).

.PARAMETER Verb
    What to do: plan, tools, sign, export, import, stage, dry-run or verify.

.PARAMETER Signer
    The signer a verb works with (a key of the table).

.PARAMETER Configured
    For plan, the signer the repository is configured for. Empty means none.

.PARAMETER Builds
    For plan, which builds sign: releases or releases-and-master. Empty means releases.

.PARAMETER Build
    For plan, the kind of build: release (a release tag), master (a push to master) or other.

.PARAMETER TablePath
    A signer table other than the shipped signers.psd1 (the self-test's).

.PARAMETER WorkDirectory
    Scratch space of a signing run.

.PARAMETER ToolsDirectory
    Where pinned tools are downloaded to and reused from.

.PARAMETER Kind
    Payload (the files of an install tree) or Packages (the installers).

.PARAMETER Root
    The install tree, or the folder the packages are in.

.PARAMETER Files
    For Packages, the package files.

.PARAMETER Manifest
    The manifest of signed files (tab separated).

.PARAMETER Destination
    Where export puts the copies, or stage installs to.

.PARAMETER From
    Where import finds the signed copies.

.PARAMETER Trust
    System (Windows must trust the signature) or Pinned (a throwaway certificate, by Thumbprint).

.PARAMETER Thumbprint
    For Pinned, the signer certificate's thumbprint.

.PARAMETER BuildDirectory
    For stage, the build directory.

.PARAMETER Packages
    For dry-run, installers to sign a copy of.

.PARAMETER Zip
    For verify, the ZIP package.

.PARAMETER Msi
    For verify, the installers.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('plan', 'tools', 'sign', 'export', 'import', 'stage', 'dry-run', 'verify')][string]$Verb,
    [string]$Signer = '',
    [string]$Configured = '',
    [string]$Builds = '',
    [string]$Build = '',
    [string]$TablePath = '',
    [string]$WorkDirectory = 'out/signing/work',
    [string]$ToolsDirectory = 'out/signing/tools',
    [ValidateSet('Payload', 'Packages')][string]$Kind = 'Payload',
    [string]$Root = '',
    [string[]]$Files = @(),
    [string]$Manifest = '',
    [string]$Destination = '',
    [string]$From = '',
    [ValidateSet('System', 'Pinned')][string]$Trust = 'System',
    [string]$Thumbprint = '',
    [string]$BuildDirectory = '',
    [string[]]$Packages = @(),
    [string]$Zip = '',
    [string[]]$Msi = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'windows-signing/WindowsSigning.psm1') -Force

# One meaning for a relative path, whatever the process's directory is: the PowerShell location, once, here.
function Get-FullPath {
    param([string]$Path)
    if ([string]::IsNullOrEmpty($Path)) { return $Path }
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}
$WorkDirectory = if ($Verb -eq 'dry-run' -and -not $PSBoundParameters.ContainsKey('WorkDirectory')) { Get-FullPath 'out/signing/dry-run' } else { Get-FullPath $WorkDirectory }
$ToolsDirectory = Get-FullPath $ToolsDirectory
$TablePath = Get-FullPath $TablePath
$Root = Get-FullPath $Root
$Manifest = Get-FullPath $Manifest
$Destination = Get-FullPath $Destination
$From = Get-FullPath $From
$BuildDirectory = Get-FullPath $BuildDirectory
$Zip = Get-FullPath $Zip
# `pwsh -File` hands a list over as one string: a,b is split here, so that a step runs the same with -File and -Command.
function Split-List { param([string[]]$Value) return @($Value | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) }
$Files = Split-List $Files
$Msi = @(Split-List $Msi | ForEach-Object { Get-FullPath $_ })
$Packages = @(Split-List $Packages | ForEach-Object { Get-FullPath $_ })
# The package files stay as given: they are read against Root first, then the location (see -Files above), and the
# module needs the location for that, which it has.

# Staging reads no table; every other verb needs a valid one.
$table = if ($Verb -eq 'stage') { $null } elseif ($TablePath) { Import-SigningTable -Path $TablePath } else { Import-SigningTable }
if (($Verb -in 'tools', 'sign', 'export', 'import', 'verify') -and -not $table.Signers.ContainsKey($Signer)) {
    throw "no signer '$Signer' in the table (it has $(@($table.Signers.Keys | Sort-Object) -join ', '))"
}

switch ($Verb) {
    'plan' {
        $plan = Resolve-SigningPlan -Table $table -Configured $Configured -Builds $Builds -Build $Build
        $lines = @(
            "signer=$($plan.Signer)"
            "transport=$($plan.Transport)"
            "dry-run=$($plan.DryRun.ToString().ToLowerInvariant())"
        )
        $lines | Write-Host
        if ($env:GITHUB_OUTPUT) {
            [System.IO.File]::AppendAllText($env:GITHUB_OUTPUT, ($lines -join "`n") + "`n", [System.Text.UTF8Encoding]::new($false))
        }
    }
    'tools' {
        $row = $table.Signers[$Signer]
        $names = foreach ($stepName in 'PayloadStep', 'PackageStep') {
            if ($row.ContainsKey($stepName)) {
                $row[$stepName].Tool
                foreach ($tool in Get-ToolReference -Text $row[$stepName].Arguments) { $tool }
            }
        }
        foreach ($name in @($names | Sort-Object -Unique)) {
            $started = Test-SigningToolStart -Name $name -Table $table -ToolsDirectory $ToolsDirectory
            Write-Host "${name}: $($started.Path)$(if ($started.Output) { " ($($started.Output))" })"
        }
    }
    'sign' {
        $rows = @(Invoke-SignBatch -Root $Root -Kind $Kind -Files $Files -Signer $Signer -Table $table -WorkDirectory $WorkDirectory `
                -ToolsDirectory $ToolsDirectory -Manifest $Manifest -Trust $Trust -Thumbprint $Thumbprint)
        Write-Host "signed $($rows.Count) file(s) with $Signer"
    }
    'export' {
        $rows = @(Export-SigningBatch -Root $Root -Kind $Kind -Files $Files -Signer $Signer -Table $table -Destination $Destination -Manifest $Manifest)
        Write-Host "exported $($rows.Count) file(s) to $Destination"
    }
    'stage' {
        Invoke-Staging -Build $BuildDirectory -Destination $Destination
    }
    'dry-run' {
        Invoke-SigningDryRun -Root $Root -Packages $Packages -Table $table -WorkDirectory $WorkDirectory -ToolsDirectory $ToolsDirectory
    }
    'verify' {
        Test-ShippedArtifact -Signer $Signer -Table $table -Manifest $Manifest -Zip $Zip -Msi $Msi -WorkDirectory $WorkDirectory
    }
    'import' {
        $rows = @(Import-SigningBatch -Root $Root -From $From -Manifest $Manifest -Trust $Trust -Thumbprint $Thumbprint -Publisher $table.Signers[$Signer].Publisher)
        Write-Host "imported $($rows.Count) file(s) from $From"
    }
}
