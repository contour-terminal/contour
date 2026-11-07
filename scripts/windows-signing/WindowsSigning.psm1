# The decisions of Windows code signing that need no signer and no network: what the signer table may say, which
# signer a build uses, how a command line is filled in, which files are portable executables, and the manifest of
# what was signed. The table is scripts/windows-signing/signers.psd1.

Set-StrictMode -Version Latest

# The functions that return lists return them unrolled, as PowerShell does: a caller wraps the call in @(...), or its
# `.Count` is an error under strict mode for an empty result and for a single one.

# The values a field of the table may have.
$script:Transports = 'None', 'SignPath', 'Command'
$script:Payloads = 'Own', 'Unsigned'
$script:BuildSettings = 'releases', 'releases-and-master'
$script:Invocations = 'InPlace', 'InOut', 'Directory'
$script:ToolSources = 'WindowsSdk', 'Path', 'GitHubRelease'

function Get-TableValue {
    # A key of a hashtable, or $null where it has none (strict mode throws on a missing property, not on a key).
    param($Hashtable, [string]$Key)
    if ($Hashtable -is [hashtable] -and $Hashtable.ContainsKey($Key)) { return $Hashtable[$Key] }
    return $null
}

function Test-SigningStep {
    # The problems of one step of a signer.
    param([string]$Where, $Step, [hashtable]$Tools)
    $problems = [System.Collections.Generic.List[string]]::new()
    $tool = Get-TableValue $Step 'Tool'
    if ($tool -isnot [string] -or -not $Tools.ContainsKey($tool)) {
        $problems.Add("$Where`: Tool '$tool' is not one of the Tools")
    }
    $invocation = Get-TableValue $Step 'Invocation'
    if ($invocation -notin $script:Invocations) {
        $problems.Add("$Where`: Invocation '$invocation' is not one of $($script:Invocations -join ', ')")
    }
    $arguments = @(Get-TableValue $Step 'Arguments' | Where-Object { $_ -is [string] })
    $usesTimestamp = @($arguments | Where-Object { $_.Contains('{timestamp}') }).Count -gt 0
    $urls = Get-TableValue $Step 'TimestampUrls'
    if ($usesTimestamp -and ($null -eq $urls -or @($urls).Count -eq 0)) {
        $problems.Add("$Where`: Arguments use {timestamp} and there are no TimestampUrls")
    }
    foreach ($argument in $arguments) {
        foreach ($match in [regex]::Matches($argument, '\{tool:([^{}]+)\}')) {
            if (-not $Tools.ContainsKey($match.Groups[1].Value)) {
                $problems.Add("$Where`: {tool:$($match.Groups[1].Value)} names no tool")
            }
        }
    }
    return , $problems
}

function Test-SigningTable {
    <#
    .SYNOPSIS
        Checks a signer table against the rules the signing code relies on.
    .PARAMETER Table
        The table, as Import-PowerShellDataFile reads signers.psd1.
    .OUTPUTS
        System.String[]. One problem per broken rule, naming the signer or tool; empty when the table is valid.
    #>
    [CmdletBinding()]
    [OutputType([string[]])]
    param([Parameter(Mandatory)][hashtable]$Table)
    $problems = [System.Collections.Generic.List[string]]::new()
    $tools = Get-TableValue $Table 'Tools'
    if ($tools -isnot [hashtable]) { $tools = @{} }

    foreach ($name in @($tools.Keys | Sort-Object)) {
        $tool = $tools[$name]
        $where = "tool '$name'"
        $source = Get-TableValue $tool 'Source'
        if ($source -notin $script:ToolSources) {
            $problems.Add("$where`: Source '$source' is not one of $($script:ToolSources -join ', ')")
            continue
        }
        $executable = Get-TableValue $tool 'Executable'
        if ($executable -isnot [string] -or $executable -eq '') {
            $problems.Add("$where`: Executable is missing")
        } elseif ($source -eq 'GitHubRelease' -and $executable -match '[<>]') {
            $problems.Add("$where`: Executable '$executable' is a placeholder, not a path")
        }
        if ($source -eq 'GitHubRelease') {
            foreach ($field in 'Repository', 'Tag', 'Asset') {
                $value = Get-TableValue $tool $field
                if ($value -isnot [string] -or $value -eq '') { $problems.Add("$where`: $field is missing") }
            }
            $hash = Get-TableValue $tool 'Sha256'
            if ($hash -isnot [string] -or $hash -notmatch '^[0-9a-fA-F]{64}$') {
                $problems.Add("$where`: Sha256 '$hash' is not 64 hexadecimal digits")
            }
        }
    }

    $signers = Get-TableValue $Table 'Signers'
    if ($signers -isnot [hashtable]) { $signers = @{} }
    foreach ($name in @($signers.Keys | Sort-Object)) {
        $signer = $signers[$name]
        $where = "signer '$name'"
        $transport = Get-TableValue $signer 'Transport'
        if ($transport -notin $script:Transports) {
            $problems.Add("$where`: Transport '$transport' is not one of $($script:Transports -join ', ')")
        }
        $payload = Get-TableValue $signer 'Payload'
        if ($payload -notin $script:Payloads) {
            $problems.Add("$where`: Payload '$payload' is not one of $($script:Payloads -join ', ')")
        }
        foreach ($build in @(Get-TableValue $signer 'AllowedBuilds') | Where-Object { $null -ne $_ }) {
            if ($build -notin $script:BuildSettings) {
                $problems.Add("$where`: AllowedBuilds names '$build', not one of $($script:BuildSettings -join ', ')")
            }
        }
        if ($transport -eq 'Command') {
            foreach ($stepName in 'PayloadStep', 'PackageStep') {
                $step = Get-TableValue $signer $stepName
                if ($step -isnot [hashtable]) {
                    $problems.Add("$where`: a Command signer needs a $stepName")
                    continue
                }
                foreach ($problem in (Test-SigningStep -Where "$where $stepName" -Step $step -Tools $tools)) {
                    $problems.Add($problem)
                }
            }
        }
    }
    return $problems.ToArray()
}

function Import-SigningTable {
    <#
    .SYNOPSIS
        Reads the signer table and throws unless it is valid.
    .PARAMETER Path
        The .psd1 file; signers.psd1 beside this module by default.
    .OUTPUTS
        System.Collections.Hashtable. The table.
    #>
    [CmdletBinding()]
    [OutputType([hashtable])]
    param([string]$Path = (Join-Path $PSScriptRoot 'signers.psd1'))
    $table = Import-PowerShellDataFile -LiteralPath $Path
    $problems = @(Test-SigningTable -Table $table)
    if ($problems.Count -gt 0) {
        throw "$Path is not a valid signer table:`n  $($problems -join "`n  ")"
    }
    return $table
}

function Resolve-SigningPlan {
    <#
    .SYNOPSIS
        Decides which signer a build uses and how.
    .PARAMETER Table
        The signer table.
    .PARAMETER Signer
        The effective signer: what the build is to sign with. Empty means none.
    .PARAMETER Configured
        The signer the repository is configured for. Empty means none.
    .PARAMETER Builds
        Which builds sign: releases or releases-and-master. Empty means releases.
    .OUTPUTS
        System.Management.Automation.PSCustomObject with Signer, Transport (lowercase none, signpath or command) and
        DryRun (a build that signs nothing, but is to take the whole path with a throwaway certificate).
    #>
    [CmdletBinding()]
    [OutputType([pscustomobject])]
    param(
        [Parameter(Mandatory)][hashtable]$Table,
        [AllowEmptyString()][Parameter(Mandatory)][string]$Signer,
        [AllowEmptyString()][Parameter(Mandatory)][string]$Configured,
        [AllowEmptyString()][Parameter(Mandatory)][string]$Builds
    )
    if ($Signer -eq '') { $Signer = 'none' }
    if ($Configured -eq '') { $Configured = 'none' }
    if ($Builds -eq '') { $Builds = 'releases' }
    if ($Builds -notin $script:BuildSettings) {
        throw "the builds setting '$Builds' is not one of $($script:BuildSettings -join ', ')"
    }
    $signers = $Table.Signers
    if (-not $signers.ContainsKey($Configured)) {
        throw "the configured signer '$Configured' is not one of $(@($signers.Keys | Sort-Object) -join ', ')"
    }
    $allowed = @($signers[$Configured].AllowedBuilds)
    if ($Configured -ne 'none') {
        if ($allowed.Count -eq 0) {
            throw "the signer '$Configured' is for local signing only (AllowedBuilds is empty), not for a build to select"
        }
        if ($Builds -notin $allowed) {
            throw "the signer '$Configured' signs $($allowed -join ', ') only (its AllowedBuilds), not '$Builds'"
        }
    }
    if ($Signer -ne 'none' -and $Signer -ne $Configured) {
        throw "the effective signer '$Signer' is neither none nor the configured '$Configured'"
    }
    return [pscustomobject]@{
        Signer    = $Signer
        Transport = ([string]$signers[$Signer].Transport).ToLowerInvariant()
        DryRun    = [bool]($Signer -eq 'none')
    }
}

function Expand-SigningArgument {
    <#
    .SYNOPSIS
        Fills a command-line template in.
    .PARAMETER Template
        The arguments, with {name} placeholders.
    .PARAMETER Values
        The values of the placeholders by name ({file} is the key file; {tool:X} is the key tool:X). {env:NAME} reads
        the process environment and throws, naming only NAME, where it is unset or empty.
    .OUTPUTS
        System.String[]. The arguments, in order, one per template element.
    #>
    [CmdletBinding()]
    [OutputType([string[]])]
    param([Parameter(Mandatory)][string[]]$Template, [Parameter(Mandatory)][hashtable]$Values)
    $expanded = foreach ($argument in $Template) {
        $text = [System.Text.StringBuilder]::new()
        $position = 0
        foreach ($match in [regex]::Matches($argument, '\{([^{}]+)\}')) {
            $null = $text.Append($argument, $position, $match.Index - $position)
            $position = $match.Index + $match.Length
            $name = $match.Groups[1].Value
            if ($name.StartsWith('env:')) {
                $variable = $name.Substring(4)
                $value = [Environment]::GetEnvironmentVariable($variable)
                if ([string]::IsNullOrEmpty($value)) { throw "the environment variable $variable is not set" }
                $null = $text.Append($value)
            } elseif ($Values.ContainsKey($name)) {
                $null = $text.Append([string]$Values[$name])
            } else {
                throw "the placeholder {$name} has no value"
            }
        }
        $null = $text.Append($argument, $position, $argument.Length - $position)
        $text.ToString()
    }
    return @($expanded)
}

function Test-PortableExecutable {
    # Whether a file starts with MZ and has PE\0\0 where e_lfanew says.
    param([string]$Path)
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        if ($stream.Length -lt 0x40) { return $false }
        $header = [byte[]]::new(0x40)
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length) { return $false }
        if ($header[0] -ne 0x4D -or $header[1] -ne 0x5A) { return $false }
        $offset = [BitConverter]::ToInt32($header, 0x3C)
        if ($offset -lt 0 -or [long]$offset + 4 -gt $stream.Length) { return $false }
        $null = $stream.Seek($offset, 'Begin')
        $signature = [byte[]]::new(4)
        if ($stream.Read($signature, 0, 4) -ne 4) { return $false }
        return $signature[0] -eq 0x50 -and $signature[1] -eq 0x45 -and $signature[2] -eq 0 -and $signature[3] -eq 0
    } finally {
        $stream.Dispose()
    }
}

function Get-PortableExecutable {
    <#
    .SYNOPSIS
        Lists the portable executables under a folder, whatever their extension.
    .PARAMETER Root
        The folder.
    .OUTPUTS
        System.String[]. Paths relative to Root, separated by /, sorted by ordinal.
    #>
    [CmdletBinding()]
    [OutputType([string[]])]
    param([Parameter(Mandatory)][string]$Root)
    $base = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
    $found = [System.Collections.Generic.List[string]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File -Force) {
        if (Test-PortableExecutable -Path $file.FullName) {
            $found.Add($file.FullName.Substring($base.Length + 1).Replace('\', '/'))
        }
    }
    $sorted = $found.ToArray()
    [System.Array]::Sort($sorted, [System.StringComparer]::Ordinal)
    return $sorted
}

$script:ManifestHeader = "# relative-path`tsigned-file`tsha256-before`tsha256-after"

function Write-SigningManifest {
    <#
    .SYNOPSIS
        Writes what was signed: one row per file.
    .PARAMETER Path
        The manifest file; UTF-8 without a byte order mark, LF line ends.
    .PARAMETER Rows
        Objects with RelativePath, SignedPath, Before and After (the SHA-256 of the file before and after signing).
    .OUTPUTS
        None.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Rows)
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add($script:ManifestHeader)
    foreach ($row in $Rows) {
        $lines.Add(($row.RelativePath, ([string]$row.SignedPath).Replace('\', '/'), ([string]$row.Before).ToLowerInvariant(), ([string]$row.After).ToLowerInvariant()) -join "`t")
    }
    $directory = Split-Path -Parent $Path
    if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
    [System.IO.File]::WriteAllText($Path, ($lines -join "`n") + "`n", [System.Text.UTF8Encoding]::new($false))
}

function Read-SigningManifest {
    <#
    .SYNOPSIS
        Reads a manifest Write-SigningManifest wrote.
    .PARAMETER Path
        The manifest file.
    .OUTPUTS
        System.Object[]. Rows with RelativePath, SignedPath, Before and After; throws, naming the line, on a row of
        another shape.
    #>
    [CmdletBinding()]
    [OutputType([object[]])]
    param([Parameter(Mandatory)][string]$Path)
    $rows = [System.Collections.Generic.List[object]]::new()
    $lines = [System.IO.File]::ReadAllText($Path).Split("`n")
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $line = $lines[$index]
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        $columns = $line.Split("`t")
        if ($columns.Count -ne 4) {
            throw "${Path}: line $($index + 1) has $($columns.Count) columns, not 4"
        }
        $rows.Add([pscustomobject]@{
                RelativePath = $columns[0]
                SignedPath   = $columns[1]
                Before       = $columns[2].ToLowerInvariant()
                After        = $columns[3].ToLowerInvariant()
            })
    }
    return $rows.ToArray()
}

function New-ThrowawayCodeSigningCertificate {
    <#
    .SYNOPSIS
        Makes a self-signed code-signing certificate for one run; no certificate store is touched.
    .PARAMETER Directory
        Where the .pfx goes.
    .OUTPUTS
        System.Management.Automation.PSCustomObject with PfxPath, Password and Thumbprint.
    #>
    [CmdletBinding()]
    [OutputType([pscustomobject])]
    param([Parameter(Mandatory)][string]$Directory)
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $rsa = [System.Security.Cryptography.RSA]::Create(3072)
    $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=Contour signing dry run (throwaway)', $rsa,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $request.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
    $usages = [System.Security.Cryptography.OidCollection]::new()
    $null = $usages.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
    $request.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($usages, $true))
    $now = [DateTimeOffset]::UtcNow
    $certificate = $request.CreateSelfSigned($now.AddMinutes(-5), $now.AddDays(1))
    $password = (New-Guid).ToString()
    $pfx = Join-Path $Directory "$((New-Guid).ToString('N')).pfx"
    [System.IO.File]::WriteAllBytes($pfx, $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $password))
    $thumbprint = $certificate.Thumbprint
    $certificate.Dispose()
    $rsa.Dispose()
    return [pscustomobject]@{ PfxPath = $pfx; Password = $password; Thumbprint = $thumbprint }
}

function Get-SignatureProblem {
    <#
    .SYNOPSIS
        Says what is wrong with the Authenticode signature of a file.
    .PARAMETER Path
        The file.
    .PARAMETER Trust
        System: Windows must trust the signature (status Valid). Pinned: the signature must be intact under a root
        Windows does not trust (a throwaway certificate's) and be made by Thumbprint.
    .PARAMETER Thumbprint
        For Pinned, the signer certificate's thumbprint.
    .OUTPUTS
        System.String. The problem, naming the status; $null when the signature is fine. Either way the file must be
        timestamped.
    #>
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][ValidateSet('System', 'Pinned')][string]$Trust,
        [string]$Thumbprint = ''
    )
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    $status = [string]$signature.Status
    if ($Trust -eq 'System') {
        if ($status -ne 'Valid') { return "$Path`: the signature status is $status, not Valid" }
    } else {
        # A self-signed root that Windows does not trust is reported as UnknownError for a signature that is intact
        # (observed with signtool and a throwaway certificate: signed and timestamped, Get-AuthenticodeSignature says
        # UnknownError; HashMismatch after a byte of the file changed, NotSigned without a signature).
        if ($status -ne 'UnknownError') { return "$Path`: the signature status is $status, not an intact signature under an untrusted root (UnknownError)" }
        $actual = if ($null -eq $signature.SignerCertificate) { '' } else { $signature.SignerCertificate.Thumbprint }
        if ($actual -ne $Thumbprint) { return "$Path`: signed by $actual, not by $Thumbprint" }
    }
    if ($null -eq $signature.TimeStamperCertificate) { return "$Path`: not timestamped" }
    return $null
}

function Select-SigningFile {
    <#
    .SYNOPSIS
        Chooses the files of a staged install tree a signer signs.
    .PARAMETER Root
        The staged tree.
    .PARAMETER Policy
        Own: the project's own binaries. Unsigned: every portable executable without a valid signature.
    .PARAMETER OwnBinaries
        The own binaries, relative to Root, separated by /. Each must exist, whatever the policy.
    .OUTPUTS
        System.String[]. Paths relative to Root, separated by /.
    #>
    [CmdletBinding()]
    [OutputType([string[]])]
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][ValidateSet('Own', 'Unsigned')][string]$Policy,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$OwnBinaries
    )
    foreach ($own in $OwnBinaries) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $own) -PathType Leaf)) {
            throw "$own not found under $Root; is this a staged install tree?"
        }
    }
    if ($Policy -eq 'Own') { return @($OwnBinaries) }
    $selected = [System.Collections.Generic.List[string]]::new()
    $odd = [System.Collections.Generic.List[string]]::new()
    foreach ($relative in Get-PortableExecutable -Root $Root) {
        $status = [string](Get-AuthenticodeSignature -LiteralPath (Join-Path $Root $relative)).Status
        # No valid signature: signtool replaces an existing one.
        if ($status -in 'NotSigned', 'UnknownError', 'NotTrusted') { $selected.Add($relative) }
        elseif ($status -ne 'Valid') { $odd.Add("$relative ($status)") }
    }
    if ($odd.Count -gt 0) { throw "files with a signature that is neither valid nor missing: $($odd -join ', ')" }
    if ($selected.Count -eq 0) {
        throw "nothing to sign under ${Root}: every PE file already carries a valid signature; stage afresh"
    }
    return $selected.ToArray()
}

function Resolve-SigningTool {
    <#
    .SYNOPSIS
        Finds a signing tool, downloading and verifying a pinned release if it has to.
    .PARAMETER Name
        The tool's key in the table.
    .PARAMETER Table
        The signer table.
    .PARAMETER ToolsDirectory
        Where downloaded tools are kept; a verified one is reused.
    .PARAMETER Download
        Fetches a URL to a file: { param($Url, $OutFile) ... }. Invoke-WebRequest by default.
    .OUTPUTS
        System.String. The path of the tool's executable.
    #>
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][hashtable]$Table,
        [Parameter(Mandatory)][string]$ToolsDirectory,
        [scriptblock]$Download = { param($Url, $OutFile) Invoke-WebRequest -Uri $Url -OutFile $OutFile }
    )
    if (-not $Table.Tools.ContainsKey($Name)) { throw "no tool '$Name' in the signer table" }
    $tool = $Table.Tools[$Name]
    switch ($tool.Source) {
        'WindowsSdk' {
            $command = Get-Command $tool.Executable -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($command) { return $command.Source }
            $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
            $found = @(Get-ChildItem -Path (Join-Path $kits "*/x64/$($tool.Executable)") -ErrorAction SilentlyContinue |
                    Sort-Object { try { [version]$_.Directory.Parent.Name } catch { [version]'0.0' } })
            if ($found.Count -eq 0) { throw "$($tool.Executable) is neither on PATH nor in the Windows SDK ($kits)" }
            return $found[-1].FullName
        }
        'Path' {
            $command = Get-Command $tool.Executable -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $command) { throw "$($tool.Executable) is not on PATH" }
            return $command.Source
        }
        'GitHubRelease' {
            $directory = Join-Path $ToolsDirectory "$Name-$($tool.Tag)"
            $marker = Join-Path $directory '.verified'
            if (-not (Test-Path -LiteralPath $marker)) {
                if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
                New-Item -ItemType Directory -Force -Path $ToolsDirectory | Out-Null
                $archive = Join-Path $ToolsDirectory "$Name-$($tool.Tag)-$($tool.Asset)"
                $url = "https://github.com/$($tool.Repository)/releases/download/$($tool.Tag)/$($tool.Asset)"
                & $Download $url $archive
                $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
                if ($actual -ne $tool.Sha256 -and $actual.ToLowerInvariant() -ne $tool.Sha256.ToLowerInvariant()) {
                    Remove-Item -LiteralPath $archive -Force
                    throw "$url has SHA-256 $($actual.ToLowerInvariant()), not the pinned $($tool.Sha256.ToLowerInvariant())"
                }
                Expand-Archive -LiteralPath $archive -DestinationPath $directory -Force
                Remove-Item -LiteralPath $archive -Force
                [System.IO.File]::WriteAllText($marker, $tool.Sha256)
            }
            $executable = Join-Path $directory $tool.Executable
            if (-not (Test-Path -LiteralPath $executable)) { throw "$($tool.Executable) is not in $($tool.Asset)" }
            return $executable
        }
    }
    throw "tool '$Name' has the unknown Source '$($tool.Source)'"
}

function Get-FileSha256 {
    # The SHA-256 of a file, as lowercase hex.
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-SigningStep {
    <#
    .SYNOPSIS
        Runs one signing step of a signer over files.
    .PARAMETER Root
        The folder the files are relative to.
    .PARAMETER Files
        The files, relative to Root, separated by /.
    .PARAMETER Step
        The step: Tool, Invocation (InPlace, InOut or Directory), Arguments, TimestampUrls.
    .PARAMETER Table
        The signer table (its tools, Attempts and RetryDelaySeconds).
    .PARAMETER ToolsDirectory
        Where downloaded tools are kept.
    .PARAMETER WorkDirectory
        Scratch space for the InOut and Directory shapes.
    .PARAMETER SecretName
        Environment variables whose values are hidden should a tool echo them.
    .OUTPUTS
        None. Throws, naming the tool and the exit code, when a call fails every attempt.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string[]]$Files,
        [Parameter(Mandatory)][hashtable]$Step,
        [Parameter(Mandatory)][hashtable]$Table,
        [Parameter(Mandatory)][string]$ToolsDirectory,
        [Parameter(Mandatory)][string]$WorkDirectory,
        [string[]]$SecretName = @()
    )
    $toolName = $Step.Tool
    $executable = Resolve-SigningTool -Name $toolName -Table $Table -ToolsDirectory $ToolsDirectory
    $template = @($Step.Arguments)
    $urls = @(if ($Step.ContainsKey('TimestampUrls')) { $Step.TimestampUrls })
    $secrets = @($SecretName | ForEach-Object { [Environment]::GetEnvironmentVariable($_) } | Where-Object { $_ })

    # The values that do not change from call to call: tools and data files named by the template.
    $fixed = @{}
    foreach ($argument in $template) {
        foreach ($match in [regex]::Matches($argument, '\{(tool|data):([^{}]+)\}')) {
            $key = "$($match.Groups[1].Value):$($match.Groups[2].Value)"
            $fixed[$key] = if ($match.Groups[1].Value -eq 'tool') {
                Resolve-SigningTool -Name $match.Groups[2].Value -Table $Table -ToolsDirectory $ToolsDirectory
            } else {
                Join-Path $PSScriptRoot $match.Groups[2].Value
            }
        }
    }

    # One call, retried: succeeds when the tool exits 0 and $Verify is happy.
    $call = {
        param([hashtable]$Values, [string]$Label, [scriptblock]$Verify)
        $attempts = [int]$Table.Attempts
        $delays = @($Table.RetryDelaySeconds)
        $tried = [System.Collections.Generic.List[string]]::new()
        $lastExit = 0
        for ($attempt = 0; $attempt -lt $attempts; $attempt++) {
            if ($attempt -gt 0 -and $delays.Count -gt 0) {
                Start-Sleep -Seconds $delays[[Math]::Min($attempt - 1, $delays.Count - 1)]
            }
            $all = $Values.Clone()
            foreach ($key in $fixed.Keys) { $all[$key] = $fixed[$key] }
            if ($urls.Count -gt 0) {
                $all['timestamp'] = $urls[$attempt % $urls.Count]
                $tried.Add($all['timestamp'])
            }
            $arguments = @(Expand-SigningArgument -Template $template -Values $all)
            Write-Host "  $toolName $($template -join ' ') [$Label] (attempt $($attempt + 1) of $attempts)"
            $output = & $executable @arguments 2>&1 | Out-String
            $lastExit = $LASTEXITCODE
            if ($lastExit -eq 0 -and (& $Verify)) { return }
            foreach ($secret in $secrets) { $output = $output.Replace($secret, '***') }
            Write-Host ($output.TrimEnd())
        }
        $servers = if ($tried.Count -gt 0) { "; timestamp servers tried: $($tried -join ', ')" } else { '' }
        throw "$toolName failed for $Label after $attempts attempts (exit code $lastExit)$servers"
    }

    $names = $Files | ForEach-Object { Split-Path -Leaf $_ }
    switch ($Step.Invocation) {
        'InPlace' {
            foreach ($relative in $Files) {
                $path = Join-Path $Root $relative
                & $call @{ file = $path } $relative { $true }
            }
        }
        'InOut' {
            $outDirectory = Join-Path $WorkDirectory 'out'
            foreach ($relative in $Files) {
                $path = Join-Path $Root $relative
                $out = Join-Path $outDirectory (Split-Path -Leaf $relative)
                New-Item -ItemType Directory -Force -Path $outDirectory | Out-Null
                if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
                & $call @{ in = $path; out = $out } $relative { Test-Path -LiteralPath $out -PathType Leaf }
                Move-Item -LiteralPath $out -Destination $path -Force
            }
        }
        'Directory' {
            $duplicate = $names | Group-Object | Where-Object { $_.Count -gt 1 } | Select-Object -First 1
            if ($duplicate) { throw "two files are named $($duplicate.Name); the Directory shape signs a flat folder" }
            $inputDir = Join-Path $WorkDirectory 'batch-in'
            $outputDir = Join-Path $WorkDirectory 'batch-out'
            foreach ($directory in $inputDir, $outputDir) {
                if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
                New-Item -ItemType Directory -Force -Path $directory | Out-Null
            }
            foreach ($relative in $Files) { Copy-Item -LiteralPath (Join-Path $Root $relative) -Destination (Join-Path $inputDir (Split-Path -Leaf $relative)) }
            & $call @{ inputDir = $inputDir; outputDir = $outputDir } "$($Files.Count) files" {
                @($names | Where-Object { -not (Test-Path -LiteralPath (Join-Path $outputDir $_) -PathType Leaf) }).Count -eq 0
            }
            foreach ($relative in $Files) {
                Copy-Item -LiteralPath (Join-Path $outputDir (Split-Path -Leaf $relative)) -Destination (Join-Path $Root $relative) -Force
            }
        }
    }
}

function Test-SigningSecret {
    # Throws, naming every one, unless each secret of the row is a non-empty environment variable.
    param([hashtable]$Row)
    $names = @(if ($Row.ContainsKey('Secrets')) { $Row.Secrets })
    $missing = @($names | Where-Object { [string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($_)) })
    if ($missing.Count -gt 0) { throw "the environment variable(s) $($missing -join ', ') are not set" }
}

function Get-RelativeFile {
    # The files under a folder, relative to it, separated by /.
    param([string]$Root)
    $base = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
    return @(Get-ChildItem -LiteralPath $base -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($base.Length + 1).Replace('\', '/') })
}

function Test-PathInside {
    # Whether Child is Parent or lies under it.
    param([string]$Parent, [string]$Child)
    $parentPath = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $childPath = [System.IO.Path]::GetFullPath($Child).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    return $childPath.StartsWith($parentPath, [StringComparison]::OrdinalIgnoreCase)
}

function Get-SigningSelection {
    # The files a batch signs, relative to Root, by the row's policy (Payload) or as given (Packages).
    param([string]$Root, [string]$Kind, [string[]]$Files, [hashtable]$Row, [hashtable]$Table)
    if ($Kind -eq 'Packages') {
        $base = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
        return @($Files | ForEach-Object {
                $full = (Resolve-Path -LiteralPath $_).Path
                if (-not (Test-PathInside $base $full)) { throw "$_ is not under $Root" }
                $full.Substring($base.Length + 1).Replace('\', '/')
            })
    }
    if ($Row.Transport -eq 'None') {
        return @(if ($Row.Payload -eq 'Own') { $Table.OwnBinaries } else { Get-PortableExecutable -Root $Root })
    }
    return @(Select-SigningFile -Root $Root -Policy $Row.Payload -OwnBinaries @($Table.OwnBinaries))
}

function Invoke-SignBatch {
    <#
    .SYNOPSIS
        Signs a batch of files with a signer and checks the result.
    .PARAMETER Root
        The staged tree (Payload) or the folder the packages are in (Packages).
    .PARAMETER Kind
        Payload: the files the signer's Payload policy selects under Root. Packages: Files.
    .PARAMETER Files
        For Packages, the package files.
    .PARAMETER Signer
        The signer's key in the table.
    .PARAMETER Table
        The signer table.
    .PARAMETER WorkDirectory
        Scratch space; for Payload it must not lie inside Root.
    .PARAMETER ToolsDirectory
        Where downloaded tools are kept.
    .PARAMETER Manifest
        A manifest file to write.
    .PARAMETER Trust
        System (a trusted certificate) or Pinned (a throwaway one, by Thumbprint).
    .PARAMETER Thumbprint
        For Pinned, the signer certificate's thumbprint.
    .OUTPUTS
        System.Object[]. Rows with RelativePath, SignedPath, Before and After.
    #>
    [CmdletBinding()]
    [OutputType([object[]])]
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][ValidateSet('Payload', 'Packages')][string]$Kind,
        [string[]]$Files = @(),
        [Parameter(Mandatory)][string]$Signer,
        [Parameter(Mandatory)][hashtable]$Table,
        [Parameter(Mandatory)][string]$WorkDirectory,
        [Parameter(Mandatory)][string]$ToolsDirectory,
        [string]$Manifest = '',
        [ValidateSet('System', 'Pinned')][string]$Trust = 'System',
        [string]$Thumbprint = ''
    )
    $row = $Table.Signers[$Signer]
    if ($Kind -eq 'Payload' -and (Test-PathInside $Root $WorkDirectory)) {
        throw "the work directory $WorkDirectory lies inside $Root, which would be signed with it"
    }
    if ($row.Transport -eq 'SignPath') { throw "the signer '$Signer' signs through SignPath: use export/import" }
    if ($row.Transport -eq 'Command') { Test-SigningSecret -Row $row }

    $selected = @(Get-SigningSelection -Root $Root -Kind $Kind -Files $Files -Row $row -Table $Table)
    $base = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
    # What must stay as it is: every file under Root for a payload, the packages for packages.
    $scope = if ($Kind -eq 'Payload') { Get-RelativeFile -Root $base } else { $selected }
    $before = @{}
    foreach ($relative in $scope) { $before[$relative] = Get-FileSha256 -Path (Join-Path $base $relative) }

    if ($row.Transport -eq 'Command') {
        $step = if ($Kind -eq 'Payload') { $row.PayloadStep } else { $row.PackageStep }
        New-Item -ItemType Directory -Force -Path $WorkDirectory | Out-Null
        Invoke-SigningStep -Root $base -Files $selected -Step $step -Table $Table -ToolsDirectory $ToolsDirectory `
            -WorkDirectory $WorkDirectory -SecretName @(if ($row.ContainsKey('Secrets')) { $row.Secrets })
    }

    $rows = [System.Collections.Generic.List[object]]::new()
    $problems = [System.Collections.Generic.List[string]]::new()
    $after = @{}
    foreach ($relative in $scope) {
        $path = Join-Path $base $relative
        if (-not (Test-Path -LiteralPath $path)) { $problems.Add("$relative vanished"); continue }
        $after[$relative] = Get-FileSha256 -Path $path
    }
    if ($Kind -eq 'Payload') {
        foreach ($relative in Get-RelativeFile -Root $base) {
            if (-not $before.ContainsKey($relative)) { $problems.Add("$relative appeared under the signed tree") }
        }
    }
    foreach ($relative in $scope) {
        if (-not $after.ContainsKey($relative)) { continue }
        $signed = $relative -in $selected
        if ($row.Transport -eq 'Command') {
            if ($signed) {
                if ($after[$relative] -eq $before[$relative]) { $problems.Add("$relative is unchanged: it was not signed") }
                elseif ($detail = Get-SignatureProblem -Path (Join-Path $base $relative) -Trust $Trust -Thumbprint $Thumbprint) { $problems.Add($detail) }
            } elseif ($after[$relative] -ne $before[$relative]) {
                $problems.Add("$relative changed, and is not a file the signer was asked to sign")
            }
        }
    }
    if ($problems.Count -gt 0) { throw "signing $Kind under ${Root} went wrong:`n  $($problems -join "`n  ")" }
    foreach ($relative in $selected) {
        $rows.Add([pscustomobject]@{
                RelativePath = $relative
                SignedPath   = ((Join-Path $base $relative).Replace('\', '/'))
                Before       = $before[$relative]
                After        = $after[$relative]
            })
    }
    if ($Manifest) { Write-SigningManifest -Path $Manifest -Rows $rows.ToArray() }
    return $rows.ToArray()
}

function Export-SigningBatch {
    <#
    .SYNOPSIS
        Copies the files a signer would sign out of a tree, for a signer that signs elsewhere (SignPath).
    .PARAMETER Root
        The staged tree or the folder of packages.
    .PARAMETER Kind
        Payload or Packages.
    .PARAMETER Files
        For Packages, the package files.
    .PARAMETER Signer
        The signer's key in the table.
    .PARAMETER Table
        The signer table.
    .PARAMETER Destination
        Where the copies go, as Destination/<relative path>.
    .PARAMETER Manifest
        The manifest to write, with After empty.
    .OUTPUTS
        System.Object[]. The manifest rows.
    #>
    [CmdletBinding()]
    [OutputType([object[]])]
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][ValidateSet('Payload', 'Packages')][string]$Kind,
        [string[]]$Files = @(),
        [Parameter(Mandatory)][string]$Signer,
        [Parameter(Mandatory)][hashtable]$Table,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Manifest
    )
    $row = $Table.Signers[$Signer]
    $base = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
    $selected = @(Get-SigningSelection -Root $base -Kind $Kind -Files $Files -Row $row -Table $Table)
    $rows = foreach ($relative in $selected) {
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $base $relative) -Destination $target -Force
        [pscustomobject]@{ RelativePath = $relative; SignedPath = $target.Replace('\', '/'); Before = (Get-FileSha256 -Path (Join-Path $base $relative)); After = '' }
    }
    Write-SigningManifest -Path $Manifest -Rows @($rows)
    return @($rows)
}

function Import-SigningBatch {
    <#
    .SYNOPSIS
        Puts the files a signer signed elsewhere back into the tree, after checking them.
    .PARAMETER Root
        The staged tree or the folder of packages the batch was exported from.
    .PARAMETER From
        Where the signed copies are, as From/<relative path>.
    .PARAMETER Manifest
        The manifest Export-SigningBatch wrote; completed with the hashes after.
    .PARAMETER Trust
        System or Pinned, as for Get-SignatureProblem.
    .PARAMETER Thumbprint
        For Pinned, the signer certificate's thumbprint.
    .OUTPUTS
        System.Object[]. The completed manifest rows.
    #>
    [CmdletBinding()]
    [OutputType([object[]])]
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$From,
        [Parameter(Mandatory)][string]$Manifest,
        [ValidateSet('System', 'Pinned')][string]$Trust = 'System',
        [string]$Thumbprint = ''
    )
    $base = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
    $rows = @(Read-SigningManifest -Path $Manifest)
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($row in $rows) {
        $signed = Join-Path $From $row.RelativePath
        $current = Join-Path $base $row.RelativePath
        if (-not (Test-Path -LiteralPath $signed)) { $problems.Add("$($row.RelativePath): no signed copy under $From"); continue }
        if (-not (Test-Path -LiteralPath $current) -or (Get-FileSha256 -Path $current) -ne $row.Before) {
            $problems.Add("$($row.RelativePath): the file under $base is not the one that was exported")
            continue
        }
        if ($detail = Get-SignatureProblem -Path $signed -Trust $Trust -Thumbprint $Thumbprint) { $problems.Add($detail) }
    }
    if ($problems.Count -gt 0) { throw "the signed files do not fit the tree:`n  $($problems -join "`n  ")" }
    $completed = foreach ($row in $rows) {
        $signed = Join-Path $From $row.RelativePath
        Copy-Item -LiteralPath $signed -Destination (Join-Path $base $row.RelativePath) -Force
        [pscustomobject]@{
            RelativePath = $row.RelativePath
            SignedPath   = (Join-Path $base $row.RelativePath).Replace('\', '/')
            Before       = $row.Before
            After        = (Get-FileSha256 -Path (Join-Path $base $row.RelativePath))
        }
    }
    Write-SigningManifest -Path $Manifest -Rows @($completed)
    return @($completed)
}

function Get-CPackComponent {
    <#
    .SYNOPSIS
        Reads the components CPack packages from a generated CPackConfig.cmake.
    .PARAMETER CPackConfig
        The path of CPackConfig.cmake.
    .OUTPUTS
        System.String[]. The CPACK_COMPONENTS_ALL list; empty when the file sets none.
    #>
    [CmdletBinding()]
    [OutputType([string[]])]
    param([Parameter(Mandatory)][string]$CPackConfig)
    $text = [System.IO.File]::ReadAllText($CPackConfig)
    $match = [regex]::Match($text, '(?m)^\s*set\(\s*CPACK_COMPONENTS_ALL\s+"([^"]*)"\s*\)')
    if (-not $match.Success) { return @() }
    return @($match.Groups[1].Value.Split(';') | Where-Object { $_ })
}

function Invoke-Staging {
    <#
    .SYNOPSIS
        Installs a build into a folder the way CPack would: the same components, nothing else.
    .PARAMETER Build
        The build directory (holding CPackConfig.cmake).
    .PARAMETER Destination
        The folder to install into; recreated.
    .OUTPUTS
        None. A component CPack does not package must not be staged: a file of it would be signed, and then fail to
        overlay the package.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Destination)
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $components = @(Get-CPackComponent -CPackConfig (Join-Path $Build 'CPackConfig.cmake'))
    # One install per component CPack packages; a plain install where it names none.
    $targets = if ($components.Count -eq 0) { @('') } else { $components }
    foreach ($component in $targets) {
        $extra = if ($component) { @('--component', $component) } else { @() }
        Write-Host "cmake --install $Build --prefix $Destination $($extra -join ' ')"
        & cmake --install $Build --prefix $Destination @extra | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cmake --install failed (exit code $LASTEXITCODE) $($extra -join ' ')" }
    }
}

function Invoke-SigningDryRun {
    <#
    .SYNOPSIS
        Signs a copy of an install tree and its packages with a throwaway certificate, to see that signing would work.
    .PARAMETER Root
        The staged install tree; it is only read.
    .PARAMETER Packages
        Installers to sign as well; copied, never changed.
    .PARAMETER Table
        The signer table.
    .PARAMETER WorkDirectory
        Recreated; must neither lie inside Root nor contain it.
    .PARAMETER ToolsDirectory
        Where downloaded tools are kept.
    .OUTPUTS
        None. Prints a summary line; throws on any problem.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Root,
        [string[]]$Packages = @(),
        [Parameter(Mandatory)][hashtable]$Table,
        [Parameter(Mandatory)][string]$WorkDirectory,
        [Parameter(Mandatory)][string]$ToolsDirectory
    )
    if ((Test-PathInside $Root $WorkDirectory) -or (Test-PathInside $WorkDirectory $Root)) {
        throw "the work directory $WorkDirectory and the tree $Root must not contain one another"
    }
    foreach ($own in $Table.OwnBinaries) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $own) -PathType Leaf)) {
            throw "$own not found under $Root; is this a staged install tree?"
        }
    }
    if (Test-Path -LiteralPath $WorkDirectory) { Remove-Item -LiteralPath $WorkDirectory -Recurse -Force }
    $stage = Join-Path $WorkDirectory 'stage'
    $packageDirectory = Join-Path $WorkDirectory 'packages'
    $certificateDirectory = Join-Path $WorkDirectory 'cert'
    New-Item -ItemType Directory -Force -Path $stage, $packageDirectory | Out-Null
    try {
        Copy-Item -Path (Join-Path (Resolve-Path -LiteralPath $Root).Path '*') -Destination $stage -Recurse -Force
        $copies = foreach ($package in $Packages) {
            $copy = Join-Path $packageDirectory (Split-Path -Leaf $package)
            Copy-Item -LiteralPath $package -Destination $copy
            $copy
        }
        $certificate = New-ThrowawayCodeSigningCertificate -Directory $certificateDirectory
        $env:CONTOUR_SIGNING_PFX = $certificate.PfxPath
        $env:CONTOUR_SIGNING_PFX_PASSWORD = $certificate.Password

        $payload = @(Invoke-SignBatch -Root $stage -Kind Payload -Signer dryrun -Table $Table -WorkDirectory (Join-Path $WorkDirectory 'payload-work') `
                -ToolsDirectory $ToolsDirectory -Trust Pinned -Thumbprint $certificate.Thumbprint)
        $packaged = @()
        if (@($copies).Count -gt 0) {
            $packaged = @(Invoke-SignBatch -Root $packageDirectory -Kind Packages -Files @($copies) -Signer dryrun -Table $Table `
                    -WorkDirectory (Join-Path $WorkDirectory 'package-work') -ToolsDirectory $ToolsDirectory -Trust Pinned -Thumbprint $certificate.Thumbprint)
        }

        $signed = @($payload | ForEach-Object { $_.RelativePath })
        $missing = @($Table.OwnBinaries | Where-Object { $_ -notin $signed })
        if ($missing.Count -gt 0) { throw "own binaries missing from the payload selection: $($missing -join ', ')" }
        $unsigned = @(Get-PortableExecutable -Root $stage | Where-Object {
                [string](Get-AuthenticodeSignature -LiteralPath (Join-Path $stage $_)).Status -eq 'NotSigned'
            })
        if ($unsigned.Count -gt 0) { throw "files that should carry a signature do not: $($unsigned -join ', ')" }
        Write-Host "signing dry run: $($payload.Count) payload files and $($packaged.Count) packages signed and verified with a throwaway certificate"
    } finally {
        Remove-Item Env:CONTOUR_SIGNING_PFX, Env:CONTOUR_SIGNING_PFX_PASSWORD -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $certificateDirectory) { Remove-Item -LiteralPath $certificateDirectory -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

function Test-ShippedArtifact {
    <#
    .SYNOPSIS
        Checks that the packages are what a signer should ship.
    .PARAMETER Signer
        The signer's key in the table.
    .PARAMETER Table
        The signer table.
    .PARAMETER Manifest
        The manifest of the signed payload.
    .PARAMETER Zip
        The ZIP package.
    .PARAMETER Msi
        The installers.
    .PARAMETER WorkDirectory
        Where the ZIP is extracted (recreated).
    .OUTPUTS
        None. Prints a summary line; throws, listing every problem, otherwise.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Signer,
        [Parameter(Mandatory)][hashtable]$Table,
        [Parameter(Mandatory)][string]$Manifest,
        [Parameter(Mandatory)][string]$Zip,
        [Parameter(Mandatory)][string[]]$Msi,
        [Parameter(Mandatory)][string]$WorkDirectory
    )
    $row = $Table.Signers[$Signer]
    $transport = ([string]$row.Transport).ToLowerInvariant()
    $extracted = Join-Path $WorkDirectory 'zip'
    if (Test-Path -LiteralPath $extracted) { Remove-Item -LiteralPath $extracted -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $extracted | Out-Null
    Expand-Archive -LiteralPath $Zip -DestinationPath $extracted -Force
    $files = @(Get-RelativeFile -Root $extracted)
    $problems = [System.Collections.Generic.List[string]]::new()

    # The files of the manifest are the ones packaged, as signed.
    foreach ($entry in Read-SigningManifest -Path $Manifest) {
        $hits = @($files | Where-Object { $_ -eq $entry.RelativePath -or $_.EndsWith('/' + $entry.RelativePath) })
        if ($hits.Count -eq 0) { $problems.Add("$($entry.RelativePath): no such entry in the ZIP"); continue }
        foreach ($name in $hits) {
            $path = Join-Path $extracted $name
            if ((Get-FileSha256 -Path $path) -ne $entry.After) { $problems.Add("${name}: is not the signed file of the manifest") }
            elseif ($transport -ne 'none' -and ($detail = Get-SignatureProblem -Path $path -Trust System)) { $problems.Add($detail) }
        }
    }

    # Nothing signed with a certificate Windows does not trust (a dry run's) may ship; a file with no signature may.
    $executables = @(Get-PortableExecutable -Root $extracted)
    foreach ($relative in $executables) {
        $status = [string](Get-AuthenticodeSignature -LiteralPath (Join-Path $extracted $relative)).Status
        if ($status -notin 'Valid', 'NotSigned') { $problems.Add("${relative}: the ZIP ships a file whose signature status is $status") }
        elseif ($status -eq 'NotSigned' -and $transport -ne 'none' -and $row.Payload -eq 'Unsigned') {
            $problems.Add("${relative}: the ZIP ships a file without a signature, which the $Signer signer signs")
        }
    }

    foreach ($installer in $Msi) {
        if ($transport -eq 'none') {
            $status = [string](Get-AuthenticodeSignature -LiteralPath $installer).Status
            if ($status -ne 'NotSigned') { $problems.Add("${installer}: the signature status is $status; the none signer ships unsigned installers") }
        } elseif ($detail = Get-SignatureProblem -Path $installer -Trust System) {
            $problems.Add($detail)
        }
    }
    if ($problems.Count -gt 0) { throw "the packages are not what the $Signer signer should ship:`n  $($problems -join "`n  ")" }
    Write-Host "verify: $Zip and $(@($Msi).Count) installers are what the $Signer signer should ship"
}

Export-ModuleMember -Function Test-SigningTable, Import-SigningTable, Resolve-SigningPlan, Expand-SigningArgument,
Get-PortableExecutable, Write-SigningManifest, Read-SigningManifest, New-ThrowawayCodeSigningCertificate,
Get-SignatureProblem, Select-SigningFile, Resolve-SigningTool, Invoke-SigningStep, Invoke-SignBatch,
Export-SigningBatch, Import-SigningBatch, Get-FileSha256, Get-CPackComponent, Invoke-Staging, Invoke-SigningDryRun,
Test-ShippedArtifact
