<#
.SYNOPSIS
    The self-test of the Windows signing module: its decisions, a throwaway-certificate signing run, the command line
    and, where a timestamp server answers, the cases that need one.

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
$UnsignedPe = (Resolve-Path -LiteralPath $UnsignedPe).Path
$WorkDirectory = (Resolve-Path -LiteralPath $WorkDirectory).Path

$failures = [System.Collections.Generic.List[string]]::new()
$script:SkippedOnline = 0

# The private keys signtool leaves in the user's profile when it signs with a .pfx: none may be left by this run.
function Get-KeyFileCount {
    $crypto = Join-Path $env:APPDATA 'Microsoft/Crypto'
    return @(Get-ChildItem -LiteralPath $crypto -Recurse -File -Force -ErrorAction SilentlyContinue).Count
}
$keyFilesAtStart = Get-KeyFileCount

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
$pwsh = (Get-Process -Id $PID).Path
$cliScript = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../sign-windows.ps1')).Path

function Invoke-Cli {
    # The command line as a workflow runs it: a process of its own. Returns ExitCode and Output (both streams).
    $output = & $pwsh -NoProfile -File $cliScript @args 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

Test-Case 'shipped table' {
    $problems = @(Test-SigningTable -Table $table)
    if ($problems.Count -gt 0) { throw "the shipped table has problems: $($problems -join ' | ')" }
    # Every Certum and SSL.com tool is pinned to a real digest, not to a placeholder.
    foreach ($name in 'ssign', 'ssignPkcs11', 'osslsigncode', 'libp11', 'codesigntool') {
        if ($table.Tools[$name].Sha256 -notmatch '^[0-9a-f]{64}$') { throw "tool $name is not pinned to a lowercase SHA-256" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'certum-code-signing-2021-ca.pem'))) { throw 'the Certum intermediate is not vendored' }
}

# A valid table, and one breakage of each rule: each yields exactly one problem.
function New-ValidTable {
    return @{
        Attempts          = 3
        RetryDelaySeconds = @(0, 0)
        OwnBinaries       = @('bin/contour.exe')
        Product           = @{ Name = 'Contour'; Url = 'https://example.org/' }
        Tools             = @{ t = @{ Source = 'Path'; Executable = 't.exe'; ProbeArguments = @('--version') }
            g = @{ Source = 'GitHubRelease'; Repository = 'a/b'; Tag = 'v1'; Asset = 'x.zip'; Sha256 = ('ab' * 32); Executable = 'x.exe'; ProbeArguments = @('--version') } }
        Signers           = @{
            none = @{ Transport = 'None'; Payload = 'Unsigned'; AllowedBuilds = @('releases'); Publisher = '' }
            s    = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @('releases'); Publisher = 'CN=Somebody*'; Secrets = @('SOME_SECRET')
                PayloadStep = @{ Tool = 't'; Invocation = 'InPlace'; Timestamp = 'Tool'; Arguments = @('{file}', '{env:SOME_SECRET}') }
                PackageStep = @{ Tool = 't'; Invocation = 'InOut'; TimestampUrls = @('http://x/'); Arguments = @('{timestamp}', '{tool:g}', '{in}', '{out}') } }
        }
    }
}

Test-Case 'table rules' {
    Assert-Equal 0 @(Test-SigningTable -Table (New-ValidTable)).Count 'the valid table has this many problems'
    $broken = [ordered]@{
        'Transport'                    = { param($t) $t.Signers.s.Transport = 'Carrier' }
        'Payload'                      = { param($t) $t.Signers.s.Payload = 'Everything' }
        'AllowedBuilds'                = { param($t) $t.Signers.s.AllowedBuilds = @('releases', 'weekly') }
        'missing AllowedBuilds'        = { param($t) $t.Signers.s.Remove('AllowedBuilds') }
        'missing Publisher'            = { param($t) $t.Signers.s.Remove('Publisher') }
        'empty Publisher'              = { param($t) $t.Signers.s.Publisher = '' }
        'missing none signer'          = { param($t) $t.Signers.Remove('none') }
        'missing PayloadStep'          = { param($t) $t.Signers.s.Remove('PayloadStep') }
        'missing PackageStep'          = { param($t) $t.Signers.s.Remove('PackageStep') }
        'step Tool'                    = { param($t) $t.Signers.s.PayloadStep.Tool = 'nosuch' }
        'Invocation'                   = { param($t) $t.Signers.s.PayloadStep.Invocation = 'Sideways' }
        'missing Arguments'            = { param($t) $t.Signers.s.PayloadStep.Remove('Arguments') }
        'timestamp without URLs'       = { param($t) $t.Signers.s.PackageStep.Remove('TimestampUrls') }
        'no timestamp declared'        = { param($t) $t.Signers.s.PayloadStep.Remove('Timestamp') }
        'legacy timestamp option'      = { param($t) $t.Signers.s.PackageStep.Arguments = @('{timestamp}', '/t') }
        'tool placeholder'             = { param($t) $t.Signers.s.PackageStep.Arguments = @('{timestamp}', '{tool:nosuch}') }
        'toolDir placeholder'          = { param($t) $t.Signers.s.PackageStep.Arguments = @('{timestamp}', '{toolDir:nosuch}') }
        'env not among Secrets'        = { param($t) $t.Signers.s.PayloadStep.Arguments = @('{file}', '{env:OTHER_SECRET}') }
        'placeholder of another shape' = { param($t) $t.Signers.s.PayloadStep.Arguments = @('{in}') }
        'unknown placeholder'          = { param($t) $t.Signers.s.PayloadStep.Arguments = @('{file}', '{bogus}') }
        'data file'                    = { param($t) $t.Signers.s.PayloadStep.Arguments = @('{file}', '{data:no-such-file.pem}') }
        'tool Source'                  = { param($t) $t.Tools.t.Source = 'Registry' }
        'batch file executable'        = { param($t) $t.Tools.t.Executable = 't.bat' }
        'missing probe'                = { param($t) $t.Tools.t.Remove('ProbeArguments') }
        'probe placeholder'            = { param($t) $t.Tools.t.ProbeArguments = @('{nosuch}') }
        'tool Environment'             = { param($t) $t.Tools.t.Environment = @{ X = 5 } }
        'tool Cleanup'                 = { param($t) $t.Tools.t.Cleanup = @(5) }
        'release Repository'           = { param($t) $t.Tools.g.Remove('Repository') }
        'release Tag'                  = { param($t) $t.Tools.g.Remove('Tag') }
        'release Asset'                = { param($t) $t.Tools.g.Remove('Asset') }
        'release Sha256'               = { param($t) $t.Tools.g.Sha256 = 'cafe' }
        'release Executable'           = { param($t) $t.Tools.g.Remove('Executable') }
        'Attempts zero'                = { param($t) $t.Attempts = 0 }
        'missing Attempts'             = { param($t) $t.Remove('Attempts') }
        'missing RetryDelaySeconds'    = { param($t) $t.Remove('RetryDelaySeconds') }
        'missing OwnBinaries'          = { param($t) $t.Remove('OwnBinaries') }
        'missing Product name'         = { param($t) $t.Product.Remove('Name') }
    }
    foreach ($rule in $broken.Keys) {
        $candidate = New-ValidTable
        & $broken[$rule] $candidate
        $problems = @(Test-SigningTable -Table $candidate)
        if ($problems.Count -ne 1) { throw "breaking '$rule' gives $($problems.Count) problems: $($problems -join ' | ')" }
    }
}

Test-Case 'plan' {
    # (configured, builds, kind of build) -> (signer, transport, dry run)
    $cases = @(
        , @('', '', 'release', 'none', 'none', $true)
        , @('', '', 'other', 'none', 'none', $true)
        , @('certum', 'releases', 'release', 'certum', 'command', $false)
        , @('certum', 'releases', 'master', 'none', 'none', $true)
        , @('certum', 'releases-and-master', 'master', 'certum', 'command', $false)
        , @('certum', 'releases-and-master', 'other', 'none', 'none', $true)
        , @('certum', '', 'release', 'certum', 'command', $false)
        , @('signpath', 'releases', 'release', 'signpath', 'signpath', $false)
        , @('signpath', 'releases', 'master', 'none', 'none', $true)
    )
    foreach ($case in $cases) {
        $label = "$($case[0]) / $($case[1]) / $($case[2])"
        $plan = Resolve-SigningPlan -Table $table -Configured $case[0] -Builds $case[1] -Build $case[2]
        Assert-Equal $case[3] $plan.Signer "the signer of $label"
        Assert-Equal $case[4] $plan.Transport "the transport of $label"
        Assert-Equal $case[5] $plan.DryRun "DryRun of $label"
    }
}

Test-Case 'plan rejects' {
    Assert-Throws { Resolve-SigningPlan -Table $table -Configured 'signpath' -Builds 'releases-and-master' -Build 'master' } @('AllowedBuilds') 'signpath on master builds'
    Assert-Throws { Resolve-SigningPlan -Table $table -Configured 'signpath' -Builds 'releases-and-master' -Build 'release' } @('AllowedBuilds') 'signpath configured for master builds, on a release'
    Assert-Throws { Resolve-SigningPlan -Table $table -Configured 'signtool' -Builds 'releases' -Build 'release' } @('local signing only') 'signtool'
    Assert-Throws { Resolve-SigningPlan -Table $table -Configured 'nosuch' -Builds 'releases' -Build 'release' } @('nosuch') 'an unknown signer'
    Assert-Throws { Resolve-SigningPlan -Table $table -Configured 'certum' -Builds 'weekly' -Build 'release' } @('weekly') 'an unknown builds setting'
    Assert-Throws { Resolve-SigningPlan -Table $table -Configured 'certum' -Builds 'releases' -Build 'nightly' } @('nightly') 'an unknown kind of build'
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
    # The command line as a workflow runs it: the decision on the screen and in $GITHUB_OUTPUT.
    $tablePath = Join-Path $PSScriptRoot 'signers.psd1'
    $output = Join-Path $WorkDirectory 'github output.txt'
    $env:GITHUB_OUTPUT = $output
    try {
        $shown = Invoke-Cli plan -Configured certum -Builds releases -Build release -TablePath $tablePath
        if ($shown.ExitCode -ne 0) { throw "plan exited $($shown.ExitCode): $($shown.Output)" }
        Assert-Equal "signer=certum`ntransport=command`ndry-run=false`n" ([System.IO.File]::ReadAllText($output)) 'the lines appended to GITHUB_OUTPUT'
        Assert-Equal 3 @($shown.Output.Trim() -split "`r?`n").Count 'the number of lines printed'
        $null = Invoke-Cli plan -Configured certum -Builds releases -Build master -TablePath $tablePath
        Assert-Equal "signer=certum`ntransport=command`ndry-run=false`nsigner=none`ntransport=none`ndry-run=true`n" ([System.IO.File]::ReadAllText($output)) 'the lines after a second plan'
        $local = Invoke-Cli plan -Configured signtool -Builds releases -Build release -TablePath $tablePath
        if ($local.ExitCode -eq 0) { throw 'plan accepted a signer that is for local signing only' }
    } finally {
        Remove-Item Env:GITHUB_OUTPUT -ErrorAction SilentlyContinue
    }
}

# ---- Signing and verifying, with a throwaway certificate -------------------------------------------------------------

$toolsDirectory = Join-Path $WorkDirectory 'tools'
$fixtureRoot = Join-Path $WorkDirectory 'signing fixture'
$script:Certificates = [System.Collections.Generic.List[object]]::new()

function Get-TestTable {
    # The shipped table, fresh, with no waiting between attempts and a PowerShell tool for the cases that need a
    # program that does something simple.
    $fresh = Import-PowerShellDataFile -LiteralPath (Join-Path $PSScriptRoot 'signers.psd1')
    $fresh.RetryDelaySeconds = @(0, 0)
    $fresh.Tools.pwsh = @{ Source = 'Path'; Executable = 'pwsh'; ProbeArguments = @('-NoProfile', '-Command', 'exit 0') }
    return $fresh
}

function New-FixtureTree {
    # bin/contour.exe and bin/plugins/second.dll (copies of the unsigned executable) and bin/notes.txt.
    param([string]$Name)
    $tree = Join-Path $fixtureRoot $Name
    New-Item -ItemType Directory -Force -Path (Join-Path $tree 'bin/plugins') | Out-Null
    Copy-Item -LiteralPath $UnsignedPe -Destination (Join-Path $tree 'bin/contour.exe')
    Copy-Item -LiteralPath $UnsignedPe -Destination (Join-Path $tree 'bin/plugins/second.dll')
    [System.IO.File]::WriteAllText((Join-Path $tree 'bin/notes.txt'), 'not an executable')
    return $tree
}

function Use-Certificate {
    # Makes a throwaway certificate and points the dry-run row's two variables at it; Clear-Certificate deletes its key.
    param([string]$Name)
    $certificate = New-ThrowawayCodeSigningCertificate -Directory (Join-Path $WorkDirectory "cert-$Name")
    $script:Certificates.Add($certificate)
    $env:CONTOUR_SIGNING_CERT = $certificate.CertificatePath
    $env:CONTOUR_SIGNING_KEY = $certificate.KeyName
    return $certificate
}

function Clear-Certificate {
    Remove-Item Env:CONTOUR_SIGNING_CERT, Env:CONTOUR_SIGNING_KEY -ErrorAction SilentlyContinue
    foreach ($certificate in $script:Certificates) { Remove-ThrowawayCodeSigningCertificate -Certificate $certificate }
    $script:Certificates.Clear()
}

function Test-OnlineCase {
    # A case that needs a timestamp server: mandatory on CI, elsewhere skipped, with a notice, where none answers.
    param([string]$Name, [scriptblock]$Body)
    if ($env:CI -ne 'true') {
        try {
            $null = Invoke-WebRequest -Uri 'http://timestamp.digicert.com' -Method Head -TimeoutSec 10 -SkipHttpErrorCheck
        } catch {
            Write-Host "skipped (no timestamp server reachable): $Name"
            $script:SkippedOnline++
            return
        }
    }
    Test-Case $Name $Body
}

function Invoke-ThrowawaySigntool {
    # Signs a file with the throwaway certificate of Use-Certificate, without a timestamp: offline. The key sits in a named
    # container, so no key file stays behind.
    param([string]$Path)
    $signtool = Resolve-SigningTool -Name signtool -Table (Get-TestTable) -ToolsDirectory $toolsDirectory
    & $signtool sign /fd sha256 /f $env:CONTOUR_SIGNING_CERT /csp 'Microsoft Software Key Storage Provider' /kc $env:CONTOUR_SIGNING_KEY $Path | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool exited $LASTEXITCODE for $Path" }
}

function Get-SignedSingleFile {
    # Signs a copy of the executable with the throwaway certificate; returns its path.
    param([string]$Name)
    $path = Join-Path $WorkDirectory "single-$Name.exe"
    Copy-Item -LiteralPath $UnsignedPe -Destination $path -Force
    Invoke-ThrowawaySigntool -Path $path
    return $path
}

Test-Case 'throwaway certificate' {
    $before = @(Get-ChildItem Cert:\CurrentUser\My).Count
    $files = Get-KeyFileCount
    $certificate = New-ThrowawayCodeSigningCertificate -Directory (Join-Path $WorkDirectory 'cert-plain')
    try {
        if (-not (Test-Path -LiteralPath $certificate.CertificatePath)) { throw 'no certificate file was written' }
        if ($certificate.Thumbprint -notmatch '^[0-9A-F]{40}$') { throw "the thumbprint is '$($certificate.Thumbprint)'" }
        Assert-Equal $before @(Get-ChildItem Cert:\CurrentUser\My).Count 'the number of certificates in the current user''s store'
        $loaded = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificate.CertificatePath)
        if ($loaded.Subject -ne 'CN=Contour signing dry run (throwaway)') { throw "the subject is '$($loaded.Subject)'" }
        if ($loaded.HasPrivateKey) { throw 'the certificate file holds the private key' }
        if ($loaded.NotAfter -gt [DateTime]::UtcNow.AddDays(1.1)) { throw 'the certificate lives longer than a day' }
    } finally {
        Remove-ThrowawayCodeSigningCertificate -Certificate $certificate
    }
    if (Test-Path -LiteralPath $certificate.CertificatePath) { throw 'the certificate file is still there' }
    Assert-Equal $files (Get-KeyFileCount) 'the number of key files after the key was deleted'
}

Test-Case 'unsigned file' {
    $tree = New-FixtureTree 'unsigned'
    $problem = Get-SignatureProblem -Path (Join-Path $tree 'bin/contour.exe') -Trust System
    if ($problem -notlike '*NotSigned*') { throw "the problem is '$problem'" }
}

Test-Case 'publisher' {
    # A program Microsoft signed: valid under System trust, and a publisher is matched against the certificate subject.
    $signtool = Resolve-SigningTool -Name signtool -Table (Get-TestTable) -ToolsDirectory $toolsDirectory
    $problem = Get-SignatureProblem -Path $signtool -Trust System -Publisher '*Microsoft*'
    if ($null -ne $problem) { throw "a Microsoft program is not Microsoft's: $problem" }
    $problem = Get-SignatureProblem -Path $signtool -Trust System -Publisher '*Somebody Else*'
    if ($problem -notlike '*not by a publisher matching*') { throw "the problem of another publisher is '$problem'" }
}

Test-Case 'missing secret' {
    $table = Get-TestTable
    $table.Signers.dryrun.Secrets = @('CONTOUR_TEST_UNSET', 'CONTOUR_TEST_UNSET_TOO')
    $table.Signers.dryrun.PayloadStep.Tool = 'nosuchtool'
    $table.Tools.nosuchtool = @{ Source = 'Path'; Executable = 'nosuchtool-that-is-not-there' }
    $tree = New-FixtureTree 'secret'
    Assert-Throws { Invoke-SignBatch -Root $tree -Kind Payload -Signer dryrun -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-secret') -ToolsDirectory $toolsDirectory } @('CONTOUR_TEST_UNSET', 'CONTOUR_TEST_UNSET_TOO') 'a missing secret'
}

Test-Case 'unknown signer' {
    $tree = New-FixtureTree 'unknown-signer'
    $table = Get-TestTable
    Assert-Throws { Invoke-SignBatch -Root $tree -Kind Payload -Signer nosuch -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-unknown') -ToolsDirectory $toolsDirectory } @("no signer 'nosuch'") 'sign with an unknown signer'
    Assert-Throws { Export-SigningBatch -Root $tree -Kind Payload -Signer nosuch -Table $table -Destination (Join-Path $WorkDirectory 'unknown-out') -Manifest (Join-Path $WorkDirectory 'unknown.tsv') } @("no signer 'nosuch'") 'export with an unknown signer'
    Assert-Throws { Test-ShippedArtifact -Signer nosuch -Table $table -Manifest (Join-Path $WorkDirectory 'unknown.tsv') -Zip 'x.zip' -Msi @('x.msi') -WorkDirectory (Join-Path $WorkDirectory 'unknown-verify') } @("no signer 'nosuch'") 'verify with an unknown signer'
}

Test-Case 'failing tool' {
    $table = Get-TestTable
    $step = @{ Tool = 'pwsh'; Invocation = 'InPlace'; Arguments = @('-NoProfile', '-Command', 'exit 3') }
    $tree = New-FixtureTree 'failing'
    Assert-Throws { Invoke-SigningStep -Root $tree -Files @('bin/contour.exe') -Step $step -Table $table -ToolsDirectory $toolsDirectory -WorkDirectory (Join-Path $WorkDirectory 'work-failing') } @('pwsh', 'exit code 3', '3 attempts') 'a failing tool'
}

Test-Case 'data and product placeholders' {
    $table = Get-TestTable
    $tree = New-FixtureTree 'placeholders'
    $step = @{ Tool = 'pwsh'; Invocation = 'InPlace'; Arguments = @('-NoProfile', '-Command',
            "exit [int](-not ((Test-Path -LiteralPath '{data:certum-code-signing-2021-ca.pem}') -and '{product:name}' -eq 'Contour' -and '{product:url}' -eq 'https://github.com/contour-terminal/contour/'))") }
    Invoke-SigningStep -Root $tree -Files @('bin/contour.exe') -Step $step -Table $table -ToolsDirectory $toolsDirectory -WorkDirectory (Join-Path $WorkDirectory 'work-placeholders')
}

Test-Case 'tool environment and cleanup' {
    $table = Get-TestTable
    $leftover = Join-Path $WorkDirectory 'tool cleanup'
    New-Item -ItemType Directory -Force -Path (Join-Path $leftover 'session') | Out-Null
    $env:CONTOUR_TEST_CLEAN = $leftover
    try {
        $table.Tools.fake = @{ Source = 'Path'; Executable = 'pwsh'; ProbeArguments = @('-NoProfile', '-Command', 'exit 0')
            Environment = @{ CONTOUR_TEST_TOOLENV = '{toolDir}' }; Cleanup = @('{env:CONTOUR_TEST_CLEAN}/session', '{env:CONTOUR_TEST_UNSET_FOR_CLEANUP}/session') }
        $tree = New-FixtureTree 'tool-env'
        # The tool sees its environment ...
        $step = @{ Tool = 'fake'; Invocation = 'InPlace'; Arguments = @('-NoProfile', '-Command', 'exit [int]([string]::IsNullOrEmpty($env:CONTOUR_TEST_TOOLENV))') }
        Invoke-SigningStep -Root $tree -Files @('bin/contour.exe') -Step $step -Table $table -ToolsDirectory $toolsDirectory -WorkDirectory (Join-Path $WorkDirectory 'work-tool-env')
        # ... for the call only, and what it left behind is gone, an unset variable in a path skipping that path.
        if ($env:CONTOUR_TEST_TOOLENV) { throw 'the tool''s environment is still set' }
        if (Test-Path -LiteralPath (Join-Path $leftover 'session')) { throw 'the tool''s leftover is still there' }
    } finally { Remove-Item Env:CONTOUR_TEST_CLEAN -ErrorAction SilentlyContinue }
}

Test-Case 'all timestamp servers down' {
    $certificate = Use-Certificate 'down'
    try {
        $table = Get-TestTable
        $table.Attempts = 2
        $table.Signers.dryrun.PayloadStep.TimestampUrls = @('http://127.0.0.1:9/', 'http://127.0.0.1:10/')
        $tree = New-FixtureTree 'down'
        Assert-Throws { Invoke-SigningStep -Root $tree -Files @('bin/contour.exe') -Step $table.Signers.dryrun.PayloadStep -Table $table -ToolsDirectory $toolsDirectory -WorkDirectory (Join-Path $WorkDirectory 'work-down') } @('http://127.0.0.1:9/', 'http://127.0.0.1:10/') 'servers that are down'
    } finally { Clear-Certificate }
}

Test-Case 'untimestamped signature' {
    $certificate = Use-Certificate 'untimestamped'
    try {
        $table = Get-TestTable
        $table.Signers.dryrun.PayloadStep.Arguments = @('sign', '/fd', 'sha256', '/f', '{env:CONTOUR_SIGNING_CERT}', '/csp', 'Microsoft Software Key Storage Provider', '/kc', '{env:CONTOUR_SIGNING_KEY}', '{file}')
        $table.Signers.dryrun.PayloadStep.Remove('TimestampUrls')
        $tree = New-FixtureTree 'untimestamped'
        Assert-Throws { Invoke-SignBatch -Root $tree -Kind Payload -Signer dryrun -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-untimestamped') -ToolsDirectory $toolsDirectory -Trust Pinned -Thumbprint $certificate.Thumbprint } @('not timestamped') 'a signature without a timestamp'
    } finally { Clear-Certificate }
}

Test-Case 'selection' {
    $table = Get-TestTable
    $tree = New-FixtureTree 'selection'
    $selected = @(Select-SigningFile -Root $tree -Policy Unsigned -OwnBinaries @('bin/contour.exe'))
    Assert-Equal 'bin/contour.exe|bin/plugins/second.dll' ($selected -join '|') 'the unsigned selection'
    Assert-Equal 'bin/contour.exe' (@(Select-SigningFile -Root $tree -Policy Own -OwnBinaries @('bin/contour.exe')) -join '|') 'the own selection'
    Assert-Throws { Select-SigningFile -Root $tree -Policy Own -OwnBinaries @('bin/missing.exe') } @('bin/missing.exe', 'is this a staged install tree?') 'a missing own binary'

    # A Microsoft-signed program is skipped; a tree of nothing else has nothing to sign.
    $signtool = Resolve-SigningTool -Name signtool -Table $table -ToolsDirectory $toolsDirectory
    $signedOnly = Join-Path $WorkDirectory 'only signed'
    New-Item -ItemType Directory -Force -Path $signedOnly | Out-Null
    Copy-Item -LiteralPath $signtool -Destination (Join-Path $signedOnly 'signtool.exe')
    Assert-Throws { Select-SigningFile -Root $signedOnly -Policy Unsigned -OwnBinaries @() } @('stage afresh') 'a tree of signed files'
    Copy-Item -LiteralPath $signtool -Destination (Join-Path $tree 'bin/signtool.exe')
    $withSigned = @(Select-SigningFile -Root $tree -Policy Unsigned -OwnBinaries @('bin/contour.exe'))
    if ('bin/signtool.exe' -in $withSigned) { throw 'a file with a valid signature was selected' }

    # `none` selects the same files, and signs none of them.
    $rows = @(Invoke-SignBatch -Root $tree -Kind Payload -Signer none -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-none') -ToolsDirectory $toolsDirectory)
    Assert-Equal ($withSigned -join '|') (($rows | ForEach-Object { $_.RelativePath }) -join '|') 'the files of the none signer'
    foreach ($row in $rows) { Assert-Equal $row.Before $row.After "the hashes of $($row.RelativePath) under none" }

    # A file signed with a certificate Windows does not trust has no valid signature: it is signed again.
    $certificate = Use-Certificate 'selection'
    try {
        $path = Get-SignedSingleFile 'selection'
        Copy-Item -LiteralPath $path -Destination (Join-Path $tree 'bin/plugins/second.dll') -Force
        $again = @(Select-SigningFile -Root $tree -Policy Unsigned -OwnBinaries @('bin/contour.exe'))
        if ('bin/plugins/second.dll' -notin $again) { throw 'a file signed by an untrusted certificate was not selected again' }
    } finally { Clear-Certificate }
}

Test-Case 'download pin' {
    $table = Get-TestTable
    $source = Join-Path $WorkDirectory 'download source'
    New-Item -ItemType Directory -Force -Path $source | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $source 'tool.exe'), 'a tool')
    $zip = Join-Path $WorkDirectory 'tool.zip'
    Compress-Archive -Path (Join-Path $source 'tool.exe') -DestinationPath $zip -Force
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    $table.Tools.pinned = @{ Source = 'GitHubRelease'; Repository = 'a/b'; Tag = 'v1'; Asset = 'tool.zip'; Sha256 = $hash.ToLowerInvariant(); Executable = 'tool.exe' }
    $tools = Join-Path $WorkDirectory 'pinned tools'
    $copy = { param($Url, $OutFile) if ($Url -notmatch '^https://github.com/a/b/releases/download/v[12]/tool\.zip$') { throw "unexpected URL $Url" }; Copy-Item -LiteralPath $zip -Destination $OutFile }.GetNewClosure()
    $path = Resolve-SigningTool -Name pinned -Table $table -ToolsDirectory $tools -Download $copy
    if (-not (Test-Path -LiteralPath $path) -or -not $path.EndsWith('tool.exe')) { throw "the path is '$path'" }
    # A verified directory is reused: this download would fail.
    $path = Resolve-SigningTool -Name pinned -Table $table -ToolsDirectory $tools -Download { throw 'downloaded again' }

    # Not reused when the pin changed under the same tag, or when the executable is not the one that was verified.
    $other = Get-TestTable
    $other.Tools.pinned = $table.Tools.pinned.Clone()
    $other.Tools.pinned.Sha256 = ('00' * 32)
    Assert-Throws { Resolve-SigningTool -Name pinned -Table $other -ToolsDirectory $tools -Download $copy } @(('00' * 32)) 'a pin that changed'
    $tools2 = Join-Path $WorkDirectory 'pinned tools two'
    $null = Resolve-SigningTool -Name pinned -Table $table -ToolsDirectory $tools2 -Download $copy
    [System.IO.File]::WriteAllText((Join-Path $tools2 'pinned-v1/tool.exe'), 'something else')
    $downloads = @{ Count = 0 }
    $count = { param($Url, $OutFile) $downloads.Count++; Copy-Item -LiteralPath $zip -Destination $OutFile }.GetNewClosure()
    $path = Resolve-SigningTool -Name pinned -Table $table -ToolsDirectory $tools2 -Download $count
    Assert-Equal 1 $downloads.Count 'the downloads after a tool file was replaced'
    Assert-Equal 'a tool' ([System.IO.File]::ReadAllText($path)) 'the tool after it was fetched again'

    $table.Tools.wrong = @{ Source = 'GitHubRelease'; Repository = 'a/b'; Tag = 'v2'; Asset = 'tool.zip'; Sha256 = ('00' * 32); Executable = 'tool.exe' }
    Assert-Throws { Resolve-SigningTool -Name wrong -Table $table -ToolsDirectory $tools -Download $copy } @(('00' * 32), $hash.ToLowerInvariant()) 'a download with the wrong hash'
    if (Test-Path -LiteralPath (Join-Path $tools 'wrong-v2')) { throw 'an expanded directory is left after the wrong hash' }
}

Test-Case 'flat collision' {
    $table = Get-TestTable
    $tree = New-FixtureTree 'collision'
    New-Item -ItemType Directory -Force -Path (Join-Path $tree 'bin/other') | Out-Null
    Copy-Item -LiteralPath (Join-Path $tree 'bin/plugins/second.dll') -Destination (Join-Path $tree 'bin/other/second.dll')
    $marker = Join-Path $WorkDirectory 'collision-marker'
    $step = @{ Tool = 'pwsh'; Invocation = 'Directory'; Arguments = @('-NoProfile', '-Command', "New-Item -ItemType File '$marker'; exit 0", '{inputDir}', '{outputDir}') }
    Assert-Throws { Invoke-SigningStep -Root $tree -Files @('bin/plugins/second.dll', 'bin/other/second.dll') -Step $step -Table $table -ToolsDirectory $toolsDirectory -WorkDirectory (Join-Path $WorkDirectory 'work-collision') } @('second.dll') 'two files of one name'
    if (Test-Path -LiteralPath $marker) { throw 'the tool ran before the collision was found' }
}

Test-Case 'relative paths' {
    # The PowerShell location and the process's directory are two things: every path means the first, as a cmdlet reads it.
    $elsewhere = Join-Path $WorkDirectory 'elsewhere'
    $processDirectory = Join-Path $WorkDirectory 'process directory'
    New-Item -ItemType Directory -Force -Path $elsewhere, $processDirectory | Out-Null
    $tree = New-FixtureTree 'relative'
    $previous = [Environment]::CurrentDirectory
    [Environment]::CurrentDirectory = $processDirectory
    Push-Location -LiteralPath $elsewhere
    try {
        $relativeTree = [System.IO.Path]::GetRelativePath($elsewhere, $tree)
        $rows = @(Invoke-SignBatch -Root $relativeTree -Kind Payload -Signer none -Table (Get-TestTable) -WorkDirectory 'work' -ToolsDirectory 'tools' -Manifest 'out/manifest.tsv')
        Assert-Equal 2 $rows.Count 'the number of rows'
        if (-not (Test-Path -LiteralPath (Join-Path $elsewhere 'out/manifest.tsv'))) { throw 'the manifest was not written below the PowerShell location' }
        if (Test-Path -LiteralPath (Join-Path $processDirectory 'out')) { throw 'the manifest went below the process directory' }
        Assert-Equal 2 @(Read-SigningManifest -Path 'out/manifest.tsv').Count 'the rows read back'
        Assert-Equal 2 @(Get-PortableExecutable -Root $relativeTree).Count 'the executables found'
        $config = 'CPackConfig.cmake'
        [System.IO.File]::WriteAllText((Join-Path $elsewhere $config), "set(CPACK_COMPONENTS_ALL ""a;b"")`n")
        Assert-Equal 'a|b' (@(Get-CPackComponent -CPackConfig $config) -join '|') 'the components of a relative config'
    } finally {
        Pop-Location
        [Environment]::CurrentDirectory = $previous
    }
}

Test-Case 'packages relative to root' {
    # The names of the installers are relative to the folder they are in, or else to where the caller stands.
    $folder = Join-Path $WorkDirectory 'package folder/sub'
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $folder 'a.msi'), 'one')
    [System.IO.File]::WriteAllText((Join-Path $folder 'b.msi'), 'two')
    $elsewhere = Join-Path $WorkDirectory 'elsewhere'
    New-Item -ItemType Directory -Force -Path $elsewhere | Out-Null
    Push-Location -LiteralPath $elsewhere
    try {
        $table = Get-TestTable
        $rows = @(Invoke-SignBatch -Root $folder -Kind Packages -Files @('a.msi', 'b.msi') -Signer none -Table $table -WorkDirectory 'w' -ToolsDirectory 'tools')
        Assert-Equal 'a.msi|b.msi' (($rows | ForEach-Object { $_.RelativePath }) -join '|') 'names relative to the root'
        $relative = [System.IO.Path]::GetRelativePath($elsewhere, $folder)
        $rows = @(Invoke-SignBatch -Root $relative -Kind Packages -Files @("$relative/a.msi") -Signer none -Table $table -WorkDirectory 'w' -ToolsDirectory 'tools')
        Assert-Equal 'a.msi' (($rows | ForEach-Object { $_.RelativePath }) -join '|') 'a name relative to the location'
        Assert-Throws { Invoke-SignBatch -Root $folder -Kind Packages -Files @('missing.msi') -Signer none -Table $table -WorkDirectory 'w' -ToolsDirectory 'tools' } @('missing.msi') 'a package that is not there'
        $out = @(Export-SigningBatch -Root $folder -Kind Packages -Files @('a.msi') -Signer signpath -Table $table -Destination 'exported' -Manifest 'exported.tsv')
        Assert-Equal 1 $out.Count 'the exported packages'
    } finally { Pop-Location }
}

# ---- Online: a timestamp server answers --------------------------------------------------------------------------------

Test-OnlineCase 'payload batch' {
    $certificate = Use-Certificate 'payload'
    try {
        $table = Get-TestTable
        $tree = New-FixtureTree 'payload'
        $notes = Get-FileSha256 -Path (Join-Path $tree 'bin/notes.txt')
        $manifest = Join-Path $WorkDirectory 'payload-manifest.txt'
        $rows = @(Invoke-SignBatch -Root $tree -Kind Payload -Signer dryrun -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-payload') -ToolsDirectory $toolsDirectory -Manifest $manifest -Trust Pinned -Thumbprint $certificate.Thumbprint)
        Assert-Equal 2 $rows.Count 'the number of signed files'
        foreach ($row in $rows) { if ($row.Before -eq $row.After) { throw "$($row.RelativePath) is unchanged" } }
        Assert-Equal $notes (Get-FileSha256 -Path (Join-Path $tree 'bin/notes.txt')) 'the hash of notes.txt'
        Assert-Equal 2 @(Read-SigningManifest -Path $manifest).Count 'the number of manifest rows'

        # Tampered: one byte of the header changed.
        $path = Join-Path $tree 'bin/contour.exe'
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $bytes[0x4E] = $bytes[0x4E] -bxor 0xFF
        [System.IO.File]::WriteAllBytes($path, $bytes)
        $problem = Get-SignatureProblem -Path $path -Trust Pinned -Thumbprint $certificate.Thumbprint
        if ($problem -notlike '*HashMismatch*') { throw "the problem of a tampered file is '$problem'" }

        # Another certificate's signature is not this one's.
        $other = New-ThrowawayCodeSigningCertificate -Directory (Join-Path $WorkDirectory 'cert-other')
        try {
            $problem = Get-SignatureProblem -Path (Join-Path $tree 'bin/plugins/second.dll') -Trust Pinned -Thumbprint $other.Thumbprint
            if ($problem -notlike "*$($certificate.Thumbprint)*" -or $problem -notlike "*$($other.Thumbprint)*") { throw "the problem of another certificate is '$problem'" }
        } finally { Remove-ThrowawayCodeSigningCertificate -Certificate $other }
    } finally { Clear-Certificate }
}

Test-OnlineCase 'timestamp rotation' {
    $certificate = Use-Certificate 'rotation'
    try {
        $table = Get-TestTable
        $table.Signers.dryrun.PayloadStep.TimestampUrls = @('http://127.0.0.1:9/', 'http://timestamp.digicert.com', 'http://timestamp.sectigo.com')
        $tree = New-FixtureTree 'rotation'
        $rows = @(Invoke-SignBatch -Root $tree -Kind Payload -Signer dryrun -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-rotation') -ToolsDirectory $toolsDirectory -Trust Pinned -Thumbprint $certificate.Thumbprint)
        Assert-Equal 2 $rows.Count 'the number of signed files'
    } finally { Clear-Certificate }
}

function New-WrapperScript {
    # A small script that stands in for a vendor tool of the InOut or Directory shape: copies, then signs the copies.
    param([ValidateSet('InOut', 'Directory')][string]$Shape)
    $script = Join-Path $WorkDirectory "wrapper-$Shape.ps1"
    $body = if ($Shape -eq 'InOut') {
        @'
param($Signtool, $Timestamp, $In, $Out)
Copy-Item -LiteralPath $In -Destination $Out -Force
& $Signtool sign /fd sha256 /td sha256 /tr $Timestamp /f $env:CONTOUR_SIGNING_CERT /csp 'Microsoft Software Key Storage Provider' /kc $env:CONTOUR_SIGNING_KEY $Out
exit $LASTEXITCODE
'@
    } else {
        @'
param($Signtool, $Timestamp, $InputDir, $OutputDir)
foreach ($file in Get-ChildItem -LiteralPath $InputDir -File) {
    $copy = Join-Path $OutputDir $file.Name
    Copy-Item -LiteralPath $file.FullName -Destination $copy -Force
    & $Signtool sign /fd sha256 /td sha256 /tr $Timestamp /f $env:CONTOUR_SIGNING_CERT /csp 'Microsoft Software Key Storage Provider' /kc $env:CONTOUR_SIGNING_KEY $copy
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
exit 0
'@
    }
    [System.IO.File]::WriteAllText($script, $body)
    return $script
}

foreach ($shape in 'InOut', 'Directory') {
    Test-OnlineCase "$($shape.ToLowerInvariant()) shape" {
        $certificate = Use-Certificate "shape-$shape"
        try {
            $table = Get-TestTable
            $script = New-WrapperScript -Shape $shape
            $names = if ($shape -eq 'InOut') { @('{in}', '{out}') } else { @('{inputDir}', '{outputDir}') }
            $table.Signers.dryrun.PayloadStep = @{
                Tool          = 'pwsh'
                Invocation    = $shape
                TimestampUrls = @('http://timestamp.digicert.com')
                Arguments     = @('-NoProfile', '-File', $script, '{tool:signtool}', '{timestamp}') + $names
            }
            $tree = New-FixtureTree "shape-$shape"
            if ($shape -eq 'Directory') { Remove-Item -LiteralPath (Join-Path $tree 'bin/plugins/second.dll') ; Copy-Item -LiteralPath $UnsignedPe -Destination (Join-Path $tree 'bin/second.dll') }
            $rows = @(Invoke-SignBatch -Root $tree -Kind Payload -Signer dryrun -Table $table -WorkDirectory (Join-Path $WorkDirectory "work-shape-$shape") -ToolsDirectory $toolsDirectory -Trust Pinned -Thumbprint $certificate.Thumbprint)
            Assert-Equal 2 $rows.Count 'the number of signed files'
        } finally { Clear-Certificate }
    }
}

Test-OnlineCase 'export import' {
    $certificate = Use-Certificate 'export'
    try {
        $table = Get-TestTable
        # A signer that signs elsewhere: only the own binaries go out.
        $table.Signers.elsewhere = @{ Transport = 'SignPath'; Payload = 'Own'; AllowedBuilds = @('releases'); Publisher = '' }
        $tree = New-FixtureTree 'export'
        $before = Get-FileSha256 -Path (Join-Path $tree 'bin/contour.exe')
        $out = Join-Path $WorkDirectory 'exported'
        $manifest = Join-Path $WorkDirectory 'export-manifest.txt'
        $rows = @(Export-SigningBatch -Root $tree -Kind Payload -Signer elsewhere -Table $table -Destination $out -Manifest $manifest)
        Assert-Equal 1 $rows.Count 'the number of exported files'
        Assert-Equal '' @(Read-SigningManifest -Path $manifest)[0].After 'After in the exported manifest'
        Assert-Throws { Invoke-SignBatch -Root $tree -Kind Payload -Signer elsewhere -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-elsewhere') -ToolsDirectory $toolsDirectory } @('export/import') 'a SignPath signer'

        # Signed over there, with the throwaway certificate.
        $null = Invoke-SignBatch -Root $out -Kind Payload -Signer dryrun -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-over-there') -ToolsDirectory $toolsDirectory -Trust Pinned -Thumbprint $certificate.Thumbprint
        $imported = @(Import-SigningBatch -Root $tree -From $out -Manifest $manifest -Trust Pinned -Thumbprint $certificate.Thumbprint)
        Assert-Equal 1 $imported.Count 'the number of imported files'
        if ($imported[0].After -eq $before) { throw 'the staged file was not replaced' }
        Assert-Equal $imported[0].After (Get-FileSha256 -Path (Join-Path $tree 'bin/contour.exe')) 'the staged file'
        Assert-Equal $imported[0].After @(Read-SigningManifest -Path $manifest)[0].After 'After in the completed manifest'
        Assert-Throws { Import-SigningBatch -Root $tree -From $out -Manifest $manifest -Trust Pinned -Thumbprint $certificate.Thumbprint } @('not the one that was exported') 'a second import'

        # Another program, validly signed, is not the file that was exported: its name and version differ.
        $tree2 = New-FixtureTree 'export-other'
        $out2 = Join-Path $WorkDirectory 'exported-other'
        $manifest2 = Join-Path $WorkDirectory 'export-other-manifest.txt'
        $null = Export-SigningBatch -Root $tree2 -Kind Payload -Signer elsewhere -Table $table -Destination $out2 -Manifest $manifest2
        $signtool = Resolve-SigningTool -Name signtool -Table $table -ToolsDirectory $toolsDirectory
        Copy-Item -LiteralPath $signtool -Destination (Join-Path $out2 'bin/contour.exe') -Force
        Assert-Throws { Import-SigningBatch -Root $tree2 -From $out2 -Manifest $manifest2 } @('OriginalFilename') 'a validly signed file of another program'
    } finally { Clear-Certificate }
}

Test-OnlineCase 'tool probes' {
    # Every tool of the table starts the way a signing run starts it (downloads the pinned ones, runs the probe from a
    # folder that is not the tool's). A tool that only starts from its own folder is found out here.
    $table = Get-TestTable
    foreach ($name in @($table.Tools.Keys | Where-Object { $_ -ne 'pwsh' } | Sort-Object)) {
        $started = Test-SigningToolStart -Name $name -Table $table -ToolsDirectory $toolsDirectory
        if (-not (Test-Path -LiteralPath $started.Path)) { throw "$name is not at $($started.Path)" }
    }
}

# ---- The dry run, the shipped-artifact check, staging -----------------------------------------------------------------

function Invoke-Msi {
    # Calls a member of a Windows Installer object: they are IDispatch-only.
    param($Target, [string]$Member, [string]$Kind = 'InvokeMethod', $Arguments = $null)
    return $Target.GetType().InvokeMember($Member, $Kind, $null, $Target, $Arguments)
}

function New-FixtureMsi {
    # A small installer database with the tables an administrative image (msiexec /a) needs, holding the given files
    # (relative path under the install folder -> source file). The files are uncompressed, beside the database in
    # Contour/<path>, so the image is made from them.
    param([string]$Path, [hashtable]$Files = @{ 'bin/contour.exe' = $UnsignedPe })
    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $statements = [System.Collections.Generic.List[string]]::new()
    foreach ($statement in @(
            'CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR NOT NULL LOCALIZABLE PRIMARY KEY `Property`)'
            'CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY `Directory`)'
            'CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `ComponentId` CHAR(38), `Directory_` CHAR(72) NOT NULL, `Attributes` SHORT NOT NULL, `Condition` CHAR(255), `KeyPath` CHAR(72) PRIMARY KEY `Component`)'
            'CREATE TABLE `Feature` (`Feature` CHAR(38) NOT NULL, `Feature_Parent` CHAR(38), `Title` CHAR(64) LOCALIZABLE, `Description` CHAR(255) LOCALIZABLE, `Display` SHORT, `Level` SHORT NOT NULL, `Directory_` CHAR(72), `Attributes` SHORT NOT NULL PRIMARY KEY `Feature`)'
            'CREATE TABLE `FeatureComponents` (`Feature_` CHAR(38) NOT NULL, `Component_` CHAR(72) NOT NULL PRIMARY KEY `Feature_`, `Component_`)'
            'CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL LOCALIZABLE, `FileSize` LONG NOT NULL, `Version` CHAR(72), `Language` CHAR(20), `Attributes` SHORT, `Sequence` SHORT NOT NULL PRIMARY KEY `File`)'
            'CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` SHORT NOT NULL, `DiskPrompt` CHAR(64) LOCALIZABLE, `Cabinet` CHAR(255), `VolumeLabel` CHAR(32), `Source` CHAR(32) PRIMARY KEY `DiskId`)'
            'CREATE TABLE `AdminExecuteSequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)'
            "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('ProductCode', '{$([guid]::NewGuid().ToString().ToUpper())}')"
            "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('ProductName', 'Contour signing fixture')"
            "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('ProductVersion', '1.0.0')"
            "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('ProductLanguage', '1033')"
            "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('Manufacturer', 'Contour')"
            "INSERT INTO ``Directory`` (``Directory``, ``Directory_Parent``, ``DefaultDir``) VALUES ('TARGETDIR', NULL, 'SourceDir')"
            "INSERT INTO ``Directory`` (``Directory``, ``Directory_Parent``, ``DefaultDir``) VALUES ('D_ROOT', 'TARGETDIR', 'Contour')"
            "INSERT INTO ``Feature`` (``Feature``, ``Title``, ``Level``, ``Attributes``) VALUES ('Main', 'Main', 1, 0)"
            "INSERT INTO ``Media`` (``DiskId``, ``LastSequence``) VALUES (1, $($Files.Count))"
        )) { $statements.Add($statement) }
    $directories = @{ '' = 'D_ROOT' }
    $sequence = 0
    foreach ($relative in @($Files.Keys | Sort-Object)) {
        $parts = $relative.Split('/')
        $parent = ''
        foreach ($part in @($parts | Select-Object -SkipLast 1)) {
            $key = if ($parent) { "$parent/$part" } else { $part }
            if (-not $directories.ContainsKey($key)) {
                $directories[$key] = 'D_' + ($key -replace '[^A-Za-z0-9]', '_')
                $statements.Add("INSERT INTO ``Directory`` (``Directory``, ``Directory_Parent``, ``DefaultDir``) VALUES ('$($directories[$key])', '$($directories[$parent])', '$part')")
            }
            $parent = $key
        }
        $sequence++
        $source = $Files[$relative]
        $target = Join-Path (Join-Path $directory 'Contour') $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
        $statements.Add("INSERT INTO ``Component`` (``Component``, ``ComponentId``, ``Directory_``, ``Attributes``, ``KeyPath``) VALUES ('C$sequence', '{$([guid]::NewGuid().ToString().ToUpper())}', '$($directories[$parent])', 0, 'F$sequence')")
        $statements.Add("INSERT INTO ``FeatureComponents`` (``Feature_``, ``Component_``) VALUES ('Main', 'C$sequence')")
        $statements.Add("INSERT INTO ``File`` (``File``, ``Component_``, ``FileName``, ``FileSize``, ``Sequence``) VALUES ('F$sequence', 'C$sequence', '$($parts[-1])', $((Get-Item -LiteralPath $source).Length), $sequence)")
    }
    foreach ($row in @(@('CostInitialize', 800), @('FileCost', 900), @('CostFinalize', 1000), @('InstallValidate', 1400),
            @('InstallInitialize', 1500), @('InstallAdminPackage', 3900), @('InstallFiles', 4000), @('InstallFinalize', 6600))) {
        $statements.Add("INSERT INTO ``AdminExecuteSequence`` (``Action``, ``Sequence``) VALUES ('$($row[0])', $($row[1]))")
    }

    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.OpenDatabase($Path, 3)
    foreach ($statement in $statements) {
        $view = Invoke-Msi $database 'OpenView' -Arguments @($statement)
        $null = Invoke-Msi $view 'Execute'
        $null = Invoke-Msi $view 'Close'
        $null = [System.Runtime.InteropServices.Marshal]::ReleaseComObject($view)
    }
    $summary = Invoke-Msi $database 'SummaryInformation' 'GetProperty' @(20)
    $set = { param($id, $value) $null = Invoke-Msi $summary 'Property' 'SetProperty' @($id, $value) }
    & $set 2 'Contour signing fixture'
    & $set 7 'x64;1033'
    & $set 9 ('{' + [guid]::NewGuid().ToString().ToUpper() + '}')
    & $set 14 200
    & $set 15 0
    $null = Invoke-Msi $summary 'Persist'
    $null = Invoke-Msi $database 'Commit'
    # The file stays locked until the COM objects are gone.
    $null = [System.Runtime.InteropServices.Marshal]::ReleaseComObject($summary)
    $null = [System.Runtime.InteropServices.Marshal]::ReleaseComObject($database)
    $summary = $null
    $database = $null
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    return $Path
}

function Get-TreeHash {
    # Every file under a folder with its hash, as one string: a snapshot to compare.
    param([string]$Root)
    $base = (Resolve-Path -LiteralPath $Root).Path
    return (Get-ChildItem -LiteralPath $base -Recurse -File | Sort-Object FullName | ForEach-Object {
            "$($_.FullName.Substring($base.Length))=$(Get-FileSha256 -Path $_.FullName)"
        }) -join "`n"
}

function New-ZipOfTree {
    # A ZIP of a tree under a top folder, as CPack makes it.
    param([string]$Tree, [string]$Zip, [string]$Top = 'Contour-0.0.0-win64')
    $source = Join-Path (Split-Path -Parent $Zip) "zip-source-$([IO.Path]::GetFileNameWithoutExtension($Zip))"
    if (Test-Path -LiteralPath $source) { Remove-Item -LiteralPath $source -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Join-Path $source $Top) | Out-Null
    Get-ChildItem -LiteralPath $Tree -Force | Copy-Item -Destination (Join-Path $source $Top) -Recurse
    if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
    Compress-Archive -Path (Join-Path $source $Top) -DestinationPath $Zip
    return $source
}

function Get-TreeFile {
    # The executables of a tree as the table of a fixture installer: relative path -> file.
    param([string]$Tree)
    $files = @{}
    foreach ($relative in Get-PortableExecutable -Root $Tree) { $files[$relative] = Join-Path $Tree $relative }
    return $files
}

Test-Case 'components' {
    $withList = Join-Path $WorkDirectory 'CPackConfig-with.cmake'
    [System.IO.File]::WriteAllText($withList, "set(CPACK_BINARY_WIX ""ON"")`nset(CPACK_COMPONENTS_ALL ""Unspecified;contour;contour_path"")`nset(CPACK_COMPONENTS_ALL_SET_BY_USER ""TRUE"")`n")
    Assert-Equal 'Unspecified|contour|contour_path' (@(Get-CPackComponent -CPackConfig $withList) -join '|') 'the components'
    $without = Join-Path $WorkDirectory 'CPackConfig-without.cmake'
    [System.IO.File]::WriteAllText($without, "set(CPACK_BINARY_WIX ""ON"")`n")
    Assert-Equal 0 @(Get-CPackComponent -CPackConfig $without).Count 'the number of components of a config that names none'
}

Test-Case 'staging' {
    # A tiny project with three components, two of them in CPack's list: only those two are staged, by the verb as run.
    $project = Join-Path $WorkDirectory 'staging project'
    $build = Join-Path $WorkDirectory 'staging build'
    New-Item -ItemType Directory -Force -Path $project | Out-Null
    foreach ($name in 'a.txt', 'b.txt', 'c.txt') { [System.IO.File]::WriteAllText((Join-Path $project $name), $name) }
    [System.IO.File]::WriteAllText((Join-Path $project 'CMakeLists.txt'), @'
cmake_minimum_required(VERSION 3.20)
project(staging NONE)
install(FILES a.txt DESTINATION bin COMPONENT one)
install(FILES b.txt DESTINATION share COMPONENT two)
install(FILES c.txt DESTINATION extra COMPONENT three)
set(CPACK_COMPONENTS_ALL one two)
include(CPack)
'@)
    & cmake -S $project -B $build | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake configure exited $LASTEXITCODE" }
    $stage = Join-Path $WorkDirectory 'staging stage'
    $result = Invoke-Cli stage -BuildDirectory $build -Destination $stage
    if ($result.ExitCode -ne 0) { throw "stage exited $($result.ExitCode): $($result.Output)" }
    foreach ($staged in 'bin/a.txt', 'share/b.txt') { if (-not (Test-Path -LiteralPath (Join-Path $stage $staged))) { throw "$staged was not staged" } }
    if (Test-Path -LiteralPath (Join-Path $stage 'extra')) { throw 'a component CPack does not package was staged' }
}

Test-Case 'dry run wrong root' {
    $table = Get-TestTable
    $empty = Join-Path $WorkDirectory 'not a stage'
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $work = Join-Path $WorkDirectory 'dry-run-wrong'
    Assert-Throws { Invoke-SigningDryRun -Root $empty -Table $table -WorkDirectory $work -ToolsDirectory $toolsDirectory } @('bin/contour.exe') 'a root without contour.exe'
    if (Test-Path -LiteralPath $work) { throw 'the work directory was made before the root was checked' }
    $tree = New-FixtureTree 'dry-run-inside'
    Assert-Throws { Invoke-SigningDryRun -Root $tree -Table $table -WorkDirectory (Join-Path $tree 'work') -ToolsDirectory $toolsDirectory } @('must not contain') 'a work directory inside the tree'
}

Test-OnlineCase 'dry run' {
    $table = Get-TestTable
    $tree = New-FixtureTree 'dry-run'
    $msi = New-FixtureMsi -Path (Join-Path $WorkDirectory 'dry-run msi/fixture.msi')
    $treeBefore = Get-TreeHash -Root $tree
    $msiBefore = Get-FileSha256 -Path $msi
    $work = Join-Path $WorkDirectory 'dry-run-work'
    # A caller's own values of the dry run's variables are theirs.
    $env:CONTOUR_SIGNING_CERT = 'callers-certificate'
    $env:CONTOUR_SIGNING_KEY = 'callers-key'
    try {
        $said = Invoke-SigningDryRun -Root $tree -Packages @($msi) -Table $table -WorkDirectory $work -ToolsDirectory $toolsDirectory 6>&1 | Out-String
        if ($said -notlike '*signing dry run: 2 payload files and 1 packages signed and verified with a throwaway certificate*') { throw "the summary line is missing: $said" }
        Assert-Equal 'callers-certificate' $env:CONTOUR_SIGNING_CERT 'the caller''s CONTOUR_SIGNING_CERT'
        Assert-Equal 'callers-key' $env:CONTOUR_SIGNING_KEY 'the caller''s CONTOUR_SIGNING_KEY'
    } finally { Remove-Item Env:CONTOUR_SIGNING_CERT, Env:CONTOUR_SIGNING_KEY -ErrorAction SilentlyContinue }
    Assert-Equal $treeBefore (Get-TreeHash -Root $tree) 'the original tree'
    Assert-Equal $msiBefore (Get-FileSha256 -Path $msi) 'the original installer'
    if (@(Get-ChildItem -LiteralPath $work -Recurse -Include '*.pfx', '*.cer' -ErrorAction SilentlyContinue).Count -gt 0) { throw 'the certificate is still there' }
    # The verb, as the workflow runs it.
    $result = Invoke-Cli dry-run -Root $tree -Packages $msi -WorkDirectory (Join-Path $WorkDirectory 'dry-run-cli') -ToolsDirectory $toolsDirectory
    if ($result.ExitCode -ne 0 -or $result.Output -notlike '*signing dry run: 2 payload files and 1 packages*') { throw "the dry-run verb exited $($result.ExitCode): $($result.Output)" }
}

# A shipped ZIP and installer of the unsigned fixture, and a manifest that says so.
$shipTree = New-FixtureTree 'ship'
$shipZip = Join-Path $WorkDirectory 'ship/Contour-0.0.0-win64.zip'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $shipZip) | Out-Null
$shipMsi = New-FixtureMsi -Path (Join-Path $WorkDirectory 'ship/unsigned.msi') -Files (Get-TreeFile -Tree $shipTree)
$shipManifest = Join-Path $WorkDirectory 'ship/manifest.txt'
$null = Invoke-SignBatch -Root $shipTree -Kind Payload -Signer none -Table (Get-TestTable) -WorkDirectory (Join-Path $WorkDirectory 'work-ship') -ToolsDirectory $toolsDirectory -Manifest $shipManifest
$null = New-ZipOfTree -Tree $shipTree -Zip $shipZip

Test-Case 'verify none' {
    Test-ShippedArtifact -Signer none -Table (Get-TestTable) -Manifest $shipManifest -Zip $shipZip -Msi @($shipMsi) -WorkDirectory (Join-Path $WorkDirectory 'verify-none')
}

Test-Case 'kept files' {
    # The manifest names every executable of the stage: the ones not signed with '-', and a package must carry them as staged.
    $tree = New-FixtureTree 'kept'
    $signtool = Resolve-SigningTool -Name signtool -Table (Get-TestTable) -ToolsDirectory $toolsDirectory
    Copy-Item -LiteralPath $signtool -Destination (Join-Path $tree 'bin/vendor.dll')
    $manifest = Join-Path $WorkDirectory 'kept/manifest.tsv'
    $rows = @(Invoke-SignBatch -Root $tree -Kind Payload -Signer none -Table (Get-TestTable) -WorkDirectory (Join-Path $WorkDirectory 'work-kept') -ToolsDirectory $toolsDirectory -Manifest $manifest)
    Assert-Equal 2 $rows.Count 'the files signed'
    $all = @(Read-SigningManifest -Path $manifest)
    Assert-Equal 3 $all.Count 'the rows of the manifest'
    $kept = @($all | Where-Object { $_.SignedPath -eq '-' })
    Assert-Equal 'bin/vendor.dll' ($kept | ForEach-Object { $_.RelativePath }) 'the kept file'
    $zip = Join-Path $WorkDirectory 'kept/Contour-0.0.0-win64.zip'
    $null = New-ZipOfTree -Tree $tree -Zip $zip
    $msi = New-FixtureMsi -Path (Join-Path $WorkDirectory 'kept/kept.msi') -Files (Get-TreeFile -Tree $tree)
    Test-ShippedArtifact -Signer none -Table (Get-TestTable) -Manifest $manifest -Zip $zip -Msi @($msi) -WorkDirectory (Join-Path $WorkDirectory 'verify-kept')
    # Another build of the kept file in the ZIP is not the staged one.
    Copy-Item -LiteralPath $UnsignedPe -Destination (Join-Path $tree 'bin/vendor.dll') -Force
    $other = Join-Path $WorkDirectory 'kept/other.zip'
    $null = New-ZipOfTree -Tree $tree -Zip $other
    Assert-Throws { Test-ShippedArtifact -Signer none -Table (Get-TestTable) -Manifest $manifest -Zip $other -Msi @($msi) -WorkDirectory (Join-Path $WorkDirectory 'verify-kept-other') } @('vendor.dll', 'is not the staged file') 'a kept file that is another build'
}

Test-Case 'verify real signer on unsigned packages' {
    # What a release would ship if signing did not happen: refused for every kind of signer, naming what is unsigned.
    foreach ($signer in 'certum', 'signpath') {
        Assert-Throws { Test-ShippedArtifact -Signer $signer -Table (Get-TestTable) -Manifest $shipManifest -Zip $shipZip -Msi @($shipMsi) -WorkDirectory (Join-Path $WorkDirectory "verify-$signer") } @('NotSigned', 'unsigned.msi', 'contour.exe') "the $signer signer on unsigned packages"
    }
}

Test-Case 'verify own binaries' {
    # A manifest that names nothing does not excuse an unsigned contour.exe from a signer that signs only that.
    $empty = Join-Path $WorkDirectory 'ship/empty-manifest.txt'
    Write-SigningManifest -Path $empty -Rows @()
    Assert-Throws { Test-ShippedArtifact -Signer signpath -Table (Get-TestTable) -Manifest $empty -Zip $shipZip -Msi @($shipMsi) -WorkDirectory (Join-Path $WorkDirectory 'verify-own') } @('bin/contour.exe', 'is not in the manifest') 'an own binary missing from the manifest'
}

Test-Case 'verify verb' {
    $result = Invoke-Cli verify -Signer none -Manifest $shipManifest -Zip $shipZip -Msi $shipMsi -WorkDirectory (Join-Path $WorkDirectory 'verify-cli')
    if ($result.ExitCode -ne 0 -or $result.Output -notlike '*are what the none signer should ship*') { throw "the verify verb exited $($result.ExitCode): $($result.Output)" }
    $result = Invoke-Cli verify -Signer certum -Manifest $shipManifest -Zip $shipZip -Msi $shipMsi -WorkDirectory (Join-Path $WorkDirectory 'verify-cli-certum')
    if ($result.ExitCode -eq 0) { throw 'the verify verb accepted unsigned packages for a real signer' }
}

Test-Case 'leak guard zip' {
    $certificate = Use-Certificate 'leak-zip'
    try {
        $tree = New-FixtureTree 'leak-zip'
        Invoke-ThrowawaySigntool -Path (Join-Path $tree 'bin/plugins/second.dll')
        $zip = Join-Path $WorkDirectory 'leak-zip/Contour-0.0.0-win64.zip'
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $zip) | Out-Null
        $null = New-ZipOfTree -Tree $tree -Zip $zip
        Assert-Throws { Test-ShippedArtifact -Signer none -Table (Get-TestTable) -Manifest $shipManifest -Zip $zip -Msi @($shipMsi) -WorkDirectory (Join-Path $WorkDirectory 'verify-leak-zip') } @('second.dll', 'UnknownError') 'a ZIP with a throwaway-signed file'
    } finally { Clear-Certificate }
}

Test-Case 'leak guard msi' {
    $certificate = Use-Certificate 'leak-msi'
    try {
        $msi = New-FixtureMsi -Path (Join-Path $WorkDirectory 'leak-msi/signed.msi') -Files (Get-TreeFile -Tree $shipTree)
        Invoke-ThrowawaySigntool -Path $msi
        Assert-Throws { Test-ShippedArtifact -Signer none -Table (Get-TestTable) -Manifest $shipManifest -Zip $shipZip -Msi @($msi) -WorkDirectory (Join-Path $WorkDirectory 'verify-leak-msi') } @('signed.msi') 'an installer signed by a dry run'
    } finally { Clear-Certificate }
}

Test-Case 'leak guard msi payload' {
    # The installer is unsigned, but a file inside it is signed by a dry run: the ZIP may be clean, the installer is not.
    $certificate = Use-Certificate 'leak-msi-payload'
    try {
        $tree = New-FixtureTree 'leak-msi-payload'
        Invoke-ThrowawaySigntool -Path (Join-Path $tree 'bin/plugins/second.dll')
        $msi = New-FixtureMsi -Path (Join-Path $WorkDirectory 'leak-msi-payload/payload.msi') -Files (Get-TreeFile -Tree $tree)
        Assert-Throws { Test-ShippedArtifact -Signer none -Table (Get-TestTable) -Manifest $shipManifest -Zip $shipZip -Msi @($msi) -WorkDirectory (Join-Path $WorkDirectory 'verify-leak-msi-payload') } @('second.dll', 'UnknownError', 'payload.msi') 'an installer with a throwaway-signed file'
        # And an installer that lacks a file of the manifest, or carries another build of it.
        $missing = New-FixtureMsi -Path (Join-Path $WorkDirectory 'leak-msi-payload/missing.msi') -Files @{ 'bin/notes.exe' = $UnsignedPe }
        Assert-Throws { Test-ShippedArtifact -Signer none -Table (Get-TestTable) -Manifest $shipManifest -Zip $shipZip -Msi @($missing) -WorkDirectory (Join-Path $WorkDirectory 'verify-missing-msi') } @('bin/contour.exe', 'missing.msi', 'no such entry') 'an installer without a file of the manifest'
    } finally { Clear-Certificate }
}

Test-Case 'not the signed file' {
    $rows = @(Read-SigningManifest -Path $shipManifest)
    $rows[0].After = ('ab' * 32)
    $other = Join-Path $WorkDirectory 'ship/manifest-other.txt'
    Write-SigningManifest -Path $other -Rows $rows
    Assert-Throws { Test-ShippedArtifact -Signer none -Table (Get-TestTable) -Manifest $other -Zip $shipZip -Msi @($shipMsi) -WorkDirectory (Join-Path $WorkDirectory 'verify-other') } @($rows[0].RelativePath, 'is not the signed file') 'a manifest that is not the ZIP''s'
}

Test-Case 'command line verbs' {
    # One case per verb of scripts/sign-windows.ps1 that runs offline, as a workflow step runs it (plan, stage, verify
    # and dry-run have cases of their own).
    $tree = New-FixtureTree 'cli'
    $manifest = Join-Path $WorkDirectory 'cli/manifest.tsv'
    $result = Invoke-Cli sign -Kind Payload -Root $tree -Signer none -Manifest $manifest -WorkDirectory (Join-Path $WorkDirectory 'cli/work')
    if ($result.ExitCode -ne 0 -or $result.Output -notlike '*signed 2 file(s) with none*') { throw "sign exited $($result.ExitCode): $($result.Output)" }
    Assert-Equal 2 @(Read-SigningManifest -Path $manifest).Count 'the rows of the manifest'

    $exported = Join-Path $WorkDirectory 'cli/exported'
    $exportManifest = Join-Path $WorkDirectory 'cli/export.tsv'
    $result = Invoke-Cli export -Kind Payload -Root $tree -Signer signpath -Destination $exported -Manifest $exportManifest
    if ($result.ExitCode -ne 0 -or $result.Output -notlike '*exported 1 file(s)*') { throw "export exited $($result.ExitCode): $($result.Output)" }
    $result = Invoke-Cli import -Root $tree -From $exported -Manifest $exportManifest -Signer signpath
    if ($result.ExitCode -eq 0) { throw 'import accepted a file nobody signed' }

    $result = Invoke-Cli sign -Kind Payload -Root $tree -Signer nosuch -Manifest $manifest -WorkDirectory (Join-Path $WorkDirectory 'cli/work')
    if ($result.ExitCode -eq 0 -or $result.Output -notlike "*no signer 'nosuch'*") { throw "an unknown signer: exit $($result.ExitCode): $($result.Output)" }

    $result = Invoke-Cli tools -Signer signtool -ToolsDirectory $toolsDirectory
    if ($result.ExitCode -ne 0 -or $result.Output -notlike '*signtool:*') { throw "tools exited $($result.ExitCode): $($result.Output)" }
}

Clear-Certificate

# The throwaway keys of this run were all deleted: signtool with a .pfx would have left one file each.
Test-Case 'no key files left' {
    Assert-Equal $keyFilesAtStart (Get-KeyFileCount) 'the number of files under %APPDATA%\Microsoft\Crypto'
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host $_ }
    exit 1
}
$skipped = if ($script:SkippedOnline -gt 0) { " ($($script:SkippedOnline) online cases skipped)" } else { '' }
Write-Host "windows-signing: OK$skipped"
