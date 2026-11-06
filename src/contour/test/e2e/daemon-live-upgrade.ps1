# Live-upgrade sweep for the contour_e2e_daemon_live_upgrade test on Windows: a real `contour daemon` hands itself
# over to a release (this very binary) while a hosted program runs, and the program notices nothing. $args[0] is
# the contour binary.
#
# The Windows counterpart of daemon-live-upgrade.sh. Windows has no exec that keeps the process, so the old
# daemon starts its successor, hands it every pseudoconsole, child and listener, and ends once the successor is
# ready: the DAEMON's pid changes, the PROGRAM's does not. vthost_test drives that handoff through a fake
# launcher; only a real one shows the wiring around it -- the handles the successor inherits, the commit point
# both processes meet at, a listener taken over from the old daemon's completion port, the pseudoconsoles
# rebuilt from their handles.
#
# Scenario 1 hosts a program that prints its pid and a numbered line every 200 ms. After `contour daemon upgrade`
# it asserts that the successor took over (the upgrade generation went up, the new daemon logged taking the
# session over), that the daemon's pid changed and the old daemon is gone, that the program is the same process,
# and that the counter is gapless across the handover.
#
# Scenario 2 breaks the configuration file the daemon started with and asks again: the new release cannot load
# it, so the upgrade is refused (exit 5, configUnreadable) and the old daemon serves on, untouched.
#
# Scenario 3 upgrades the successor itself: what one handover appended to the command line is replaced, not
# repeated, and the program comes through a second time.
#
# The handover pause is measured and printed, never asserted.
#
# Windows PowerShell 5.1, which every Windows has: nothing newer is assumed.

$ErrorActionPreference = 'Stop'
$Contour = $args[0]
if (-not $Contour -or -not (Test-Path $Contour)) { Write-Error 'usage: daemon-live-upgrade.ps1 C:\path\to\contour.exe'; exit 64 }

$Out = Join-Path ([System.IO.Path]::GetTempPath()) ('contour-live-upgrade-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $Out | Out-Null

# An install of its own, as an installer lays one out (bin\ beside share\contour\): the daemon runs from it and
# the upgrade stages a copy of it, so it holds what the daemon loads and nothing a build tree has besides --
# test programs and debug databases by the gigabyte.
$Built = Split-Path -Parent (Resolve-Path $Contour)
$Install = Join-Path $Out 'install'
New-Item -ItemType Directory -Force (Join-Path $Install 'bin'), (Join-Path $Install 'share\contour') | Out-Null
Get-ChildItem $Built -Filter '*.dll' | Copy-Item -Destination (Join-Path $Install 'bin')
Copy-Item (Join-Path $Built 'contour.exe') (Join-Path $Install 'bin')
$OpenConsole = Join-Path $Built 'OpenConsole.exe'
if (Test-Path $OpenConsole) { Copy-Item $OpenConsole (Join-Path $Install 'bin') }
$Contour = Join-Path $Install 'bin\contour.exe'

# The developer's own configuration and state stay out of it; and so does the pane this may be run from, whose
# CONTOUR_* variables would otherwise name it as the target of every verb below.
$env:LOCALAPPDATA = Join-Path $Out 'appdata'
foreach ($name in 'CONTOUR_WINDOW', 'CONTOUR_TAB', 'CONTOUR_PANE', 'CONTOUR_MUX', 'CONTOUR_DAEMON', 'CONTOUR_PROFILE') {
    Remove-Item "env:$name" -ErrorAction SilentlyContinue
}
$Config = Join-Path $env:LOCALAPPDATA 'contour\contour.yml'
New-Item -ItemType Directory -Force (Split-Path -Parent $Config) | Out-Null
$Socket = Join-Path $Out 'sock'
$Log = Join-Path $Out 'daemon.log'
$script:Daemon = $null
# Every daemon this run started or saw take over: the cleanup ends each, whichever assertion failed.
$script:Daemons = @()
$script:ProgramPid = 0

function Fail([string]$why) {
    Write-Host "error: $why"
    if (Test-Path $Log) { Write-Host '--- daemon log ---'; Get-Content $Log -Tail 40 | Write-Host }
    $last = Join-Path $Out 'last.txt'
    if (Test-Path $last) { Write-Host '--- last command output ---'; Get-Content $last | Write-Host }
    throw $why
}

# Runs the contour binary against the daemon under test, bounded: a verb that waits forever must not hold the
# RUN_SERIAL slot until ctest's own timeout. Sets $script:Status, returns the output.
function Contour([string[]]$arguments) {
    $quoted = $arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }
    $process = Start-Process -FilePath $Contour -ArgumentList ($quoted -join ' ') -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $Out 'last.txt') -RedirectStandardError (Join-Path $Out 'last.err')
    # Taken now: a process object that never held its handle reports no exit code once the process is gone.
    $null = $process.Handle
    if (-not $process.WaitForExit(60000)) { $process.Kill(); Fail "contour $($arguments -join ' ') did not end within 60s" }
    $script:Status = $process.ExitCode
    return ((Get-Content (Join-Path $Out 'last.txt') -Raw) + (Get-Content (Join-Path $Out 'last.err') -Raw))
}

# Waits, at most $Tenths deciseconds, for $Condition to hold.
function WaitFor([int]$Tenths, [scriptblock]$Condition) {
    for ($waited = 0; $waited -le $Tenths; $waited++) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function ProcessAlive([int]$id) { return [bool](Get-Process -Id $id -ErrorAction SilentlyContinue) }

# The daemon's pid, from `contour daemon status`.
function DaemonStatus {
    $text = Contour @('daemon', 'status', '--socket', $Socket)
    if ($script:Status -ne 0) { Fail "daemon status exited $($script:Status)" }
    return $text
}
function DaemonPidOf([string]$status) { return [int]([regex]::Match($status, '\(pid (\d+)\)').Groups[1].Value) }
function UpgradesOf([string]$status) { return [int]([regex]::Match($status, '(?m)^upgrades:\s+(\d+)').Groups[1].Value) }

# The tick numbers the program's screen shows now.
function Ticks {
    $screen = Contour @('pane', 'capture', 'pane', $script:Pane, 'daemon', $Socket)
    if ($script:Status -ne 0) { Fail "pane capture exited $($script:Status)" }
    return @([regex]::Matches($screen, '(?m)^\s*tick (\d+)\s*$') | ForEach-Object { [int]$_.Groups[1].Value })
}

# The ticks must form one unbroken run, at least $Need long: a byte lost across the handover would leave a hole.
function GapOf([int[]]$ticks, [int]$need) {
    for ($i = 1; $i -lt $ticks.Count; $i++) {
        if ($ticks[$i] -ne $ticks[$i - 1] + 1) { return "tick $($ticks[$i - 1]) is followed by tick $($ticks[$i])" }
    }
    if ($ticks.Count -lt $need) { return "only $($ticks.Count) ticks on screen, wanted $need" }
    return ''
}

# Asks for an upgrade; the daemon serving afterwards must be a successor of the one before, a generation on.
function Upgrade([int]$generation) {
    $before = DaemonPidOf (DaemonStatus)
    $tickBefore = (Ticks)[-1]
    $t0 = Get-Date
    $said = Contour @('daemon', 'upgrade', '--socket', $Socket)
    $t1 = Get-Date
    if ($script:Status -ne 0) { Fail "contour daemon upgrade exited $($script:Status), not 0: $said" }
    if ($said -notmatch 'upgraded') { Fail "contour daemon upgrade does not say it upgraded: $said" }
    if (-not (WaitFor 300 { (Ticks)[-1] -gt $tickBefore })) { Fail 'no new tick appeared within 30s of the upgrade' }
    $t2 = Get-Date
    Write-Host ("live upgrade: the command took {0} ms; the first new tick was on screen {1} ms after it was asked for" `
        -f [int]($t1 - $t0).TotalMilliseconds, [int]($t2 - $t0).TotalMilliseconds)

    $status = DaemonStatus
    $after = DaemonPidOf $status
    $script:Daemons += $after
    if ($after -eq $before) { Fail "the daemon's pid is still ${before}: no successor took over" }
    if (-not (WaitFor 100 { -not (ProcessAlive $before) })) { Fail "the old daemon $before still runs" }
    if ((UpgradesOf $status) -ne $generation) { Fail "the daemon counts $(UpgradesOf $status) upgrades, not $generation" }
    # Lines end in CR LF, which `$` stops at the LF of.
    if ($status -notmatch '(?m)^sessions:\s+1\s*$') { Fail 'the successor does not host exactly the one session' }
    if (-not (ProcessAlive $script:ProgramPid)) { Fail "the hosted program $($script:ProgramPid) died in the upgrade" }
    $taken = Select-String -Path $Log -Pattern "\[$after\].*took over 1 of 1 session\(s\) from release" -Quiet
    if (-not $taken) { Fail "the successor $after did not log taking the session over" }

    # Still counting, and no hole where the handover was: the run on screen reaches from before it to after it.
    if (-not (WaitFor 300 { (Ticks)[-1] -ge $tickBefore + 10 })) { Fail 'the program did not count 10 more ticks within 30s' }
    $ticks = Ticks
    if ($ticks -notcontains $tickBefore) { Fail "tick $tickBefore, the last before the upgrade, is not on screen any more" }
    $gap = GapOf $ticks 20
    if ($gap) { Fail "the counter is not gapless across the handover: $gap" }
    $script:Daemon = $after
}

try {
    # 57 lines, so that the ticks outlive the handover on the screen: over 11 s of them, at one every 200 ms.
    @'
default_profile: main
profiles:
  main:
    terminal_size:
      columns: 91
      lines: 57
'@ | Out-File -Encoding ascii $Config

    @'
"pid $PID" | Out-File -Encoding ascii (Join-Path $args[0] 'program.pid')
$n = 0
while ($true) { $n++; Write-Host "tick $n"; Start-Sleep -Milliseconds 200 }
'@ | Out-File -Encoding ascii (Join-Path $Out 'program.ps1')

    # --- A daemon that loaded its configuration file -------------------------------------------------------------
    $process = Start-Process -FilePath $Contour -ArgumentList "daemon --socket `"$Socket`" --log-file `"$Log`"" `
        -PassThru -WindowStyle Hidden
    $script:Daemon = $process.Id
    $script:Daemons += $process.Id
    if (-not (WaitFor 200 { Test-Path $Socket })) { Fail "the daemon did not bind $Socket within 20s" }

    $created = Contour @('new', 'daemon', $Socket, 'json', 'powershell.exe', '-NoProfile', '-File', (Join-Path $Out 'program.ps1'), $Out)
    if ($script:Status -ne 0) { Fail "contour new exited $($script:Status)" }
    $script:Pane = [regex]::Match($created, '"pane":"([^"]*)"').Groups[1].Value
    if (-not $script:Pane) { Fail 'contour new named no pane' }
    if (-not (WaitFor 200 { Test-Path (Join-Path $Out 'program.pid') })) { Fail 'the hosted program did not start within 20s' }
    $script:ProgramPid = [int]((Get-Content (Join-Path $Out 'program.pid')) -replace 'pid ', '')
    if (-not (WaitFor 200 { (Ticks).Count -ge 10 })) { Fail 'the program printed no ticks within 20s' }

    $status = DaemonStatus
    if ((DaemonPidOf $status) -ne $script:Daemon) { Fail "daemon status does not name the daemon's pid $($script:Daemon)" }
    if ((UpgradesOf $status) -ne 0) { Fail 'a fresh daemon does not count 0 upgrades' }

    # --- Scenario 1: a same-version upgrade --------------------------------------------------------------------
    Upgrade 1

    # --- Scenario 2: a configuration the new release cannot load -----------------------------------------------
    $good = Get-Content $Config -Raw
    "profiles: [`n" | Out-File -Encoding ascii $Config
    $tickRefused = (Ticks)[-1]
    $said = Contour @('daemon', 'upgrade', '--socket', $Socket)
    if ($script:Status -ne 5) { Fail "an upgrade over a broken configuration exited $($script:Status), not 5" }
    if ($said -notmatch "cannot load the daemon's configuration") { Fail "the refusal does not name the configuration: $said" }
    $good | Out-File -Encoding ascii $Config -NoNewline
    $status = DaemonStatus
    if ((DaemonPidOf $status) -ne $script:Daemon) { Fail 'the refused upgrade changed the daemon' }
    if ((UpgradesOf $status) -ne 1) { Fail 'the refused upgrade changed the upgrade count' }
    if (-not (ProcessAlive $script:ProgramPid)) { Fail 'the refused upgrade killed the hosted program' }
    if (-not (WaitFor 300 { (Ticks)[-1] -ge $tickRefused + 10 })) { Fail 'the program stopped counting after the refused upgrade' }

    # --- Scenario 3: the successor upgrades in turn ------------------------------------------------------------
    Upgrade 2

    # --- Stopping it ends the program with it -------------------------------------------------------------------
    Contour @('daemon', 'stop', '--socket', $Socket, '--yes') | Out-Null
    if ($script:Status -ne 0) { Fail "daemon stop exited $($script:Status)" }
    if (-not (WaitFor 100 { -not (ProcessAlive $script:Daemon) })) { Fail 'the daemon was still running 10s after daemon stop' }
    Write-Host 'live upgrade: ok'
    exit 0
}
catch {
    Write-Host $_
    exit 1
}
finally {
    # Killed by pid, never by name: a developer's own contour runs beside this. Whatever daemon serves the
    # socket now too: a successor that took over before the upgrade command or an assertion failed.
    try {
        $serving = DaemonPidOf (Contour @('daemon', 'status', '--socket', $Socket))
        if ($serving) { $script:Daemons += $serving }
    } catch {}
    foreach ($id in @($script:ProgramPid) + $script:Daemons) {
        if ($id -and (ProcessAlive $id)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Milliseconds 500
    Remove-Item -Recurse -Force $Out -ErrorAction SilentlyContinue
}
