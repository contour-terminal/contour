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

# ---- Signing and verifying, with a throwaway certificate -------------------------------------------------------------

$toolsDirectory = Join-Path $WorkDirectory 'tools'
$fixtureRoot = Join-Path $WorkDirectory 'signing fixture'

function Get-TestTable {
    # The shipped table, fresh, with no waiting between attempts and a PowerShell tool for the cases that need a
    # program that does something simple.
    $fresh = Import-PowerShellDataFile -LiteralPath (Join-Path $PSScriptRoot 'signers.psd1')
    $fresh.RetryDelaySeconds = @(0, 0)
    $fresh.Tools.pwsh = @{ Source = 'Path'; Executable = 'pwsh' }
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
    # Makes a throwaway certificate and points the dry-run row's two variables at it.
    param([string]$Name)
    $certificate = New-ThrowawayCodeSigningCertificate -Directory (Join-Path $WorkDirectory "cert-$Name")
    $env:CONTOUR_SIGNING_PFX = $certificate.PfxPath
    $env:CONTOUR_SIGNING_PFX_PASSWORD = $certificate.Password
    return $certificate
}

function Clear-Certificate {
    Remove-Item Env:CONTOUR_SIGNING_PFX, Env:CONTOUR_SIGNING_PFX_PASSWORD -ErrorAction SilentlyContinue
}

function Test-OnlineCase {
    # A case that needs a timestamp server: mandatory on CI, elsewhere skipped, with a notice, where none answers.
    param([string]$Name, [scriptblock]$Body)
    if ($env:CI -ne 'true') {
        try {
            $null = Invoke-WebRequest -Uri 'http://timestamp.digicert.com' -Method Head -TimeoutSec 10 -SkipHttpErrorCheck
        } catch {
            Write-Host "skipped (no timestamp server reachable): $Name"
            return
        }
    }
    Test-Case $Name $Body
}

function Get-SignedSingleFile {
    # Signs a copy of the executable with the throwaway certificate; returns its path.
    param([string]$Name, [switch]$Timestamped)
    $path = Join-Path $WorkDirectory "single-$Name.exe"
    Copy-Item -LiteralPath $UnsignedPe -Destination $path -Force
    $signtool = Resolve-SigningTool -Name signtool -Table (Get-TestTable) -ToolsDirectory $toolsDirectory
    $arguments = @('sign', '/fd', 'sha256', '/f', $env:CONTOUR_SIGNING_PFX, '/p', $env:CONTOUR_SIGNING_PFX_PASSWORD)
    if ($Timestamped) { $arguments += @('/td', 'sha256', '/tr', 'http://timestamp.digicert.com') }
    & $signtool @arguments $path | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool exited $LASTEXITCODE" }
    return $path
}

Test-Case 'throwaway certificate' {
    $before = @(Get-ChildItem Cert:\CurrentUser\My).Count
    $certificate = New-ThrowawayCodeSigningCertificate -Directory (Join-Path $WorkDirectory 'cert-plain')
    if (-not (Test-Path -LiteralPath $certificate.PfxPath)) { throw 'no .pfx was written' }
    if ($certificate.Thumbprint -notmatch '^[0-9A-F]{40}$') { throw "the thumbprint is '$($certificate.Thumbprint)'" }
    Assert-Equal $before @(Get-ChildItem Cert:\CurrentUser\My).Count 'the number of certificates in the current user''s store'
    $loaded = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificate.PfxPath, $certificate.Password)
    if ($loaded.Subject -ne 'CN=Contour signing dry run (throwaway)') { throw "the subject is '$($loaded.Subject)'" }
    if ($loaded.NotAfter -gt [DateTime]::UtcNow.AddDays(1.1)) { throw 'the certificate lives longer than a day' }
}

Test-Case 'unsigned file' {
    $tree = New-FixtureTree 'unsigned'
    $problem = Get-SignatureProblem -Path (Join-Path $tree 'bin/contour.exe') -Trust System
    if ($problem -notlike '*NotSigned*') { throw "the problem is '$problem'" }
}

Test-Case 'missing secret' {
    $table = Get-TestTable
    $table.Signers.dryrun.Secrets = @('CONTOUR_TEST_UNSET', 'CONTOUR_TEST_UNSET_TOO')
    $table.Signers.dryrun.PayloadStep.Tool = 'nosuchtool'
    $table.Tools.nosuchtool = @{ Source = 'Path'; Executable = 'nosuchtool-that-is-not-there' }
    $tree = New-FixtureTree 'secret'
    Assert-Throws { Invoke-SignBatch -Root $tree -Kind Payload -Signer dryrun -Table $table -WorkDirectory (Join-Path $WorkDirectory 'work-secret') -ToolsDirectory $toolsDirectory } @('CONTOUR_TEST_UNSET', 'CONTOUR_TEST_UNSET_TOO') 'a missing secret'
}

Test-Case 'failing tool' {
    $table = Get-TestTable
    $step = @{ Tool = 'pwsh'; Invocation = 'InPlace'; Arguments = @('-NoProfile', '-Command', 'exit 3') }
    $tree = New-FixtureTree 'failing'
    Assert-Throws { Invoke-SigningStep -Root $tree -Files @('bin/contour.exe') -Step $step -Table $table -ToolsDirectory $toolsDirectory -WorkDirectory (Join-Path $WorkDirectory 'work-failing') } @('pwsh', 'exit code 3', '3 attempts') 'a failing tool'
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
        $table.Signers.dryrun.PayloadStep.Arguments = @('sign', '/fd', 'sha256', '/f', '{env:CONTOUR_SIGNING_PFX}', '/p', '{env:CONTOUR_SIGNING_PFX_PASSWORD}', '{file}')
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
        $problem = Get-SignatureProblem -Path (Join-Path $tree 'bin/plugins/second.dll') -Trust Pinned -Thumbprint $other.Thumbprint
        if ($problem -notlike "*$($certificate.Thumbprint)*" -or $problem -notlike "*$($other.Thumbprint)*") { throw "the problem of another certificate is '$problem'" }
    } finally { Clear-Certificate }
}

Test-OnlineCase 'timestamp rotation' {
    $certificate = Use-Certificate 'rotation'
    try {
        $table = Get-TestTable
        $table.Signers.dryrun.PayloadStep.TimestampUrls = @('http://127.0.0.1:9/', 'http://timestamp.digicert.com')
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
& $Signtool sign /fd sha256 /td sha256 /tr $Timestamp /f $env:CONTOUR_SIGNING_PFX /p $env:CONTOUR_SIGNING_PFX_PASSWORD $Out
exit $LASTEXITCODE
'@
    } else {
        @'
param($Signtool, $Timestamp, $InputDir, $OutputDir)
foreach ($file in Get-ChildItem -LiteralPath $InputDir -File) {
    $copy = Join-Path $OutputDir $file.Name
    Copy-Item -LiteralPath $file.FullName -Destination $copy -Force
    & $Signtool sign /fd sha256 /td sha256 /tr $Timestamp /f $env:CONTOUR_SIGNING_PFX /p $env:CONTOUR_SIGNING_PFX_PASSWORD $copy
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
        $table.Signers.elsewhere = @{ Transport = 'SignPath'; Payload = 'Own'; AllowedBuilds = @('releases') }
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
    } finally { Clear-Certificate }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host 'windows-signing: OK'
