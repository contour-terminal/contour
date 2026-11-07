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

Export-ModuleMember -Function Test-SigningTable, Import-SigningTable, Resolve-SigningPlan, Expand-SigningArgument,
Get-PortableExecutable, Write-SigningManifest, Read-SigningManifest
