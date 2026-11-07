<#
.SYNOPSIS
    Windows code signing for the release packages.

.DESCRIPTION
    Usage:
      sign-windows.ps1 plan   -Signer <effective> -Configured <name> -Builds <setting>
      sign-windows.ps1 tools  -Signer <name>
      sign-windows.ps1 sign   -Kind Payload|Packages -Root <dir> [-Files <a>,<b>] -Signer <name> [-Manifest <tsv>]
      sign-windows.ps1 export -Kind Payload|Packages -Root <dir> [-Files <a>,<b>] -Signer <name> -Destination <dir> -Manifest <tsv>
      sign-windows.ps1 import -Root <dir> -From <dir> -Manifest <tsv> [-Trust System|Pinned] [-Thumbprint <sha1>]

    plan    Decides which signer a build uses and prints the decision; in a workflow it is also appended to
            $env:GITHUB_OUTPUT as signer=, transport= and dry-run=.
    tools   Resolves and prints every tool the signer's row names, downloading the pinned ones: a probe.
    sign    Signs the batch with the signer and checks the result.
    export  Copies the files a signer that signs elsewhere (SignPath) would sign out of the tree.
    import  Puts the files signed elsewhere back into the tree, after checking them.

    Common: -WorkDirectory (default out/signing/work), -ToolsDirectory (default out/signing/tools).

.PARAMETER Verb
    What to do: plan.

.PARAMETER Signer
    The effective signer: the one this build signs with. Empty means none.

.PARAMETER Configured
    The signer the repository is configured for. Empty means none.

.PARAMETER Builds
    Which builds sign: releases or releases-and-master. Empty means releases.

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
    Where export puts the copies.

.PARAMETER From
    Where import finds the signed copies.

.PARAMETER Trust
    System (Windows must trust the signature) or Pinned (a throwaway certificate, by Thumbprint).

.PARAMETER Thumbprint
    For Pinned, the signer certificate's thumbprint.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('plan', 'tools', 'sign', 'export', 'import')][string]$Verb,
    [string]$Signer = '',
    [string]$Configured = '',
    [string]$Builds = '',
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
    [string]$Thumbprint = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'windows-signing/WindowsSigning.psm1') -Force

$table = if ($TablePath) { Import-SigningTable -Path $TablePath } else { Import-SigningTable }

switch ($Verb) {
    'plan' {
        $plan = Resolve-SigningPlan -Table $table -Signer $Signer -Configured $Configured -Builds $Builds
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
        if (-not $row) { throw "no signer '$Signer' in the table" }
        $names = foreach ($stepName in 'PayloadStep', 'PackageStep') {
            if ($row.ContainsKey($stepName)) {
                $row[$stepName].Tool
                foreach ($argument in $row[$stepName].Arguments) {
                    foreach ($match in [regex]::Matches($argument, '\{tool:([^{}]+)\}')) { $match.Groups[1].Value }
                }
            }
        }
        foreach ($name in @($names | Sort-Object -Unique)) {
            Write-Host "${name}: $(Resolve-SigningTool -Name $name -Table $table -ToolsDirectory $ToolsDirectory)"
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
    'import' {
        $rows = @(Import-SigningBatch -Root $Root -From $From -Manifest $Manifest -Trust $Trust -Thumbprint $Thumbprint)
        Write-Host "imported $($rows.Count) file(s) from $From"
    }
}
