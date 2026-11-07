# The decisions of Windows code signing that need no signer and no network: what the signer table may say, which
# signer a build uses, how a command line is filled in, which files are portable executables, and the manifest of
# what was signed. The table is scripts/windows-signing/signers.psd1.

Set-StrictMode -Version Latest
# A failing cmdlet ends the call whoever calls it: the CLI, a workflow step (which sets this itself) or an owner's session.
$ErrorActionPreference = 'Stop'

# The functions that return lists return them unrolled, as PowerShell does: a caller wraps the call in @(...), or its
# `.Count` is an error under strict mode for an empty result and for a single one.

# The values a field of the table may have.
$script:Transports = 'None', 'SignPath', 'Command'
$script:Payloads = 'Own', 'Unsigned'
$script:BuildSettings = 'releases', 'releases-and-master'
$script:BuildKinds = 'release', 'master', 'other'
$script:Invocations = 'InPlace', 'InOut', 'Directory'
$script:ToolSources = 'WindowsSdk', 'Path', 'GitHubRelease'
# What each invocation shape has to refer to its files, besides {timestamp} and the references that hold for all.
$script:InvocationPlaceholders = @{ InPlace = @('file'); InOut = @('in', 'out'); Directory = @('inputDir', 'outputDir') }
# The options of the known tools that timestamp the legacy way (a PKCS#9 countersignature, not RFC 3161).
$script:LegacyTimestampOptions = '/t', '-t', '-timestamp'

function Get-TableValue {
    # A key of a hashtable, or $null where it has none (strict mode throws on a missing property, not on a key).
    param($Hashtable, [string]$Key)
    if ($Hashtable -is [hashtable] -and $Hashtable.ContainsKey($Key)) { return $Hashtable[$Key] }
    return $null
}

function Get-FullPath {
    # A path as the PowerShell location reads it, as a full path. The .NET calls resolve a relative path against the
    # process's directory instead, which Set-Location does not change: every path is made full where it enters.
    param([string]$Path)
    if ([string]::IsNullOrEmpty($Path)) { return $Path }
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Get-Placeholder {
    # The names of the {placeholders} in some strings, in order.
    param([string[]]$Text)
    return @($Text | ForEach-Object { if ($_ -is [string]) { [regex]::Matches($_, '\{([^{}]+)\}') | ForEach-Object { $_.Groups[1].Value } } })
}

function Get-ToolReference {
    # The names of the tools {tool:X} and {toolDir:X} point at in some strings.
    param([string[]]$Text)
    return @(Get-Placeholder -Text $Text | ForEach-Object { if ($_ -match '^tool(?:Dir)?:(.+)$') { $Matches[1] } })
}

function Test-SigningStep {
    # The problems of one step of a signer.
    param([string]$Where, $Step, [hashtable]$Tools, [string[]]$Secrets, [string]$DataDirectory)
    $problems = [System.Collections.Generic.List[string]]::new()
    $tool = Get-TableValue $Step 'Tool'
    if ($tool -isnot [string] -or -not $Tools.ContainsKey($tool)) {
        $problems.Add("$Where`: Tool '$tool' is not one of the Tools")
    }
    $invocation = Get-TableValue $Step 'Invocation'
    if ($invocation -notin $script:Invocations) {
        $problems.Add("$Where`: Invocation '$invocation' is not one of $($script:Invocations -join ', ')")
    }
    $rawArguments = Get-TableValue $Step 'Arguments'
    if ($null -eq $rawArguments) {
        $problems.Add("$Where`: Arguments is missing")
        return , $problems
    }
    $arguments = @($rawArguments | Where-Object { $_ -is [string] })
    $usesTimestamp = 'timestamp' -in (Get-Placeholder -Text $arguments)
    $urls = Get-TableValue $Step 'TimestampUrls'
    $timestampMode = Get-TableValue $Step 'Timestamp'
    if ($usesTimestamp -and ($null -eq $urls -or @($urls).Count -eq 0)) {
        $problems.Add("$Where`: Arguments use {timestamp} and there are no TimestampUrls")
    } elseif (-not $usesTimestamp -and $timestampMode -ne 'Tool') {
        $problems.Add("$Where`: Arguments have no {timestamp}: either rotate the timestamp servers, or say Timestamp = 'Tool' for a tool that timestamps by itself")
    }
    foreach ($argument in $arguments) {
        if ($argument -in $script:LegacyTimestampOptions) {
            $problems.Add("$Where`: '$argument' timestamps the legacy way, not RFC 3161 (use /tr or -ts)")
        }
    }
    $shapeKnown = $script:InvocationPlaceholders.ContainsKey([string]$invocation)
    $shapeNames = @($(if ($shapeKnown) { $script:InvocationPlaceholders[[string]$invocation] }))
    foreach ($name in @(Get-Placeholder -Text $arguments | Select-Object -Unique)) {
        # A step of an invocation the table does not know has already been reported; its placeholders are not judged.
        if (-not $shapeKnown -and $name -notmatch '^(env|tool|toolDir|data):') { continue }
        if ($name -eq 'timestamp' -or $name -in $shapeNames -or $name -in 'product:name', 'product:url') { continue }
        if ($name -match '^env:(.+)$') {
            if ($Matches[1] -notin $Secrets) { $problems.Add("$Where`: {$name} is not among the signer's Secrets, so it would neither be checked up front nor masked") }
        } elseif ($name -match '^tool(Dir)?:(.+)$') {
            if (-not $Tools.ContainsKey($Matches[2])) { $problems.Add("$Where`: {$name} names no tool") }
        } elseif ($name -match '^data:(.+)$') {
            if (-not (Test-Path -LiteralPath (Join-Path $DataDirectory $Matches[1]) -PathType Leaf)) { $problems.Add("$Where`: {$name} names no file beside the module") }
        } else {
            $problems.Add("$Where`: the placeholder {$name} does not exist for a step of invocation '$invocation'")
        }
    }
    return , $problems
}

function Test-SigningTool {
    # The problems of one tool of the table.
    param([string]$Where, $Tool)
    $problems = [System.Collections.Generic.List[string]]::new()
    $source = Get-TableValue $Tool 'Source'
    if ($source -notin $script:ToolSources) {
        $problems.Add("$Where`: Source '$source' is not one of $($script:ToolSources -join ', ')")
        return , $problems
    }
    $executable = Get-TableValue $Tool 'Executable'
    if ($executable -isnot [string] -or $executable -eq '') {
        $problems.Add("$Where`: Executable is missing")
    } else {
        if ($source -eq 'GitHubRelease' -and $executable -match '[<>]') {
            $problems.Add("$Where`: Executable '$executable' is a placeholder, not a path")
        }
        if ($executable -match '\.(bat|cmd)$') {
            $problems.Add("$Where`: Executable '$executable' is a batch file, which cmd.exe would run and parse the arguments (secrets among them) again")
        }
    }
    if ($source -eq 'GitHubRelease') {
        foreach ($field in 'Repository', 'Tag', 'Asset') {
            $value = Get-TableValue $Tool $field
            if ($value -isnot [string] -or $value -eq '') { $problems.Add("$Where`: $field is missing") }
        }
        $hash = Get-TableValue $Tool 'Sha256'
        if ($hash -isnot [string] -or $hash -notmatch '^[0-9a-fA-F]{64}$') {
            $problems.Add("$Where`: Sha256 '$hash' is not 64 hexadecimal digits")
        }
    }
    # A program that starts says so: a probe runs it the way a signing run does. A library has none.
    $probe = Get-TableValue $Tool 'ProbeArguments'
    if ($executable -is [string] -and $executable -notmatch '\.dll$' -and ($null -eq $probe -or @($probe).Count -eq 0)) {
        $problems.Add("$Where`: ProbeArguments is missing (a program of the table says how to see it start)")
    }
    foreach ($name in @(Get-Placeholder -Text @($probe))) {
        if ($name -ne 'toolDir') { $problems.Add("$Where`: the probe has the placeholder {$name}; only {toolDir} exists there") }
    }
    $environment = Get-TableValue $Tool 'Environment'
    if ($null -ne $environment -and ($environment -isnot [hashtable] -or @($environment.Values | Where-Object { $_ -isnot [string] }).Count -gt 0)) {
        $problems.Add("$Where`: Environment is not a hashtable of strings")
    }
    foreach ($name in @(Get-Placeholder -Text @($(if ($environment -is [hashtable]) { $environment.Values })))) {
        if ($name -ne 'toolDir') { $problems.Add("$Where`: the Environment has the placeholder {$name}; only {toolDir} exists there") }
    }
    $cleanup = Get-TableValue $Tool 'Cleanup'
    if ($null -ne $cleanup -and @($cleanup | Where-Object { $_ -isnot [string] }).Count -gt 0) {
        $problems.Add("$Where`: Cleanup is not a list of strings")
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

    $attempts = Get-TableValue $Table 'Attempts'
    if ($attempts -isnot [int] -or $attempts -lt 1) { $problems.Add("Attempts '$attempts' is not a whole number of at least 1") }
    $delays = Get-TableValue $Table 'RetryDelaySeconds'
    if ($null -eq $delays -or @($delays | Where-Object { $_ -isnot [int] -or $_ -lt 0 }).Count -gt 0) {
        $problems.Add('RetryDelaySeconds is missing or not a list of whole numbers of seconds')
    }
    $own = Get-TableValue $Table 'OwnBinaries'
    if ($null -eq $own -or @($own).Count -eq 0 -or @($own | Where-Object { $_ -isnot [string] }).Count -gt 0) {
        $problems.Add('OwnBinaries is missing or not a list of paths')
    }
    $product = Get-TableValue $Table 'Product'
    foreach ($field in 'Name', 'Url') {
        $value = Get-TableValue $product $field
        if ($value -isnot [string] -or $value -eq '') { $problems.Add("Product.$field is missing") }
    }

    $tools = Get-TableValue $Table 'Tools'
    if ($tools -isnot [hashtable]) { $tools = @{} }
    foreach ($name in @($tools.Keys | Sort-Object)) {
        foreach ($problem in (Test-SigningTool -Where "tool '$name'" -Tool $tools[$name])) { $problems.Add($problem) }
    }

    $signers = Get-TableValue $Table 'Signers'
    if ($signers -isnot [hashtable]) { $signers = @{} }
    if (-not $signers.ContainsKey('none')) { $problems.Add("there is no 'none' signer: a build with nothing configured has none to take") }
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
        $allowed = Get-TableValue $signer 'AllowedBuilds'
        if ($signer -is [hashtable] -and -not $signer.ContainsKey('AllowedBuilds')) {
            $problems.Add("$where`: AllowedBuilds is missing (an empty list marks a signer for local use)")
        }
        foreach ($build in @($allowed) | Where-Object { $null -ne $_ }) {
            if ($build -notin $script:BuildSettings) {
                $problems.Add("$where`: AllowedBuilds names '$build', not one of $($script:BuildSettings -join ', ')")
            }
        }
        $publisher = Get-TableValue $signer 'Publisher'
        if ($publisher -isnot [string]) {
            $problems.Add("$where`: Publisher is missing (an empty string for a signer whose certificate is not checked)")
        } elseif ($publisher -eq '' -and $transport -ne 'None' -and @($allowed | Where-Object { $null -ne $_ }).Count -gt 0) {
            $problems.Add("$where`: a signer CI can select needs a Publisher, the subject its certificate must have")
        }
        $secrets = @(Get-TableValue $signer 'Secrets' | Where-Object { $null -ne $_ })
        if ($transport -eq 'Command') {
            foreach ($stepName in 'PayloadStep', 'PackageStep') {
                $step = Get-TableValue $signer $stepName
                if ($step -isnot [hashtable]) {
                    $problems.Add("$where`: a Command signer needs a $stepName")
                    continue
                }
                foreach ($problem in (Test-SigningStep -Where "$where $stepName" -Step $step -Tools $tools -Secrets $secrets -DataDirectory $PSScriptRoot)) {
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
    $Path = Get-FullPath $Path
    $table = Import-PowerShellDataFile -LiteralPath $Path
    $problems = @(Test-SigningTable -Table $table)
    if ($problems.Count -gt 0) {
        throw "$Path is not a valid signer table:`n  $($problems -join "`n  ")"
    }
    return $table
}

function Get-SignerRow {
    # The row of a signer, or a throw that names it.
    param([hashtable]$Table, [string]$Signer)
    if (-not $Table.Signers.ContainsKey($Signer)) {
        throw "no signer '$Signer' in the table (it has $(@($Table.Signers.Keys | Sort-Object) -join ', '))"
    }
    return $Table.Signers[$Signer]
}

function Resolve-SigningPlan {
    <#
    .SYNOPSIS
        Decides which signer a build uses and how.
    .PARAMETER Table
        The signer table.
    .PARAMETER Configured
        The signer the repository is configured for. Empty means none.
    .PARAMETER Builds
        Which builds sign: releases or releases-and-master. Empty means releases.
    .PARAMETER Build
        What kind of build this is: release (a release tag), master (a push to master) or other.
    .OUTPUTS
        System.Management.Automation.PSCustomObject with Signer (the effective one), Transport (lowercase none, signpath
        or command) and DryRun (a build that signs nothing, but is to take the whole path with a throwaway certificate).
    #>
    [CmdletBinding()]
    [OutputType([pscustomobject])]
    param(
        [Parameter(Mandatory)][hashtable]$Table,
        [AllowEmptyString()][Parameter(Mandatory)][string]$Configured,
        [AllowEmptyString()][Parameter(Mandatory)][string]$Builds,
        [Parameter(Mandatory)][string]$Build
    )
    if ($Configured -eq '') { $Configured = 'none' }
    if ($Builds -eq '') { $Builds = 'releases' }
    if ($Builds -notin $script:BuildSettings) {
        throw "the builds setting '$Builds' is not one of $($script:BuildSettings -join ', ')"
    }
    if ($Build -notin $script:BuildKinds) {
        throw "the kind of build '$Build' is not one of $($script:BuildKinds -join ', ')"
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
    # A release signs; a master push signs when the repository says so; anything else never does.
    $signs = $Build -eq 'release' -or ($Build -eq 'master' -and $Builds -eq 'releases-and-master')
    $signer = if ($signs) { $Configured } else { 'none' }
    return [pscustomobject]@{
        Signer    = $signer
        Transport = ([string]$signers[$signer].Transport).ToLowerInvariant()
        DryRun    = [bool]($signer -eq 'none')
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

function Get-RelativeFile {
    # The files under a folder, relative to it, separated by /, sorted by ordinal.
    param([string]$Root)
    $base = (Get-FullPath $Root).TrimEnd('\', '/')
    $found = [string[]]@(Get-ChildItem -LiteralPath $base -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($base.Length + 1).Replace('\', '/') })
    [System.Array]::Sort($found, [System.StringComparer]::Ordinal)
    return $found
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
    $base = (Get-FullPath $Root).TrimEnd('\', '/')
    return @(Get-RelativeFile -Root $base | Where-Object { Test-PortableExecutable -Path (Join-Path $base $_) })
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
    $Path = Get-FullPath $Path
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
    $Path = Get-FullPath $Path
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

$script:KeyStorageProvider = 'Microsoft Software Key Storage Provider'

function New-ThrowawayCodeSigningCertificate {
    <#
    .SYNOPSIS
        Makes a self-signed code-signing certificate for one run; no certificate store is touched.
    .DESCRIPTION
        The key lives in a named container of the Microsoft Software Key Storage Provider, so that
        Remove-ThrowawayCodeSigningCertificate can delete it. A key imported from a .pfx by signtool would stay in the
        user's profile (%APPDATA%\Microsoft\Crypto) for ever. Only the certificate is written out.
    .PARAMETER Directory
        Where the certificate (.cer) goes.
    .OUTPUTS
        System.Management.Automation.PSCustomObject with CertificatePath, KeyName and Thumbprint.
    #>
    [CmdletBinding()]
    [OutputType([pscustomobject])]
    param([Parameter(Mandatory)][string]$Directory)
    $Directory = Get-FullPath $Directory
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $keyName = "contour-signing-throwaway-$((New-Guid).ToString('N'))"
    $parameters = [System.Security.Cryptography.CngKeyCreationParameters]::new()
    $parameters.Provider = [System.Security.Cryptography.CngProvider]::new($script:KeyStorageProvider)
    $parameters.KeyUsage = [System.Security.Cryptography.CngKeyUsages]::Signing
    $parameters.Parameters.Add([System.Security.Cryptography.CngProperty]::new('Length', [BitConverter]::GetBytes(3072), [System.Security.Cryptography.CngPropertyOptions]::None))
    $key = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::Rsa, $keyName, $parameters)
    try {
        $rsa = [System.Security.Cryptography.RSACng]::new($key)
        try {
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
        } finally {
            $rsa.Dispose()
        }
        $path = Join-Path $Directory "$keyName.cer"
        [System.IO.File]::WriteAllBytes($path, $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
        $thumbprint = $certificate.Thumbprint
        $certificate.Dispose()
    } catch {
        $key.Delete()
        throw
    } finally {
        $key.Dispose()
    }
    return [pscustomobject]@{ CertificatePath = $path; KeyName = $keyName; Thumbprint = $thumbprint }
}

function Remove-ThrowawayCodeSigningCertificate {
    <#
    .SYNOPSIS
        Deletes the key (and the certificate file) New-ThrowawayCodeSigningCertificate made.
    .PARAMETER Certificate
        What New-ThrowawayCodeSigningCertificate returned.
    .OUTPUTS
        None.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Certificate)
    if ([System.Security.Cryptography.CngKey]::Exists($Certificate.KeyName, [System.Security.Cryptography.CngProvider]::new($script:KeyStorageProvider))) {
        $key = [System.Security.Cryptography.CngKey]::Open($Certificate.KeyName, [System.Security.Cryptography.CngProvider]::new($script:KeyStorageProvider))
        try { $key.Delete() } finally { $key.Dispose() }
    }
    Remove-Item -LiteralPath $Certificate.CertificatePath -Force -ErrorAction SilentlyContinue
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
    .PARAMETER Publisher
        For System, a -like pattern the signer certificate's subject must match; empty accepts any.
    .OUTPUTS
        System.String. The problem, naming the status; $null when the signature is fine. Either way the file must be
        timestamped.
    #>
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][ValidateSet('System', 'Pinned')][string]$Trust,
        [string]$Thumbprint = '',
        [string]$Publisher = ''
    )
    $Path = Get-FullPath $Path
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    $status = [string]$signature.Status
    if ($Trust -eq 'System') {
        if ($status -ne 'Valid') { return "$Path`: the signature status is $status, not Valid" }
        $subject = if ($null -eq $signature.SignerCertificate) { '' } else { $signature.SignerCertificate.Subject }
        if ($Publisher -ne '' -and $subject -notlike $Publisher) { return "$Path`: signed by '$subject', not by a publisher matching '$Publisher'" }
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

function Assert-OwnBinary {
    # Throws unless each own binary exists under a staged tree.
    param([string]$Root, [string[]]$OwnBinaries)
    foreach ($own in $OwnBinaries) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $own) -PathType Leaf)) {
            throw "$own not found under $Root; is this a staged install tree?"
        }
    }
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
    $Root = Get-FullPath $Root
    Assert-OwnBinary -Root $Root -OwnBinaries $OwnBinaries
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

function Get-FileSha256 {
    # The SHA-256 of a file, as lowercase hex.
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
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
        Where downloaded tools are kept; a verified one is reused, after its executable is hashed again.
    .PARAMETER Download
        Fetches a URL to a file: { param($Url, $OutFile) ... }. Invoke-WebRequest, retried, by default.
    .OUTPUTS
        System.String. The path of the tool's executable.
    #>
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][hashtable]$Table,
        [Parameter(Mandatory)][string]$ToolsDirectory,
        [scriptblock]$Download = { param($Url, $OutFile) Invoke-WebRequest -Uri $Url -OutFile $OutFile -MaximumRetryCount 3 -RetryIntervalSec 5 }
    )
    $ToolsDirectory = Get-FullPath $ToolsDirectory
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
            $executable = Join-Path $directory $tool.Executable
            # The marker holds the pin and the executable's hash from when the archive was verified: a pin that changed
            # under the same tag, or files restored from a cache that are not those, are fetched afresh.
            $reusable = $false
            if ((Test-Path -LiteralPath $marker) -and (Test-Path -LiteralPath $executable)) {
                $recorded = @([System.IO.File]::ReadAllText($marker).Split("`n") | ForEach-Object { $_.Trim() })
                $reusable = $recorded.Count -ge 2 -and $recorded[0] -eq $tool.Sha256 -and $recorded[1] -eq (Get-FileSha256 -Path $executable)
            }
            if (-not $reusable) {
                if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
                New-Item -ItemType Directory -Force -Path $ToolsDirectory | Out-Null
                $archive = Join-Path $ToolsDirectory "$Name-$($tool.Tag)-$($tool.Asset)"
                $url = "https://github.com/$($tool.Repository)/releases/download/$($tool.Tag)/$($tool.Asset)"
                & $Download $url $archive
                $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($actual -ne $tool.Sha256) {
                    Remove-Item -LiteralPath $archive -Force
                    throw "$url has SHA-256 $actual, not the pinned $($tool.Sha256.ToLowerInvariant())"
                }
                Expand-Archive -LiteralPath $archive -DestinationPath $directory -Force
                Remove-Item -LiteralPath $archive -Force
                if (-not (Test-Path -LiteralPath $executable)) { throw "$($tool.Executable) is not in $($tool.Asset)" }
                [System.IO.File]::WriteAllText($marker, "$($tool.Sha256.ToLowerInvariant())`n$(Get-FileSha256 -Path $executable)`n")
            }
            return $executable
        }
    }
    throw "tool '$Name' has the unknown Source '$($tool.Source)'"
}

function Get-SigningToolRoot {
    # The folder of a tool: a downloaded release's, or where the program is.
    param([string]$Name, [hashtable]$Table, [string]$ToolsDirectory)
    $tool = $Table.Tools[$Name]
    if ($tool.Source -eq 'GitHubRelease') { return Join-Path (Get-FullPath $ToolsDirectory) "$Name-$($tool.Tag)" }
    return Split-Path -Parent (Resolve-SigningTool -Name $Name -Table $Table -ToolsDirectory $ToolsDirectory)
}

function Invoke-SigningTool {
    # The one way a tool is launched, for a signing step and for its probe: an environment for the call only, a working
    # directory, and the output and exit code back. Never throws for a tool that fails.
    param([string]$Executable, [string[]]$Arguments, [hashtable]$Environment = @{}, [string]$WorkingDirectory = '')
    $saved = @{}
    foreach ($key in $Environment.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key)
        [Environment]::SetEnvironmentVariable($key, [string]$Environment[$key])
    }
    if ($WorkingDirectory) { Push-Location -LiteralPath $WorkingDirectory }
    try {
        $output = & $Executable @Arguments 2>&1 | Out-String
        return [pscustomobject]@{ Output = $output; ExitCode = $LASTEXITCODE }
    } finally {
        if ($WorkingDirectory) { Pop-Location }
        foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key]) }
    }
}

function Expand-ToolEnvironment {
    # The Environment of a tool with {toolDir} filled in.
    param([string]$Name, [hashtable]$Table, [string]$ToolsDirectory)
    $environment = @{}
    $declared = Get-TableValue $Table.Tools[$Name] 'Environment'
    if ($declared -is [hashtable]) {
        $root = Get-SigningToolRoot -Name $Name -Table $Table -ToolsDirectory $ToolsDirectory
        foreach ($key in $declared.Keys) { $environment[$key] = $declared[$key].Replace('{toolDir}', $root) }
    }
    return $environment
}

function Test-SigningToolStart {
    <#
    .SYNOPSIS
        Starts a tool the way a signing run does and says it ran: the probe of the table.
    .PARAMETER Name
        The tool's key in the table.
    .PARAMETER Table
        The signer table.
    .PARAMETER ToolsDirectory
        Where downloaded tools are kept.
    .OUTPUTS
        System.Management.Automation.PSCustomObject with Name, Path and Output (the first line of what the probe printed);
        for a library, with Output empty, only that it was found. Throws where the program does not start.
    #>
    [CmdletBinding()]
    [OutputType([pscustomobject])]
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][hashtable]$Table, [Parameter(Mandatory)][string]$ToolsDirectory)
    $path = Resolve-SigningTool -Name $Name -Table $Table -ToolsDirectory $ToolsDirectory
    $probe = Get-TableValue $Table.Tools[$Name] 'ProbeArguments'
    if ($null -eq $probe) { return [pscustomobject]@{ Name = $Name; Path = $path; Output = '' } }
    $root = Get-SigningToolRoot -Name $Name -Table $Table -ToolsDirectory $ToolsDirectory
    $arguments = @($probe | ForEach-Object { $_.Replace('{toolDir}', $root) })
    # From a folder that is not the tool's: a tool that only starts from its own folder is found out here.
    $result = Invoke-SigningTool -Executable $path -Arguments $arguments -Environment (Expand-ToolEnvironment -Name $Name -Table $Table -ToolsDirectory $ToolsDirectory) `
        -WorkingDirectory ([System.IO.Path]::GetTempPath())
    if ($result.ExitCode -ne 0) { throw "$Name did not start (exit code $($result.ExitCode)): $($result.Output.Trim())" }
    return [pscustomobject]@{ Name = $Name; Path = $path; Output = ($result.Output.Trim() -split "`r?`n")[0] }
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
        The signer table (its tools, Attempts, RetryDelaySeconds and Product).
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
    $Root = Get-FullPath $Root
    $ToolsDirectory = Get-FullPath $ToolsDirectory
    $WorkDirectory = Get-FullPath $WorkDirectory
    $toolName = $Step.Tool
    $executable = Resolve-SigningTool -Name $toolName -Table $Table -ToolsDirectory $ToolsDirectory
    $template = @($Step.Arguments)
    $urls = @(if ($Step.ContainsKey('TimestampUrls')) { $Step.TimestampUrls })
    $secrets = @($SecretName | ForEach-Object { [Environment]::GetEnvironmentVariable($_) } | Where-Object { $_ })
    $environment = Expand-ToolEnvironment -Name $toolName -Table $Table -ToolsDirectory $ToolsDirectory

    # The values that do not change from call to call: tools, their folders, data files and the product named by the
    # template.
    $fixed = @{}
    foreach ($name in @(Get-Placeholder -Text $template | Select-Object -Unique)) {
        if ($name -match '^tool:(.+)$') {
            $fixed[$name] = Resolve-SigningTool -Name $Matches[1] -Table $Table -ToolsDirectory $ToolsDirectory
        } elseif ($name -match '^toolDir:(.+)$') {
            $fixed[$name] = Get-SigningToolRoot -Name $Matches[1] -Table $Table -ToolsDirectory $ToolsDirectory
        } elseif ($name -match '^data:(.+)$') {
            $fixed[$name] = Join-Path $PSScriptRoot $Matches[1]
        } elseif ($name -match '^product:(name|url)$') {
            $fixed[$name] = if ($Matches[1] -eq 'name') { $Table.Product.Name } else { $Table.Product.Url }
        }
    }
    $usedTools = @($toolName) + @(Get-ToolReference -Text $template) | Select-Object -Unique

    # One call, retried: succeeds when the tool exits 0 and $Verify is happy. $Prepare clears what the last attempt left.
    $call = {
        param([hashtable]$Values, [string]$Label, [scriptblock]$Verify, [scriptblock]$Prepare)
        $attempts = [int]$Table.Attempts
        $delays = @($Table.RetryDelaySeconds)
        $tried = [System.Collections.Generic.List[string]]::new()
        $lastExit = 0
        for ($attempt = 0; $attempt -lt $attempts; $attempt++) {
            if ($attempt -gt 0 -and $delays.Count -gt 0) {
                Start-Sleep -Seconds $delays[[Math]::Min($attempt - 1, $delays.Count - 1)]
            }
            & $Prepare
            $all = $Values.Clone()
            foreach ($key in $fixed.Keys) { $all[$key] = $fixed[$key] }
            if ($urls.Count -gt 0) {
                $all['timestamp'] = $urls[$attempt % $urls.Count]
                $tried.Add($all['timestamp'])
            }
            $arguments = @(Expand-SigningArgument -Template $template -Values $all)
            Write-Host "  $toolName $($template -join ' ') [$Label] (attempt $($attempt + 1) of $attempts)"
            $result = Invoke-SigningTool -Executable $executable -Arguments $arguments -Environment $environment
            $lastExit = $result.ExitCode
            if ($lastExit -eq 0 -and (& $Verify)) { return }
            $output = $result.Output
            foreach ($secret in $secrets) { $output = $output.Replace($secret, '***') }
            Write-Host ($output.TrimEnd())
        }
        $servers = if ($tried.Count -gt 0) { "; timestamp servers tried: $($tried -join ', ')" } else { '' }
        throw "$toolName failed for $Label after $attempts attempts (exit code $lastExit)$servers"
    }

    $names = $Files | ForEach-Object { Split-Path -Leaf $_ }
    try {
        switch ($Step.Invocation) {
            'InPlace' {
                foreach ($relative in $Files) {
                    $path = Join-Path $Root $relative
                    & $call @{ file = $path } $relative { $true } {}
                }
            }
            'InOut' {
                $outDirectory = Join-Path $WorkDirectory 'out'
                foreach ($relative in $Files) {
                    $path = Join-Path $Root $relative
                    $out = Join-Path $outDirectory (Split-Path -Leaf $relative)
                    New-Item -ItemType Directory -Force -Path $outDirectory | Out-Null
                    & $call @{ in = $path; out = $out } $relative { Test-Path -LiteralPath $out -PathType Leaf } {
                        if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
                    }
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
                } {
                    Get-ChildItem -LiteralPath $outputDir -Force | Remove-Item -Recurse -Force
                }
                foreach ($relative in $Files) {
                    Copy-Item -LiteralPath (Join-Path $outputDir (Split-Path -Leaf $relative)) -Destination (Join-Path $Root $relative) -Force
                }
            }
        }
    } finally {
        # What a tool leaves behind (ssign's session is a token that signs for half an hour).
        foreach ($used in $usedTools) {
            foreach ($pattern in @(Get-TableValue $Table.Tools[$used] 'Cleanup' | Where-Object { $_ })) {
                $path = $pattern
                foreach ($name in @(Get-Placeholder -Text $pattern)) {
                    $value = if ($name -match '^env:(.+)$') { [Environment]::GetEnvironmentVariable($Matches[1]) }
                    if ([string]::IsNullOrEmpty($value)) { $path = $null; break }
                    $path = $path.Replace("{$name}", $value)
                }
                if ($path) { Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue }
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

function Test-PathInside {
    # Whether Child is Parent or lies under it.
    param([string]$Parent, [string]$Child)
    $parentPath = (Get-FullPath $Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $childPath = (Get-FullPath $Child).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    return $childPath.StartsWith($parentPath, [StringComparison]::OrdinalIgnoreCase)
}

function Get-SigningSelection {
    # The files a batch signs, relative to Root, by the row's policy (Payload) or as given (Packages: relative to Root,
    # or else to the current location).
    param([string]$Root, [string]$Kind, [string[]]$Files, [hashtable]$Row, [hashtable]$Table)
    $base = (Get-FullPath $Root).TrimEnd('\', '/')
    if ($Kind -eq 'Packages') {
        return @($Files | ForEach-Object {
                $candidate = if ([System.IO.Path]::IsPathRooted($_)) { $_ } elseif (Test-Path -LiteralPath (Join-Path $base $_)) { Join-Path $base $_ } else { $_ }
                $full = (Resolve-Path -LiteralPath (Get-FullPath $candidate)).Path
                if (-not (Test-PathInside $base $full)) { throw "$_ is not under $Root" }
                $full.Substring($base.Length + 1).Replace('\', '/')
            })
    }
    # `none` selects the very files a real signer would (and signs nothing), so that its manifest, and the checks
    # that read it, are those of a signed build.
    return @(Select-SigningFile -Root $base -Policy $Row.Payload -OwnBinaries @($Table.OwnBinaries))
}

function Get-KeptRow {
    # The manifest rows of the portable executables of a tree that a batch leaves as they are: SignedPath is '-', the hash
    # is the same before and after. The CPack hook fails on a staged .exe or .dll the manifest does not name, so that a
    # file the signed stage lacks cannot ship unsigned in a package; these rows are how it learns the rest.
    param([string]$Base, [string[]]$Selected, [hashtable]$Hash)
    return @(Get-PortableExecutable -Root $Base | Where-Object { $_ -notin $Selected } | ForEach-Object {
            [pscustomobject]@{ RelativePath = $_; SignedPath = '-'; Before = $Hash[$_]; After = $Hash[$_] }
        })
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
        For Packages, the package files: relative to Root, or else to the current location.
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
        System.Object[]. Rows with RelativePath, SignedPath, Before and After, for the files signed. The manifest also
        names the other portable executables of a payload, with SignedPath '-'.
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
    $Root = Get-FullPath $Root
    $WorkDirectory = Get-FullPath $WorkDirectory
    $ToolsDirectory = Get-FullPath $ToolsDirectory
    $Manifest = Get-FullPath $Manifest
    $row = Get-SignerRow -Table $Table -Signer $Signer
    if ($Kind -eq 'Payload' -and (Test-PathInside $Root $WorkDirectory)) {
        throw "the work directory $WorkDirectory lies inside $Root, which would be signed with it"
    }
    if ($row.Transport -eq 'SignPath') { throw "the signer '$Signer' signs through SignPath: use export/import" }
    if ($row.Transport -eq 'Command') { Test-SigningSecret -Row $row }

    $selected = @(Get-SigningSelection -Root $Root -Kind $Kind -Files $Files -Row $row -Table $Table)
    $base = $Root.TrimEnd('\', '/')
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
                elseif ($detail = Get-SignatureProblem -Path (Join-Path $base $relative) -Trust $Trust -Thumbprint $Thumbprint -Publisher $row.Publisher) { $problems.Add($detail) }
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
    if ($Manifest) {
        $kept = if ($Kind -eq 'Payload') { Get-KeptRow -Base $base -Selected $selected -Hash $after } else { @() }
        Write-SigningManifest -Path $Manifest -Rows @($rows.ToArray() + $kept)
    }
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
        For Packages, the package files: relative to Root, or else to the current location.
    .PARAMETER Signer
        The signer's key in the table.
    .PARAMETER Table
        The signer table.
    .PARAMETER Destination
        Where the copies go, as Destination/<relative path>; recreated, so that nothing of an earlier export goes along.
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
    $Root = Get-FullPath $Root
    $Destination = Get-FullPath $Destination
    $row = Get-SignerRow -Table $Table -Signer $Signer
    $base = $Root.TrimEnd('\', '/')
    if ((Test-PathInside $base $Destination) -or (Test-PathInside $Destination $base)) {
        throw "the destination $Destination and $Root must not contain one another"
    }
    $selected = @(Get-SigningSelection -Root $base -Kind $Kind -Files $Files -Row $row -Table $Table)
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    $rows = foreach ($relative in $selected) {
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $base $relative) -Destination $target -Force
        [pscustomobject]@{ RelativePath = $relative; SignedPath = $target.Replace('\', '/'); Before = (Get-FileSha256 -Path (Join-Path $base $relative)); After = '' }
    }
    $kept = if ($Kind -eq 'Payload') {
        $hashes = @{}
        foreach ($relative in Get-PortableExecutable -Root $base) { $hashes[$relative] = Get-FileSha256 -Path (Join-Path $base $relative) }
        Get-KeptRow -Base $base -Selected $selected -Hash $hashes
    } else { @() }
    Write-SigningManifest -Path $Manifest -Rows @(@($rows) + $kept)
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
    .PARAMETER Publisher
        For System, the pattern the signer certificate's subject must match.
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
        [string]$Thumbprint = '',
        [string]$Publisher = ''
    )
    $base = (Get-FullPath $Root).TrimEnd('\', '/')
    $From = Get-FullPath $From
    $Manifest = Get-FullPath $Manifest
    $rows = @(Read-SigningManifest -Path $Manifest)
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($row in $rows) {
        $signed = Join-Path $From $row.RelativePath
        $current = Join-Path $base $row.RelativePath
        if ($row.SignedPath -eq '-') {
            if (-not (Test-Path -LiteralPath $current) -or (Get-FileSha256 -Path $current) -ne $row.Before) {
                $problems.Add("$($row.RelativePath): the file under $base is not the one that was exported")
            }
            continue
        }
        if (-not (Test-Path -LiteralPath $signed)) { $problems.Add("$($row.RelativePath): no signed copy under $From"); continue }
        if (-not (Test-Path -LiteralPath $current) -or (Get-FileSha256 -Path $current) -ne $row.Before) {
            $problems.Add("$($row.RelativePath): the file under $base is not the one that was exported")
            continue
        }
        if ($detail = Get-SignatureProblem -Path $signed -Trust $Trust -Thumbprint $Thumbprint -Publisher $Publisher) { $problems.Add($detail); continue }
        # A validly signed file of another program (a copy of signtool.exe, say) is not the one that was exported.
        $was = (Get-Item -LiteralPath $current).VersionInfo
        $now = (Get-Item -LiteralPath $signed).VersionInfo
        foreach ($field in 'OriginalFilename', 'FileVersion') {
            if ([string]$was.$field -ne [string]$now.$field) {
                $problems.Add("$($row.RelativePath): the signed file has $field '$($now.$field)', the exported one '$($was.$field)'")
            }
        }
    }
    if ($problems.Count -gt 0) { throw "the signed files do not fit the tree:`n  $($problems -join "`n  ")" }
    $completed = foreach ($row in $rows) {
        if ($row.SignedPath -eq '-') { $row; continue }
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
    return @($completed | Where-Object { $_.SignedPath -ne '-' })
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
    $text = [System.IO.File]::ReadAllText((Get-FullPath $CPackConfig))
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
    $Build = Get-FullPath $Build
    $Destination = Get-FullPath $Destination
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
    $Root = Get-FullPath $Root
    $WorkDirectory = Get-FullPath $WorkDirectory
    $ToolsDirectory = Get-FullPath $ToolsDirectory
    $Packages = @($Packages | ForEach-Object { Get-FullPath $_ })
    if ((Test-PathInside $Root $WorkDirectory) -or (Test-PathInside $WorkDirectory $Root)) {
        throw "the work directory $WorkDirectory and the tree $Root must not contain one another"
    }
    Assert-OwnBinary -Root $Root -OwnBinaries @($Table.OwnBinaries)
    if (Test-Path -LiteralPath $WorkDirectory) { Remove-Item -LiteralPath $WorkDirectory -Recurse -Force }
    $stage = Join-Path $WorkDirectory 'stage'
    $packageDirectory = Join-Path $WorkDirectory 'packages'
    $certificateDirectory = Join-Path $WorkDirectory 'cert'
    New-Item -ItemType Directory -Force -Path $stage, $packageDirectory | Out-Null
    $certificate = $null
    $variables = 'CONTOUR_SIGNING_CERT', 'CONTOUR_SIGNING_KEY'
    $saved = @{}
    foreach ($variable in $variables) { $saved[$variable] = [Environment]::GetEnvironmentVariable($variable) }
    try {
        Get-ChildItem -LiteralPath $Root -Force | Copy-Item -Destination $stage -Recurse -Force
        $copies = foreach ($package in $Packages) {
            $copy = Join-Path $packageDirectory (Split-Path -Leaf $package)
            Copy-Item -LiteralPath $package -Destination $copy
            $copy
        }
        $certificate = New-ThrowawayCodeSigningCertificate -Directory $certificateDirectory
        $env:CONTOUR_SIGNING_CERT = $certificate.CertificatePath
        $env:CONTOUR_SIGNING_KEY = $certificate.KeyName

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
        # What the caller had in these variables is theirs: the dry run puts it back.
        foreach ($variable in $variables) { [Environment]::SetEnvironmentVariable($variable, $saved[$variable]) }
        if ($null -ne $certificate) { Remove-ThrowawayCodeSigningCertificate -Certificate $certificate }
        if (Test-Path -LiteralPath $certificateDirectory) { Remove-Item -LiteralPath $certificateDirectory -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

function Expand-MsiPayload {
    # The files of an installer, without installing it: an administrative image (msiexec /a) registers nothing and
    # changes nothing for the user. Bounded, as every wait on msiexec is.
    param([string]$Msi, [string]$Destination)
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $process = Start-Process -FilePath msiexec.exe -PassThru -NoNewWindow -ArgumentList @('/a', "`"$Msi`"", '/qn', "TARGETDIR=`"$Destination`"")
    $null = $process.Handle
    if (-not $process.WaitForExit(600000)) {
        $process.Kill($true)
        throw "msiexec /a did not end within 600 s for ${Msi}: it was ended"
    }
    if ($process.ExitCode -ne 0) { throw "msiexec /a failed for $Msi (exit code $($process.ExitCode))" }
}

function Test-PayloadTree {
    # What a package carries, as a list of problems: the manifest's files as signed, no signature of a certificate
    # Windows does not trust, and for a real signer no unsigned file it signs, and its own binaries signed.
    param([string]$Root, [string]$Label, [object[]]$Entries, [string]$Signer, [hashtable]$Table)
    $row = Get-SignerRow -Table $Table -Signer $Signer
    $transport = ([string]$row.Transport).ToLowerInvariant()
    $problems = [System.Collections.Generic.List[string]]::new()
    $files = @(Get-RelativeFile -Root $Root)

    # The files of the manifest are the ones packaged, as signed.
    foreach ($entry in $Entries) {
        $hits = @($files | Where-Object { $_ -eq $entry.RelativePath -or $_.EndsWith('/' + $entry.RelativePath) })
        $kept = $entry.SignedPath -eq '-'
        # A file the signer left alone need not be in every package, but where it is it is the staged one.
        if ($hits.Count -eq 0) { if (-not $kept) { $problems.Add("$Label`: $($entry.RelativePath): no such entry") }; continue }
        foreach ($name in $hits) {
            $path = Join-Path $Root $name
            if ((Get-FileSha256 -Path $path) -ne $entry.After) { $problems.Add("${name}: is not the $(if ($kept) { 'staged' } else { 'signed' }) file of the manifest ($Label)") }
            elseif (-not $kept -and $transport -ne 'none' -and ($detail = Get-SignatureProblem -Path $path -Trust System -Publisher $row.Publisher)) { $problems.Add("$detail ($Label)") }
        }
    }
    # Whatever the manifest says, a real signer's own binaries are in it and carry its signature.
    if ($transport -ne 'none') {
        foreach ($own in $Table.OwnBinaries) {
            if ($own -notin @($Entries | ForEach-Object { $_.RelativePath })) { $problems.Add("${own}: is not in the manifest, so $Label could ship it unsigned") }
        }
    }

    # Nothing signed with a certificate Windows does not trust (a dry run's) may ship; a file with no signature may.
    foreach ($relative in @(Get-PortableExecutable -Root $Root)) {
        $status = [string](Get-AuthenticodeSignature -LiteralPath (Join-Path $Root $relative)).Status
        if ($status -notin 'Valid', 'NotSigned') { $problems.Add("${relative}: $Label ships a file whose signature status is $status") }
        elseif ($status -eq 'NotSigned' -and $transport -ne 'none' -and $row.Payload -eq 'Unsigned') {
            $problems.Add("${relative}: $Label ships a file without a signature, which the $Signer signer signs")
        }
    }
    return , $problems
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
        Where the ZIP and the installers' files are extracted (recreated).
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
    $Manifest = Get-FullPath $Manifest
    $Zip = Get-FullPath $Zip
    $Msi = @($Msi | ForEach-Object { Get-FullPath $_ })
    $WorkDirectory = Get-FullPath $WorkDirectory
    $row = Get-SignerRow -Table $Table -Signer $Signer
    $transport = ([string]$row.Transport).ToLowerInvariant()
    $entries = @(Read-SigningManifest -Path $Manifest)
    $problems = [System.Collections.Generic.List[string]]::new()

    $extracted = Join-Path $WorkDirectory 'zip'
    if (Test-Path -LiteralPath $extracted) { Remove-Item -LiteralPath $extracted -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $extracted | Out-Null
    Expand-Archive -LiteralPath $Zip -DestinationPath $extracted -Force
    foreach ($problem in (Test-PayloadTree -Root $extracted -Label 'the ZIP' -Entries $entries -Signer $Signer -Table $Table)) { $problems.Add($problem) }

    foreach ($installer in $Msi) {
        if ($transport -eq 'none') {
            $status = [string](Get-AuthenticodeSignature -LiteralPath $installer).Status
            if ($status -ne 'NotSigned') { $problems.Add("${installer}: the signature status is $status; the none signer ships unsigned installers") }
        } elseif ($detail = Get-SignatureProblem -Path $installer -Trust System -Publisher $row.Publisher) {
            $problems.Add($detail)
        }
        # What the installer installs: the same checks as for the ZIP. The installers come from separate cpack runs, and
        # only the signature of the package itself says nothing of the files inside.
        $payload = Join-Path $WorkDirectory "msi-$([System.IO.Path]::GetFileNameWithoutExtension($installer))"
        Expand-MsiPayload -Msi $installer -Destination $payload
        foreach ($problem in (Test-PayloadTree -Root $payload -Label (Split-Path -Leaf $installer) -Entries $entries -Signer $Signer -Table $Table)) { $problems.Add($problem) }
    }
    if ($problems.Count -gt 0) { throw "the packages are not what the $Signer signer should ship:`n  $($problems -join "`n  ")" }
    Write-Host "verify: $Zip and $(@($Msi).Count) installers are what the $Signer signer should ship"
}

Export-ModuleMember -Function Test-SigningTable, Import-SigningTable, Resolve-SigningPlan, Expand-SigningArgument,
Get-PortableExecutable, Write-SigningManifest, Read-SigningManifest, New-ThrowawayCodeSigningCertificate,
Remove-ThrowawayCodeSigningCertificate, Get-SignatureProblem, Select-SigningFile, Resolve-SigningTool,
Test-SigningToolStart, Get-SigningToolRoot, Get-ToolReference, Invoke-SigningStep, Invoke-SignBatch, Export-SigningBatch,
Import-SigningBatch, Get-FileSha256, Get-CPackComponent, Invoke-Staging, Invoke-SigningDryRun, Test-ShippedArtifact
