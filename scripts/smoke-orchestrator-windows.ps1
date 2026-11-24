# End-to-end smoke check of the agent orchestrator on Windows: an installed contour, a daemon of its own under
# -Root, the bundled orchestrator.wasm, and (by tier) Claude Code and the GitLab and Jira forges.
#
#   pwsh -NoProfile -File scripts/smoke-orchestrator-windows.ps1                       # Offline tier
#   pwsh -NoProfile -File scripts/smoke-orchestrator-windows.ps1 -Tier Claude          # also rows 9-10
#   pwsh -NoProfile -File scripts/smoke-orchestrator-windows.ps1 -Tier Forge `
#        -GitLabRepo C:\path\to\a\gitlab\checkout -JiraSite https://example.atlassian.net -Jql 'assignee = currentUser()'
#   pwsh -NoProfile -File scripts/smoke-orchestrator-windows.ps1 -Probe rename|board|tree   # one manual-table check
#   pwsh -NoProfile -File scripts/smoke-orchestrator-windows.ps1 -Stop [-Keep]              # stop and clean up
#
# A run installs the build into -Root, makes an offline repository with a bare `file` remote, writes a configuration
# and starts a daemon named orch-smoke with %LOCALAPPDATA% pointed into -Root, so the user's own daemon, state and
# configuration are never touched. It then runs the automated checks, prints the manual table, starts a client
# window and waits (5 min) for the grant. -NoClient leaves both to the user: the run ends after the automated checks
# and the printed table, the daemon still running for them.
#
# Nothing is written anywhere but -Root, the daemon's state under the overridden %LOCALAPPDATA% and, for the Forge
# tier, the worktree metadata git keeps in -GitLabRepo\.git\worktrees. Nothing in -GitLabRepo is ever deleted: -Stop
# lists the agent/* branches a run left there for the user to delete.

[CmdletBinding()]
param(
    [string]$Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\build\clangcl-release'),
    [string]$Root = (Join-Path $env:TEMP 'contour-orch-smoke'),
    [ValidateSet('Offline', 'Claude', 'Forge')]
    [string]$Tier = 'Offline',
    [string]$GitLabRepo,
    [string]$JiraSite,
    [string]$Jql,
    [switch]$Keep,
    [switch]$Stop,
    [ValidateSet('rename', 'board', 'tree')]
    [string]$Probe,
    [switch]$NoClient
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$DaemonName = 'orch-smoke'
$LocalAppData = Join-Path $Root 'localappdata'
$Install = Join-Path $Root 'install'
$Config = Join-Path $Root 'contour.yml'
$DaemonLog = Join-Path $Root 'daemon.log'
# The control socket, given explicitly: by --name alone it would sit in the user's runtime directory, outside -Root.
# Client verbs reach it by path (--daemon PATH); their --daemon NAME names a saved host, not a daemon.
$Socket = Join-Path $Root 'daemon.sock'
$SmokeRepo = Join-Path $Root 'repos\smoke'
$SmokeRemote = Join-Path $Root 'remote\smoke.git'
$WorktreeRoot = Join-Path $Root 'wt'
$TreeRecord = Join-Path $Root 'probe-tree.txt'
$script:Failures = 0

Add-Type -Namespace SmokeNative -Name Path -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern uint GetLongPathNameW(string shortPath, System.Text.StringBuilder longPath, uint size);
'@

# The long spelling of an existing path (C:\Users\LONGNAME\... for C:\Users\LONGNA~1\...); the path itself when
# Windows cannot tell.
function Get-LongPath([string]$Path) {
    $buffer = [System.Text.StringBuilder]::new(1024)
    if ([SmokeNative.Path]::GetLongPathNameW($Path, $buffer, 1024) -gt 0) { return $buffer.ToString() }
    return $Path
}

# A YAML single-quoted scalar: the documented spelling for a Windows path, backslashes taken literally.
function ConvertTo-YamlQuoted([string]$Text) { return "'" + ($Text -replace "'", "''") + "'" }

function Write-Check([string]$Name, [bool]$Passed, [string]$Detail = '') {
    if ($Passed) { Write-Host "PASS  $Name" }
    else {
        $script:Failures++
        Write-Host "FAIL  $Name"
        if ($Detail) { $Detail -split "`n" | ForEach-Object { Write-Host "        $_" } }
    }
}

function Stop-Smoke([string]$Why) { Write-Host "smoke: $Why"; exit 2 }

# Runs contour (or any program) bounded, so that a verb that waits forever cannot hold the run. Sets
# $script:ExitCode; returns stdout and stderr together. Both streams are read asynchronously and their wait is
# bounded too: a detached daemon that inherited a pipe would otherwise keep them open.
function Invoke-Bounded([string]$Program, [string[]]$Arguments, [int]$Seconds = 60) {
    $quoted = $Arguments | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }
    $info = [System.Diagnostics.ProcessStartInfo]::new($Program)
    $info.Arguments = $quoted -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $info.StandardOutputEncoding = $utf8
    $info.StandardErrorEncoding = $utf8
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit($Seconds * 1000)
    if ($timedOut) { $process.Kill() }
    $process.WaitForExit()
    $null = [System.Threading.Tasks.Task]::WaitAll([System.Threading.Tasks.Task[]]@($stdout, $stderr), 10000)
    $text = ''
    if ($stdout.IsCompleted) { $text += $stdout.Result }
    if ($stderr.IsCompleted) { $text += $stderr.Result }
    $script:ExitCode = if ($timedOut) { -1 } else { $process.ExitCode }
    return $text
}

function Invoke-Contour([string[]]$Arguments, [int]$Seconds = 60) {
    return Invoke-Bounded $script:Contour $Arguments $Seconds
}

# Waits, at most $Seconds, for $Condition to hold.
function Wait-Until([int]$Seconds, [scriptblock]$Condition) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return [bool](& $Condition)
}

# The program's exit code, run in $Directory, output discarded.
function Get-ExitCode([string]$Directory, [string]$Program, [string[]]$Arguments) {
    Push-Location $Directory
    try { $null = Invoke-Bounded $Program $Arguments 60 } finally { Pop-Location }
    return $script:ExitCode
}

# ---- the process environment: the smoke's own state directory, and no pane's CONTOUR_* target ------------------

$SavedEnvironment = @{}
foreach ($name in 'LOCALAPPDATA', 'CONTOUR_WORKSPACE', 'CONTOUR_TAB', 'CONTOUR_PANE', 'CONTOUR_MUX', 'CONTOUR_DAEMON', 'CONTOUR_PROFILE') {
    $SavedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Enter-SmokeEnvironment {
    $env:LOCALAPPDATA = $LocalAppData
    foreach ($name in 'CONTOUR_WORKSPACE', 'CONTOUR_TAB', 'CONTOUR_PANE', 'CONTOUR_MUX', 'CONTOUR_DAEMON', 'CONTOUR_PROFILE') {
        Remove-Item "env:$name" -ErrorAction SilentlyContinue
    }
}

function Exit-SmokeEnvironment {
    foreach ($entry in $SavedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
}

$script:Contour = Join-Path $Install 'bin\contour.exe'
if (-not (Test-Path $script:Contour)) { $script:Contour = Join-Path $Build 'bin\contour.exe' }

# ---- -Stop ------------------------------------------------------------------------------------------------------

function Invoke-Stop {
    if (Test-Path $script:Contour) {
        $output = Invoke-Contour @('daemon', 'stop', '--socket', $Socket, '--yes')
        Write-Host "daemon stop (exit $script:ExitCode): $($output.Trim())"
    }
    foreach ($repo in @($SmokeRepo, $GitLabRepo)) {
        if ($repo -and (Test-Path (Join-Path $repo '.git'))) { git -C $repo worktree prune }
    }
    if ($GitLabRepo -and (Test-Path $GitLabRepo)) {
        $branches = @(git -C $GitLabRepo branch --list 'agent/*' --format '%(refname:short)')
        if ($branches.Count -gt 0) {
            Write-Host "The run left these branches in $GitLabRepo; delete them yourself once you have looked:"
            $branches | ForEach-Object { Write-Host "    $_" }
        }
    }
    if ($Keep) { Write-Host "kept $Root"; return }
    # The daemon's per-version copy under $LocalAppData may stay locked for a moment after it ends.
    $removed = Wait-Until 15 {
        Remove-Item -Recurse -Force $Root -ErrorAction SilentlyContinue
        -not (Test-Path $Root)
    }
    if ($removed) { Write-Host "removed $Root" } else { Write-Host "could not remove all of $Root; remove it by hand" }
}

# ---- -Probe -----------------------------------------------------------------------------------------------------

function Get-Worktrees {
    $parent = Join-Path $WorktreeRoot 'smoke'
    if (-not (Test-Path $parent)) { return @() }
    return @(Get-ChildItem -Directory $parent)
}

# The PIDs of every claude.exe run by this smoke's daemon, and their descendants.
function Get-AgentTree {
    $all = @(Get-CimInstance Win32_Process)
    $byId = @{}
    foreach ($p in $all) { $byId[[int]$p.ProcessId] = $p }
    $roots = @((Get-LongPath $Root), $Root) | ForEach-Object { $_.TrimEnd('\') + '\' }
    $fromSmoke = {
        param($process)
        $seen = @{}
        $current = $process
        while ($current -and -not $seen.ContainsKey([int]$current.ProcessId)) {
            $seen[[int]$current.ProcessId] = $true
            $path = $current.ExecutablePath
            if ($path) { foreach ($r in $roots) { if ($path.StartsWith($r, [StringComparison]::OrdinalIgnoreCase)) { return $true } } }
            $current = $byId[[int]$current.ParentProcessId]
        }
        return $false
    }
    $agents = @($all | Where-Object { $_.Name -eq 'claude.exe' -and (& $fromSmoke $_) })
    $tree = [System.Collections.Generic.List[object]]::new()
    $queue = [System.Collections.Generic.Queue[object]]::new()
    foreach ($a in $agents) { $queue.Enqueue($a) }
    while ($queue.Count -gt 0) {
        $p = $queue.Dequeue()
        $tree.Add($p)
        foreach ($child in $all) {
            if ($child.ParentProcessId -eq $p.ProcessId -and $child.ProcessId -ne $p.ProcessId) { $queue.Enqueue($child) }
        }
    }
    return $tree
}

function Invoke-Probe([string]$Which) {
    if (-not (Test-Path $Root)) { Stop-Smoke "no smoke under $Root; start one first" }
    switch ($Which) {
        'rename' {
            $worktrees = @(Get-Worktrees)
            Write-Check 'a worktree exists under wt\smoke' ($worktrees.Count -gt 0)
            foreach ($target in @($worktrees) + @(Get-Item (Join-Path $WorktreeRoot 'smoke') -ErrorAction SilentlyContinue)) {
                if (-not $target) { continue }
                $newName = $target.Name + '-renamed'
                $refused = $false
                try { Rename-Item -LiteralPath $target.FullName -NewName $newName -ErrorAction Stop }
                catch { $refused = $true; $reason = $_.Exception.Message }
                if (-not $refused) { Rename-Item -LiteralPath (Join-Path $target.Parent.FullName $newName) -NewName $target.Name }
                Write-Check "renaming $($target.Name) is refused while its tab is open" $refused 'the rename succeeded (it was renamed back)'
                if ($refused) { Write-Host "        ($reason)" }
            }
        }
        'board' {
            $worktrees = @(Get-Worktrees)
            if ($worktrees.Count -eq 0) { Stop-Smoke 'no worktree under wt\smoke; start a run first (row 3)' }
            $newest = $worktrees | Sort-Object LastWriteTime | Select-Object -Last 1
            $text = "smoke board $(Get-Date -Format 'HH:mm:ss')"
            Set-Content -LiteralPath (Join-Path $newest.FullName 'STATUS.md') -Value "# $text" -Encoding utf8
            Write-Host "wrote STATUS.md into $($newest.FullName); press b on its run: the board must show '$text'"
        }
        'tree' {
            $tree = @(Get-AgentTree)
            $previous = @()
            if (Test-Path $TreeRecord) { $previous = @(Get-Content $TreeRecord | Where-Object { $_ } | ForEach-Object { [int]($_ -split ' ')[0] }) }
            if ($previous.Count -gt 0) {
                Write-Host 'Recorded before:'
                foreach ($id in $previous) {
                    $alive = [bool](Get-Process -Id $id -ErrorAction SilentlyContinue)
                    Write-Host ("    {0,6}  {1}" -f $id, $(if ($alive) { 'alive' } else { 'gone' }))
                }
            }
            Write-Host "Now: $($tree.Count) process(es) under the smoke's claude.exe"
            $lines = $tree | ForEach-Object { '{0} {1} parent={2}' -f $_.ProcessId, $_.Name, $_.ParentProcessId }
            $lines | ForEach-Object { Write-Host "    $_" }
            if ($tree.Count -gt 0) { Set-Content -LiteralPath $TreeRecord -Value $lines; Write-Host "recorded to $TreeRecord" }
        }
    }
}

# ---- the manual table -------------------------------------------------------------------------------------------

function Write-ManualSteps {
    $self = "pwsh -NoProfile -File `"$PSCommandPath`""
    $withEnv = "`$env:LOCALAPPDATA = '$LocalAppData'; & '$script:Contour'"
    $client = "$withEnv client --daemon '$Socket'"
    $rows = @(
        @('1', "With the same LOCALAPPDATA: $client", 'The window asks "“orchestrator” wants to run in the background", its directories in the C:\... long spelling; grant', 'Offline'),
        @('2', 'Command palette -> "Open Agents"', 'The orchestrator:// tab; under problems smoke''s glab checks fail (no GitLab host) with a readable reason: a numeric code decimal or 0x..., never negative', 'Offline'),
        @('3', 'n, type a topic, choose topic, confirm', "A worktree under $WorktreeRoot\smoke\<run>; a tab runs claude.exe in it (without a login, its login screen)", 'Offline'),
        @('4', "$self -Probe rename", 'Renaming the worktree and its parent smoke is refused (access denied) while the tab is open', 'Offline'),
        @('5', "$self -Probe board, then press b", 'The board shows the text the probe wrote (file-read on Windows)', 'Offline'),
        @('6', "$self -Probe tree", 'The claude.exe PIDs and their children, recorded', 'Offline'),
        @('7', "$withEnv daemon upgrade --socket '$Socket'; then $self -Probe tree", 'The tab and its claude.exe PID survive (-Probe tree: alive); the dashboard still lists the run', 'Offline'),
        @('8', "Delete STATUS.md, press e on the run; within 5 s: $self -Probe tree", "The tab closes first, then the worktree is removed (git -C $SmokeRepo worktree list no longer names it); every PID from 6 is gone", 'Offline'),
        @('9', 'Logged in to Claude: repeat 3 and let the agent finish', 'The agent writes STATUS.md; the board shows it; note whether a refresh ever answered "unreadable"', 'Claude'),
        @('10', 't on that run', 'The transcript pages load (the session log''s Windows cwd spelling matched)', 'Claude'),
        @('11', 'The Forge tier''s lists', 'The MR list has as many rows as the count printed below for glab, the issue list as many as for acli; CI marks follow detailed_merge_status', 'Forge'),
        @('12', 'Start issue on one Jira item (spends Claude tokens; optional)', 'A worktree from base or the default branch; the agent''s prompt carries <JiraSite>/browse/<KEY>', 'Forge')
    )
    $tiers = switch ($Tier) { 'Offline' { @('Offline') } 'Claude' { @('Offline', 'Claude') } 'Forge' { @('Offline', 'Claude', 'Forge') } }
    Write-Host ''
    Write-Host 'Manual steps (do, then expect):'
    foreach ($row in $rows) {
        if ($tiers -notcontains $row[3]) { continue }
        Write-Host ("{0,3}. [{1}] {2}" -f $row[0], $row[3], $row[1])
        Write-Host ("      expect: {0}" -f $row[2])
    }
    Write-Host "When done: $self -Stop   (add -Keep to keep $Root)"
}

# ---- the run ----------------------------------------------------------------------------------------------------

function Invoke-Preflight {
    if (-not (Test-Path (Join-Path $Build 'bin\contour.exe'))) { Stop-Smoke "no $Build\bin\contour.exe; build first" }
    if (-not (Test-Path (Join-Path $Build 'wasm\orchestrator.wasm'))) { Stop-Smoke "no $Build\wasm\orchestrator.wasm; build with the orchestrator guest" }
    if (-not (Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue)) { Stop-Smoke 'cmake not found' }
    if (-not (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) { Stop-Smoke 'git.exe not found' }
    $claude = Get-Command claude -CommandType Application -ErrorAction SilentlyContinue | Where-Object Extension -eq '.exe' | Select-Object -First 1
    if (-not $claude) { Stop-Smoke "install Claude Code's native claude.exe" }
    $script:ClaudeExe = $claude.Source
    if (Test-Path $Root) { Stop-Smoke "$Root exists: a smoke may still be running; run -Stop first" }

    if ($Tier -eq 'Forge') {
        if (-not $GitLabRepo -or -not $JiraSite -or -not $Jql) { Stop-Smoke '-Tier Forge needs -GitLabRepo, -JiraSite and -Jql' }
        if (-not (Test-Path (Join-Path $GitLabRepo '.git'))) { Stop-Smoke "-GitLabRepo $GitLabRepo is no git checkout" }
        if ($JiraSite -notmatch '^https://[^/\s]+/?$') { Stop-Smoke '-JiraSite must be https://<host>, with no path' }
        foreach ($tool in 'glab', 'acli') {
            if (-not (Get-Command $tool -CommandType Application -ErrorAction SilentlyContinue | Where-Object Extension -eq '.exe')) { Stop-Smoke "$tool.exe not found" }
        }
        $checks = @(@('glab', @('auth', 'status')), @('acli', @('jira', 'auth', 'status')))
        foreach ($check in $checks) {
            $own = Get-ExitCode $GitLabRepo $check[0] $check[1]
            if ($own -ne 0) { Stop-Smoke "$($check[0]) $($check[1] -join ' ') fails (exit $own); log it in first" }
            $saved = $env:LOCALAPPDATA
            $env:LOCALAPPDATA = $LocalAppData
            try { $isolated = Get-ExitCode $GitLabRepo $check[0] $check[1] } finally { $env:LOCALAPPDATA = $saved }
            if ($isolated -ne $own) { Stop-Smoke "state isolation would log glab/acli out ($($check[0]) exits $isolated with LOCALAPPDATA under -Root)" }
        }
    }
    Write-Host "preflight: contour.exe, orchestrator.wasm, git.exe and claude.exe found"
}

function Invoke-Install {
    New-Item -ItemType Directory -Force $Root | Out-Null
    $output = Invoke-Bounded 'cmake' @('--install', $Build, '--prefix', $Install) 600
    if ($script:ExitCode -ne 0) { Write-Host $output; Stop-Smoke "cmake --install failed (exit $script:ExitCode)" }
    $script:Contour = Join-Path $Install 'bin\contour.exe'
    Write-Check 'install: share\contour\extensions\orchestrator.wasm is installed' (Test-Path (Join-Path $Install 'share\contour\extensions\orchestrator.wasm'))
    if (-not (Test-Path $script:Contour)) { Stop-Smoke "the install has no bin\contour.exe" }
}

function Invoke-Git([string[]]$Arguments) {
    $output = & git @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host ($output | Out-String); Stop-Smoke "git $($Arguments -join ' ') failed" }
}

function New-OfflineRepository {
    New-Item -ItemType Directory -Force (Split-Path -Parent $SmokeRepo), (Split-Path -Parent $SmokeRemote) | Out-Null
    Invoke-Git @('init', '-q', '-b', 'main', $SmokeRepo)
    Set-Content -LiteralPath (Join-Path $SmokeRepo 'README.md') -Value '# smoke' -Encoding utf8
    Invoke-Git @('-C', $SmokeRepo, 'add', 'README.md')
    Invoke-Git @('-C', $SmokeRepo, '-c', 'user.name=smoke', '-c', 'user.email=smoke@example.invalid', '-c', 'commit.gpgsign=false', 'commit', '-q', '-m', 'smoke')
    Invoke-Git @('init', '-q', '--bare', '-b', 'main', $SmokeRemote)
    Invoke-Git @('-C', $SmokeRepo, 'remote', 'add', 'origin', $SmokeRemote)
    Invoke-Git @('-C', $SmokeRepo, 'push', '-q', '-u', 'origin', 'main')
    Write-Host "repository: $SmokeRepo, origin $SmokeRemote"
}

function Write-SmokeConfig {
    $notExe = Join-Path $Root 'bin\not-an-exe.cmd'
    New-Item -ItemType Directory -Force (Split-Path -Parent $notExe) | Out-Null
    Set-Content -LiteralPath $notExe -Value '@exit /b 0' -Encoding ascii
    $q = { param($text) ConvertTo-YamlQuoted $text }
    # Task keys: `t` (and d, e, b, x, z, n, I, m, /, ?, j, k, R) are the dashboard's own and refused; the topic task
    # takes `s`.
    $yaml = @"
daemon:
    extensions:
        orchestrator:
            autostart: true
            programs:
                claude: $(& $q $script:ClaudeExe)
                smoke-not-exe: $(& $q $notExe)
            config:
                worktree_root: $(& $q $WorktreeRoot)
                models: [sonnet]
                status_board_after: 1m
                end_done_after: 0
                permissions: ask
                repos:
                    - { path: $(& $q $SmokeRepo), name: smoke, forge: gitlab, base: main }

"@
    if ($Tier -eq 'Forge') {
        $yaml += @"
                    - path: $(& $q $GitLabRepo)
                      forge: gitlab
                      issues:
                          - { source: jira, jql: $(& $q $Jql), site: $(& $q $JiraSite) }

"@
    }
    $yaml += @'
                tasks:
                    - { id: topic, key: s, label: topic, applies_to: topic, model: sonnet,
                        prompt: "Write the word smoke into STATUS.md at the worktree's root, then stop." }
                    - { id: issue, key: w, label: "work on issue", applies_to: issue, model: sonnet,
                        prompt: "Read {url} and write a one-line plan into STATUS.md, then stop." }
'@
    [System.IO.File]::WriteAllText($Config, $yaml, [System.Text.UTF8Encoding]::new($false))
    Write-Host "configuration: $Config"
}

function Start-SmokeDaemon {
    New-Item -ItemType Directory -Force $LocalAppData | Out-Null
    $output = Invoke-Contour @('daemon', '--name', $DaemonName, '--socket', $Socket, '--config', $Config, '--background', '--log-file', $DaemonLog) 120
    Write-Check 'daemon: started in the background' ($script:ExitCode -eq 0) $output
    if ($script:ExitCode -ne 0) { if (Test-Path $DaemonLog) { Get-Content $DaemonLog -Tail 40 | Write-Host }; Stop-Smoke 'the daemon did not start' }
}

function Invoke-AutomatedChecks {
    $logHas = { param($text) (Test-Path $DaemonLog) -and (Select-String -LiteralPath $DaemonLog -SimpleMatch $text -Quiet) }
    $warned = Wait-Until 10 { (& $logHas "Program 'smoke-not-exe' of extension 'orchestrator'") -and (& $logHas 'only an .exe') }
    Write-Check "daemon.log: Program 'smoke-not-exe' of extension 'orchestrator' ... only an .exe" $warned

    $script:Listing = ''
    $pending = Wait-Until 60 {
        $script:Listing = Invoke-Contour @('extension', 'list', '--daemon', $Socket)
        $script:Listing -match '(?m)^orchestrator\s+pending\s*$'
    }
    Write-Check 'extension list: orchestrator pending' $pending $script:Listing
    $lines = @($script:Listing -split "`r?`n")
    $longRoot = Get-LongPath $Root
    $leaf = Split-Path -Leaf $longRoot
    Write-Check "extension list: the question names ...\$leaf\wt" ([bool]($lines | Where-Object { $_.Contains("\$leaf\wt") })) $script:Listing
    Write-Check 'extension list: the question names ...\repos\smoke' ([bool]($lines | Where-Object { $_.Contains('\repos\smoke') })) $script:Listing
    $shortNote = if ($Root.Contains('~')) { " (-Root is spelled $Root)" } else { ' (-Root has no 8.3 spelling here, so this proves little)' }
    Write-Check "extension list: no path in the question contains ~1$shortNote" (-not $script:Listing.Contains('~1')) $script:Listing
    $module = $lines | Where-Object { $_ -match '^\s*Module:' } | Select-Object -First 1
    Write-Check 'extension list: the module line ends in share\contour\extensions\orchestrator.wasm' ([bool]($module -and $module.TrimEnd().EndsWith('share\contour\extensions\orchestrator.wasm'))) "$module"
}

function Write-ForgeCounts {
    if ($Tier -ne 'Forge') { return }
    Push-Location $GitLabRepo
    try {
        $mrs = Invoke-Bounded 'glab' @('mr', 'list', '--author=@me', '--per-page', '100', '--output', 'json')
        $mrCount = if ($script:ExitCode -eq 0) { @($mrs | ConvertFrom-Json).Count } else { "(glab exit $script:ExitCode)" }
        $items = Invoke-Bounded 'acli' @('jira', 'workitem', 'search', '--jql', $Jql, '--limit', '100', '--json')
        $itemCount = if ($script:ExitCode -eq 0) { @($items | ConvertFrom-Json).Count } else { "(acli exit $script:ExitCode)" }
    } finally { Pop-Location }
    Write-Host "row 11: glab lists $mrCount merge request(s), acli $itemCount work item(s)"
}

function Wait-ForGrant {
    Start-Process -FilePath $script:Contour -ArgumentList @('client', '--daemon', $Socket) | Out-Null
    Write-Host 'Started a client window: grant the orchestrator there (row 1). Waiting up to 5 min for it to run ...'
    $script:Status = ''
    $running = Wait-Until 300 {
        $script:Status = Invoke-Contour @('extension', 'status', '--daemon', $Socket, 'orchestrator')
        ($script:ExitCode -eq 0) -and ($script:Status -match '\brunning\b')
    }
    Write-Check 'extension status: orchestrator running after the grant' $running $script:Status
}

try {
    Enter-SmokeEnvironment
    if ($Stop) { Invoke-Stop; exit 0 }
    if ($Probe) { Invoke-Probe $Probe; exit ([int]($script:Failures -gt 0)) }

    Invoke-Preflight
    Invoke-Install
    New-OfflineRepository
    Write-SmokeConfig
    Start-SmokeDaemon
    Invoke-AutomatedChecks
    if ($script:Failures -gt 0) {
        Write-Host "$script:Failures automated check(s) failed; the daemon is left running for a look ($DaemonLog). Then -Stop."
        exit 1
    }
    Write-ForgeCounts
    Write-ManualSteps
    if (-not $NoClient) { Wait-ForGrant }
    exit ([int]($script:Failures -gt 0))
}
finally {
    Exit-SmokeEnvironment
}
