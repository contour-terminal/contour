<#
.SYNOPSIS
    Windows code signing for the release packages.

.DESCRIPTION
    plan   Decides which signer a build uses and prints the decision; in a workflow it is also appended to
           $env:GITHUB_OUTPUT as signer=, transport= and dry-run=.

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
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('plan')][string]$Verb,
    [string]$Signer = '',
    [string]$Configured = '',
    [string]$Builds = '',
    [string]$TablePath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'windows-signing/WindowsSigning.psm1') -Force

switch ($Verb) {
    'plan' {
        $table = if ($TablePath) { Import-SigningTable -Path $TablePath } else { Import-SigningTable }
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
}
