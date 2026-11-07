<#
.SYNOPSIS
    The self-test of the Windows signing module: its pure decisions, offline.

.PARAMETER UnsignedPe
    A portable executable to copy into the fixtures (contour.exe).

.PARAMETER WorkDirectory
    Where the fixtures go; recreated on every run.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$UnsignedPe,
    [Parameter(Mandatory)][string]$WorkDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'WindowsSigning.psm1') -Force

if (Test-Path -LiteralPath $WorkDirectory) { Remove-Item -LiteralPath $WorkDirectory -Recurse -Force }
New-Item -ItemType Directory -Force -Path $WorkDirectory | Out-Null

$failures = [System.Collections.Generic.List[string]]::new()

function Test-Case {
    # Runs one named case; a throw is its failure, shown with the case's name.
    param([string]$Name, [scriptblock]$Body)
    try {
        & $Body
    } catch {
        $failures.Add("FAILED: ${Name}: $($_.Exception.Message)")
    }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$What)
    if (-not ($Expected -ceq $Actual)) { throw "$What is '$Actual', expected '$Expected'" }
}

function Assert-Throws {
    # The scriptblock throws, with a message that contains every needle.
    param([scriptblock]$Body, [string[]]$Needle, [string]$What)
    $message = $null
    try { & $Body } catch { $message = $_.Exception.Message }
    if ($null -eq $message) { throw "$What did not throw" }
    foreach ($text in $Needle) {
        if (-not $message.Contains($text)) { throw "$What threw '$message', which lacks '$text'" }
    }
}

$table = Import-PowerShellDataFile -LiteralPath (Join-Path $PSScriptRoot 'signers.psd1')

Test-Case 'shipped table' {
    # Only the two values of codesigntool that are pinned when its signer is added; every other row is valid.
    $problems = @(Test-SigningTable -Table $table)
    Assert-Equal 2 $problems.Count 'the shipped table has this many problems'
    foreach ($problem in $problems) {
        if (-not $problem.Contains("tool 'codesigntool'")) { throw "an unexpected problem: $problem" }
    }
}

# A valid table, and one breakage of each rule: each yields exactly one problem.
function New-ValidTable {
    return @{
        Tools   = @{ t = @{ Source = 'Path'; Executable = 't.exe' }
            g = @{ Source = 'GitHubRelease'; Repository = 'a/b'; Tag = 'v1'; Asset = 'x.zip'; Sha256 = ('ab' * 32); Executable = 'x.exe' } }
        Signers = @{ s = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @('releases')
                PayloadStep = @{ Tool = 't'; Invocation = 'InPlace'; Arguments = @('{file}') }
                PackageStep = @{ Tool = 't'; Invocation = 'InOut'; TimestampUrls = @('http://x/'); Arguments = @('{timestamp}', '{tool:g}') } } }
    }
}

Test-Case 'table rules' {
    Assert-Equal 0 @(Test-SigningTable -Table (New-ValidTable)).Count 'the valid table has this many problems'
    $broken = [ordered]@{
        'Transport'              = { param($t) $t.Signers.s.Transport = 'Carrier' }
        'Payload'                = { param($t) $t.Signers.s.Payload = 'Everything' }
        'AllowedBuilds'          = { param($t) $t.Signers.s.AllowedBuilds = @('releases', 'weekly') }
        'missing PayloadStep'    = { param($t) $t.Signers.s.Remove('PayloadStep') }
        'missing PackageStep'    = { param($t) $t.Signers.s.Remove('PackageStep') }
        'step Tool'              = { param($t) $t.Signers.s.PayloadStep.Tool = 'nosuch' }
        'Invocation'             = { param($t) $t.Signers.s.PayloadStep.Invocation = 'Sideways' }
        'timestamp without URLs' = { param($t) $t.Signers.s.PackageStep.Remove('TimestampUrls') }
        'tool placeholder'       = { param($t) $t.Signers.s.PackageStep.Arguments = @('{timestamp}', '{tool:nosuch}') }
        'tool Source'            = { param($t) $t.Tools.t.Source = 'Registry' }
        'release Repository'     = { param($t) $t.Tools.g.Remove('Repository') }
        'release Tag'            = { param($t) $t.Tools.g.Remove('Tag') }
        'release Asset'          = { param($t) $t.Tools.g.Remove('Asset') }
        'release Sha256'         = { param($t) $t.Tools.g.Sha256 = 'cafe' }
        'release Executable'     = { param($t) $t.Tools.g.Remove('Executable') }
    }
    foreach ($rule in $broken.Keys) {
        $candidate = New-ValidTable
        & $broken[$rule] $candidate
        $problems = @(Test-SigningTable -Table $candidate)
        if ($problems.Count -ne 1) { throw "breaking '$rule' gives $($problems.Count) problems: $($problems -join ' | ')" }
    }
}

Test-Case 'plan none' {
    $plan = Resolve-SigningPlan -Table $table -Signer '' -Configured '' -Builds ''
    Assert-Equal 'none' $plan.Signer 'the signer'
    Assert-Equal 'none' $plan.Transport 'the transport'
    Assert-Equal $true $plan.DryRun 'DryRun'
}

Test-Case 'plan unsigned master build' {
    $plan = Resolve-SigningPlan -Table $table -Signer 'none' -Configured 'certum' -Builds 'releases'
    Assert-Equal 'none' $plan.Transport 'the transport'
    Assert-Equal $true $plan.DryRun 'DryRun'
}

Test-Case 'plan certum release' {
    $plan = Resolve-SigningPlan -Table $table -Signer 'certum' -Configured 'certum' -Builds 'releases'
    Assert-Equal 'certum' $plan.Signer 'the signer'
    Assert-Equal 'command' $plan.Transport 'the transport'
    Assert-Equal $false $plan.DryRun 'DryRun'
}

Test-Case 'plan rejects' {
    Assert-Throws { Resolve-SigningPlan -Table $table -Signer 'signpath' -Configured 'signpath' -Builds 'releases-and-master' } @('AllowedBuilds') 'signpath on master builds'
    Assert-Throws { Resolve-SigningPlan -Table $table -Signer 'signtool' -Configured 'signtool' -Builds 'releases' } @('local signing only') 'signtool'
    Assert-Throws { Resolve-SigningPlan -Table $table -Signer '' -Configured 'nosuch' -Builds 'releases' } @('nosuch') 'an unknown signer'
    Assert-Throws { Resolve-SigningPlan -Table $table -Signer '' -Configured 'certum' -Builds 'weekly' } @('weekly') 'an unknown builds setting'
    Assert-Throws { Resolve-SigningPlan -Table $table -Signer 'certum' -Configured 'sslcom' -Builds 'releases' } @('certum', 'sslcom') 'a signer other than the configured one'
}

Test-Case 'expand' {
    $values = @{ inputDir = 'C:\a b'; file = 'x.exe' }
    $expanded = @(Expand-SigningArgument -Template @('-input_dir_path={inputDir}', '{file}') -Values $values)
    Assert-Equal 2 $expanded.Count 'the number of arguments'
    Assert-Equal '-input_dir_path=C:\a b' $expanded[0] 'the first argument'
    Assert-Equal 'x.exe' $expanded[1] 'the second argument'

    $env:CONTOUR_TEST_VAR = 'from-the-environment'
    try {
        Assert-Equal 'p=from-the-environment' @(Expand-SigningArgument -Template @('p={env:CONTOUR_TEST_VAR}') -Values @{})[0] 'an {env:} argument'
        $env:CONTOUR_TEST_OTHER = 'other-secret-value'
        $env:CONTOUR_TEST_EMPTY = ''
        $message = $null
        try { Expand-SigningArgument -Template @('{env:CONTOUR_TEST_OTHER}', '{env:CONTOUR_TEST_EMPTY}') -Values @{} | Out-Null } catch { $message = $_.Exception.Message }
        if ($null -eq $message) { throw 'an empty {env:X} did not throw' }
        if (-not $message.Contains('CONTOUR_TEST_EMPTY')) { throw "the message '$message' does not name the variable" }
        if ($message.Contains('other-secret-value')) { throw 'the message holds the value of another variable' }
        Assert-Throws { Expand-SigningArgument -Template @('{env:CONTOUR_TEST_UNSET_VARIABLE}') -Values @{} } @('CONTOUR_TEST_UNSET_VARIABLE') 'an unset {env:X}'
    } finally {
        Remove-Item Env:CONTOUR_TEST_VAR, Env:CONTOUR_TEST_OTHER, Env:CONTOUR_TEST_EMPTY -ErrorAction SilentlyContinue
    }
    Assert-Throws { Expand-SigningArgument -Template @('{nosuch}') -Values @{} } @('nosuch') 'an unknown placeholder'
}

Test-Case 'manifest' {
    $fixture = Join-Path $WorkDirectory 'signing fixture'
    $path = Join-Path $fixture 'manifest.txt'
    $rows = @(
        [pscustomobject]@{ RelativePath = 'bin/contour.exe'; SignedPath = 'C:\out dir\contour.exe'; Before = ('AB' * 32); After = ('cd' * 32) }
        [pscustomobject]@{ RelativePath = 'bin/Qt6Core.dll'; SignedPath = 'C:/out dir/Qt6Core.dll'; Before = ('01' * 32); After = ('EF' * 32) }
    )
    Write-SigningManifest -Path $path -Rows $rows
    $read = @(Read-SigningManifest -Path $path)
    Assert-Equal 2 $read.Count 'the number of rows'
    for ($i = 0; $i -lt 2; $i++) {
        Assert-Equal $rows[$i].RelativePath $read[$i].RelativePath "row $i RelativePath"
        Assert-Equal ($rows[$i].SignedPath.Replace('\', '/')) $read[$i].SignedPath "row $i SignedPath"
        Assert-Equal $rows[$i].Before.ToLowerInvariant() $read[$i].Before "row $i Before"
        Assert-Equal $rows[$i].After.ToLowerInvariant() $read[$i].After "row $i After"
    }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB) { throw 'the manifest starts with a byte order mark' }
    if ([System.Text.Encoding]::UTF8.GetString($bytes).Contains("`r")) { throw 'the manifest has a CR' }

    $bad = Join-Path $fixture 'bad.txt'
    [System.IO.File]::WriteAllText($bad, "# header`nbin/a.exe`tC:/a.exe`t$('00' * 32)`n")
    Assert-Throws { Read-SigningManifest -Path $bad } @('line 2') 'a row of three columns'
}

Test-Case 'pe detection' {
    $fixture = Join-Path $WorkDirectory 'pe fixture'
    New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'sub dir') | Out-Null
    Copy-Item -LiteralPath $UnsignedPe -Destination (Join-Path $fixture 'sub dir/copy.exe')
    [System.IO.File]::WriteAllText((Join-Path $fixture 'fake.dll'), 'this is text, not a library')
    # MZ, an e_lfanew that points at something other than PE\0\0.
    $headerOnly = [byte[]]::new(0x100)
    $headerOnly[0] = 0x4D; $headerOnly[1] = 0x5A; $headerOnly[0x3C] = 0x80
    [System.IO.File]::WriteAllBytes((Join-Path $fixture 'almost.exe'), $headerOnly)
    $found = @(Get-PortableExecutable -Root $fixture)
    Assert-Equal 1 $found.Count "the number of executables ($($found -join ', '))"
    Assert-Equal 'sub dir/copy.exe' $found[0] 'the executable'
}

Test-Case 'plan verb' {
    # The command line as a workflow runs it: the decision on the screen and in $GITHUB_OUTPUT. Run against the shipped
    # table with its two unpinned values filled in, as the shipped table is not valid yet.
    $text = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'signers.psd1'))
    $text = $text.Replace('<pinned in Task 4>', ('ab' * 32)).Replace('<path inside the archive, Task 4>', 'CodeSignTool.bat')
    $tablePath = Join-Path $WorkDirectory 'valid signers.psd1'
    [System.IO.File]::WriteAllText($tablePath, $text)
    $output = Join-Path $WorkDirectory 'github output.txt'
    $pwsh = (Get-Process -Id $PID).Path
    $script = Join-Path $PSScriptRoot '../sign-windows.ps1'
    $env:GITHUB_OUTPUT = $output
    try {
        $shown = & $pwsh -NoProfile -File $script plan -Signer certum -Configured certum -Builds releases -TablePath $tablePath
        if ($LASTEXITCODE -ne 0) { throw "plan exited $LASTEXITCODE" }
        Assert-Equal "signer=certum`ntransport=command`ndry-run=false`n" ([System.IO.File]::ReadAllText($output)) 'the lines appended to GITHUB_OUTPUT'
        Assert-Equal 3 @($shown).Count 'the number of lines printed'
        $null = & $pwsh -NoProfile -File $script plan -Signer none -Configured certum -Builds releases -TablePath $tablePath
        Assert-Equal "signer=certum`ntransport=command`ndry-run=false`nsigner=none`ntransport=none`ndry-run=true`n" ([System.IO.File]::ReadAllText($output)) 'the lines after a second plan'
        $null = & $pwsh -NoProfile -File $script plan -Configured signtool -TablePath $tablePath 2>&1
        if ($LASTEXITCODE -eq 0) { throw 'plan accepted a signer that is for local signing only' }
    } finally {
        Remove-Item Env:GITHUB_OUTPUT -ErrorAction SilentlyContinue
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host 'windows-signing: OK'
