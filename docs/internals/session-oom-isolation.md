# Session OOM isolation

An out-of-memory kill of a process running inside Contour — a compiler, a language server — must
end that process and nothing else. This page explains why that took code, and how it works.

## Why Contour died with its children

Desktops start applications as systemd units (`app-org.contourterminal.Contour@….service`).
Every process a session spawns lands in that unit's cgroup, beside Contour. Two things then
act on the unit as a whole:

- **`OOMPolicy=stop`** (systemd's `DefaultOOMPolicy`): when the kernel OOM-kills any process in
  the unit, systemd stops the whole unit — Contour and every tab.
- **systemd-oomd** kills whole cgroups under memory pressure, so a session's cgroup that contains
  Contour makes Contour a candidate.

## What happens now

`vtpty::Process::start()` forks the child and **parks** it on a gate — a socket pair — before
`exec`. The parent hands the child, as a `ParkedChild`, to the injected `ProcessPlacement`:

- `SystemdScopePlacement` (Linux, `CONTOUR_WITH_SYSTEMD=ON`) asks the user's systemd, over
  sd-bus, for a transient `contour-session-<contour pid>-<child pid>-<n>.scope` in `app.slice` with
  `OOMPolicy=continue`, waits for the job, and releases the child. Everything the child forks from
  then on is born in that scope.
  systemd before 253 knows neither `PIDFDs` nor `OOMPolicy=` on scopes and refuses a request
  carrying either, so the first such refusal switches the connection to `PIDs=` and no
  `OOMPolicy=`. Its scopes do not react to an out-of-memory kill at all, which is what we want.
  A child released unplaced because the bus fell silent -- its request timed out, or the breaker
  held requests off -- is placed **late**: once a request may be sent again, the worker asks for a
  scope for the running process (by pidfd; by pid only where systemd takes no pidfd). What the
  child started in the meantime stays in Contour's unit. A refusal is not asked again, and neither
  is a child released while there is no systemd user instance at all: that is a configuration, not
  an outage, so its children run unplaced, quietly, with nothing kept to ask again.
- `NoPlacement` (other platforms, Flatpak, builds without systemd) releases at once.

What became of each child is told through an injected `PlacementReporter`, which Contour logs. Every
child released unplaced for a fault is logged where a user sees it, with its pid and why -- each one,
not only the first -- and so is its late placement.

Before parking, the child also sets the `oom_score_adj` its placement supplies -- Contour's own plus 100 -- so in a *global*
out-of-memory the kernel picks a session's process before Contour.

The programs a session starts outside its terminal go through the **same placement**: the opener of a
local file a link names (`xdg-open`), the viewer `OpenCommandOutput` hands a command's output to, the
second Contour that follows a link to an executable file or opens one in `$EDITOR`, and the one
`spawn_new_process` starts. `ExternalLauncher` starts each through `vtpty::startDetached()`, which
double-forks -- the program is not Contour's child, and outlives it -- and parks the program's
process on a gate exactly as `Process::start()` parks a shell. `ContourGuiApp` hands the sessions'
factory and the launcher one placement. Windows starts them through `QProcess`, having no placement.
The daemon `contour client` spawns, and the one `contour daemon --background` starts, go through
`startDetached()` and a placement too: the daemon holds every hosted session's scrollback, and must
live in neither the client's unit nor the scope of the tab it was typed into. `--background`'s
placement lasts only as long as the command does, which returns once the daemon accepts: a daemon
deferred there because systemd fell silent is never placed late, and stays where it was started.

What can still end up in Contour's unit -- a child released unplaced, what a late-placed one
started before, and the shells the status line's `Command` segments run (`CommandCapture`,
`posix_spawn`: one short run per refresh, bounded by a timeout and killed as a process group; a
transient scope per run would churn systemd units every few seconds, per tab) -- no longer takes
Contour down either: the Linux packages install systemd drop-ins
setting `OOMPolicy=continue` on the unit a desktop launches Contour as (Contour cannot change its
own running unit's `OOMPolicy` over the bus, so it is configured instead). Under `<prefix>/lib/systemd/user/`:
`app-org.contourterminal.Contour@.service.d/` for KDE Plasma's launcher (measured: KIO names it
`app-org.contourterminal.Contour@<random>.service`) and XDG autostart, and `…-.scope.d/` for the
launchers that start scopes -- `app-org.contourterminal.Contour-` (KDE's scope launcher),
`app-gnome-org.contourterminal.Contour-` (GNOME Shell), `app-glib-org.contourterminal.Contour-`
(GLib). A drop-in applies to the next unit the desktop creates, and to a running one at the next
`systemctl --user daemon-reload`. Services take `OOMPolicy=` since systemd 243, scopes only since 253:
an older systemd (Ubuntu 22.04's 249, Debian 12's 252) ignores the three scope drop-ins, with an
"Unknown key name 'OOMPolicy' in section 'Scope'" warning per launch, and a desktop that launches
Contour into a scope there leaves it unprotected. An install to a prefix systemd does not search --
`~/.local`, whose `lib/systemd/user` is not on the user search path -- installs drop-ins that have
no effect.

## Invariants worth not breaking

- **Every path releases the child.** Release is `ParkedChild`'s destructor: an exception, a
  refusal, a timeout or shutdown cannot leave a shell parked.
- **Release is a byte sent with `MSG_NOSIGNAL`.** Contour does not ignore `SIGPIPE`, and a child
  killed while parked must not take Contour with it. A byte, not end-of-file, because a sibling
  forked meanwhile inherits the gate's parent end.
- **The child waits on the gate before anything else**, using async-signal-safe calls only.
- **`placeThenRelease` never blocks and never throws**: `Process::start()` runs on the GUI thread
  and reports failures as values. One worker thread owns the sd-bus connection, and starts with the
  first session, with every signal blocked: the daemon waits for `SIGTERM` with `sigwait()`.
- **A child that exited while queued is not asked about.** Its pid may name another process by
  then; the pidfd says whether it is gone.
- **Each request listens for `JobRemoved` only while it lasts**, through a match removed afterwards,
  and picks its own unit's job out; nothing queues up on the connection between spawns. The match
  must not name the unit: sd-bus matches `argN` only up to the first argument that is not a string,
  and `JobRemoved` begins with the job's number, so an `arg2=` match is routed to Contour by the
  broker and then never dispatched -- every request would time out although systemd placed the child.
- **Nothing thrown on the worker ends it, or Contour.** Each child's placement is caught: the child is
  let go, the failure is logged, and a deferred child it concerned is not asked about again.
- **A wedged bus costs one deadline (1 s), not one per tab**: the breaker skips requests for 30 s,
  then lets one trial request decide. Late placements run on the worker, a queued child before
  them, and one of them may be that trial; a tab never waits for more than one deadline per cooldown.
- **A deferred child is placed late only while its pidfd says it lives.** One that exited is let go
  -- its pid may name another process by then -- and one without a pidfd is never kept. A pidfd is
  read as exited once it becomes readable, which a zombie's already is; signal 0 would still reach
  the zombie. Where systemd takes no pidfd (before 253), the late request names the pid, checked
  alive just before: a child that exits, is reaped and has its pid reused within that moment would
  have the unrelated process moved into the scope instead -- harmless, and not guarded further.
- **A late answer is still an answer.** When a request runs into its deadline, the kernel is asked
  where the child runs (`/proc/<pid>/cgroup`, through an injected reader). A child already in the
  scope it asked for was placed. The request still waited out its deadline, though, so the breaker
  counts it as the silence it was, and it is told where a user sees it: answers that go astray for
  good -- as every one did while the `JobRemoved` match named the unit -- cost one deadline per
  cooldown, like a wedged bus, and do not go unnoticed. The price is latency: each late trial whose
  answer goes unheard as well holds requests off again, so deferred children are then placed one per
  cooldown -- after a restore of ten tabs, the last stays in Contour's unit for some five minutes.
  Placing them all in one pass would make the next new tab wait one deadline per deferred child.

## Testing

`vtpty_test "[placement]"` covers the gate, the breaker and the worker through a fake `ScopeBus`.
`vtpty_session_oom_isolation` (CTest label `systemd`) proves the requirement under a real user
manager: a driver running as a unit with `OOMPolicy=stop` must end in `oom-kill` without placement
and in `success` with it -- for a hog on a terminal, and for one started detached, as the launcher
starts programs. It also places a first and a later child through fresh placements and
requires systemd's answer to each to be heard -- systemd moves the child whether or not Contour hears
it, so the hog's cgroup alone cannot tell. CI runs it under a lingering user session and fails if it
skipped.
