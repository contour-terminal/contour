# Status-line extensions

!!! note "Experimental"
    Status-line extensions are new, and the interface they are written against, the
    `terminal-extensions:statusline@0.1.0` world, may still change before it is declared stable.

A status-line extension is a small WebAssembly module that draws part of the indicator status line,
or all of it. It is told what happens in its pane (the working directory changed, a program set a user
variable, the pane gained the focus, ...), and it answers with the text, colours and, when it owns the
whole line, images to show.

Extensions run sandboxed. A module sees no files, no network and no environment; it gets the time and
the pane's state from Contour, and it can run only the commands it declared up front, and only once you
said it may. Every call into an extension has a time limit, and its memory a cap.

Extensions run on a small pool of worker threads shared by every pane -- as many as the machine has
cores, four at most -- and never on the threads that read the terminal's output, parse it or draw it.
Each extension is handed one of the workers, in turn. A slow, frozen or crashing extension therefore never
slows the terminal down: it keeps reading, parsing and drawing at full speed (see
[Performance](#performance)). It can delay the other extensions on its worker, though, by up to its time
limit per call, until it fails often enough to be disabled.

## Configuring extensions

Extensions are configured per profile. Each entry under `extensions:` is one extension; its key is the
name the status line refers to it by.

```yaml
profiles:
    main:
        extensions:
            clock: {}
            git:    { run_command: ask }
            plasma: { config: { text: "{mode}  {title}", speed: "2" } }
            mine:   { path: ~/wasm/mine.wasm, run_command: deny }
```

Key           | Meaning
--------------|----------------------------------------------------------------------------------------------
`path`        | The module to load; a leading `~` is the home directory, and a relative path is taken relative to the directory of the configuration file that declares it. Without a `path` the key selects the [bundled extension](#bundled-extensions) of that name.
`config`      | Text values the extension reads by key; what each extension understands is listed with it.
`run_command` | Whether the extension may run the commands its manifest declares: `allow`, `deny` or `ask`. Default: `ask`. See [Permissions](#permissions).

An extension shows up where the status line names it. There are two ways:

- **As a segment.** `{Extension:<name>}` in one of the indicator status line's templates is replaced by
  the extension's text, with its colours and attributes. Every other placeholder keeps working beside
  it. The long form takes the usual formatting styles: `{Extension:Name=git,Left= ,Right= }`.

  ```yaml
  profiles:
      main:
          status_line:
              indicator:
                  left: " {InputMode:Bold}{Extension:git}"
                  right: "{Extension:clock}"
  ```

- **Taking the line over.** `status_line.extension` names one extension that draws the whole indicator
  status line instead of its templates. Only an extension that takes the line over may draw images
  (Sixel or the kitty graphics protocol); as a segment an extension draws text only.

  ```yaml
  profiles:
      main:
          status_line:
              display: indicator
              extension: plasma
  ```

  A name that is not one of the profile's `extensions` is reported in the log, and the templates are
  used.

Every pane runs its own instance of each of its profile's extensions. Changing `extensions:` and
reloading the configuration (or switching the profile) restarts them; an unchanged set keeps running.

When an extension fails (it traps, exceeds its time limit or its memory, or writes too much output), it
is restarted after a short back-off. Three failures within a minute disable it, and its place shows
`⚠ <name> disabled: <reason>` instead. A module that cannot be found or loaded shows the same notice
right away.

## Bundled extensions

Contour ships these extensions. Name one under `extensions:` without a `path` to use it.

Name           | Shows                                                                                                             | Settings
---------------|-------------------------------------------------------------------------------------------------------------------|---------------------------------------------
`clock`        | The local time of day, `HH:MM`, daylight saving included. Wakes once a minute.                                    | none
`git`          | The branch of the git repository the pane's working directory is in, with `*` when the working tree has changes. Runs `git`, so it asks first. Refreshes when the directory or the foreground process changes (the latter on Linux and macOS) and when the pane gains the focus. | `refresh-ms`: also refresh this often while focused (default `0`, never)
`sysload`      | The system's load: the 1, 5 and 15 minute load averages from `uptime`, or the processor load on Windows. Runs a command, so it asks first. | `refresh-ms`: how often, while focused (default `5000`)
`agent-status` | A badge for the state a coding agent reports in a user variable (`working`, `waiting`, `done`), and the progress it reports (OSC 9;4). | `variable`: the user variable to follow (default `agent_status`)
`plasma`       | An animated plasma drawn with Sixel across the whole line, with text on top. Meant to take the line over; animates only while the pane is focused. | `text`: drawn on top, where `{mode}`, `{title}` and `{tabs}` are filled in; `speed`: how fast the colours cycle (default `1`)

A coding agent, or a hook it runs, sets the variable `agent-status` follows like this:

```sh
printf '\e]1337;SetUserVar=agent_status=%s\a' "$(printf working | base64)"
```

## Permissions

An extension can run a program only if its module declares the command in its
[manifest](#the-manifest), and only as declared: the program and its arguments are fixed, and the
extension fills in only the placeholders the declaration names. No shell is involved.

What `run_command` says decides the rest:

- `deny`: no command runs.
- `allow`: every declared command runs, without asking.
- `ask` (the default): the first time the extension tries to run a command, Contour asks you. The
  question names the extension, the module it was loaded from, and every command it declares, exactly
  as it would run.

  - **Allow** lets it run its declared commands from now on.
  - **Deny** refuses them from now on.
  - Closing the question answers nothing: the extension's commands keep failing, and you are asked
    again once the extension starts anew, in a new pane or after a configuration reload restarts it.

  The call that triggered the question fails at once; nothing waits for your answer. Once you allowed
  it, the extension's next attempt succeeds.

Your answer is remembered for that installation of the extension and for that exact manifest: an extension
configured under another name or loaded from another path, or one whose manifest changed in any way (an
update that adds a command, say), is asked about again. Answers are kept beside the configuration file:

- `extension-grants.txt` for the GUI;
- `extension-grants.daemon.txt` for `contour daemon`, named after the socket it serves, so that daemons
  sharing a configuration directory, and the GUI, keep their answers apart:
  - `extension-grants.daemon.<name>.txt` for a daemon of another name than the default one
    (every character of the name but letters, digits, `.`, `-` and `_` spelled `_`);
  - `extension-grants.daemon[.<name>].<hash>.txt` for a daemon given a `socket` path other than its
    name's own, where `<hash>` is eight hex digits computed from that path.

  A daemon's answers govern commands that run on the daemon's machine.

To take an answer back, delete its line from the file, or the whole file.

## In daemon sessions

A session hosted by [`contour daemon`](persistent-sessions.md) runs its extensions in the daemon,
where the session's terminal lives, and its clients show what they draw. The profile the daemon loads
decides which extensions run; a client's own profile only decides whether the status line is shown.
Mirrored panes run no extensions of their own. A question about an extension's commands is asked in
the attached client, and remembered in the daemon's grants file.

This works with some limits for now:

- **No font events.** The daemon draws in no font, so extensions in daemon sessions are not told the
  font's size or the cell size, and an image an extension draws is sized for a default cell.
- **A dismissed question waits for a reattach.** If you close the question instead of answering it,
  it is asked again only when a client attaches to the session again.
- **No tab events.** The daemon does not tell its extensions about the client's tabs.

## Writing an extension

An extension is a WebAssembly core module built for `wasm32-wasip1` that implements the
`terminal-extensions:statusline` world. The world, at [`wit/statusline/`][wit], is the contract, and its
README spells out what the WIT text cannot: when the host calls what, what it guarantees, and how the
contract is carried in the binary. Any language with a `wit-bindgen` generator works; the bundled
extensions are written in Rust, and so is this walkthrough.

### The world in short

The host calls three functions of the module:

- `load(config)`: once, first, with the extension's `config:` values. It returns the event kinds the
  extension wants to hear about, any of `title`, `cwd`, `foreground-process`, `input-mode`, `tabs`,
  `user-var`, `progress`, `focus` and `font`. `foreground-process` is reported on Linux and macOS only;
  on Windows no such event arrives. It is always told when its own surface is resized, when a
  frame it asked for is due, and the terminal's replies to queries in its output.
- `update(event)`: once per event, in order. It answers `render` when what the extension shows has
  changed, `none` otherwise.
- `render(cols, rows)`: after `load`, and after an `update` that answered `render`. It returns the bytes
  that draw the status line on a terminal of that many cells: text, SGR colours and attributes, cursor
  movement and, when the extension takes the line over, images.

And the module may call four functions of the host:

- `state()`: the pane's current state (working directory, title, focus, user variables, the time and
  the time zone's offset, the font, ...);
- `request-frame(ms)`: asks for a `timer` event; this is how an extension animates, or wakes at the next
  minute. It never sleeps;
- `run-command(name, args)`: runs a command its manifest declares, if allowed;
- `log(level, message)`: writes to Contour's log.

### A Rust crate

```toml
# Cargo.toml
[package]
name = "hello"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[dependencies]
wit-bindgen = { version = "0.63.0", default-features = false, features = ["macros", "realloc"] }

[profile.release]
opt-level = "s"
lto = true
panic = "abort"
```

```rust
// src/lib.rs
wit_bindgen::generate!({
    path: "wit/statusline", // a copy of Contour's wit/statusline directory
    world: "statusline",
});

struct Hello;

impl Guest for Hello {
    fn load(_config: Vec<(String, String)>) -> EventKinds {
        EventKinds::CWD
    }

    fn update(_event: Event) -> RenderRequest {
        RenderRequest::Render
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        let cwd = terminal_extensions::statusline::host::state().cwd;
        format!("\x1b[1m{cwd}\x1b[m").into_bytes()
    }
}

export!(Hello);
```

Build it with the `wasm32-wasip1` target, and point a profile at the module:

```sh
rustup target add wasm32-wasip1
cargo build --release --target wasm32-wasip1
# target/wasm32-wasip1/release/hello.wasm
```

```yaml
profiles:
    main:
        extensions:
            hello: { path: ~/src/hello/target/wasm32-wasip1/release/hello.wasm }
        status_line:
            indicator:
                right: "{Extension:hello}"
```

The module stays a plain core module: do not componentize it. The bundled extensions under
[`extensions/bundled/`][bundled] are complete examples; `clock` is the smallest.

Keep the limits in mind: each call (`load` included) has a deadline (50 ms for `update` and `render`),
the module's memory is capped at 32 MiB, one `render` may return at most 1 MiB, and renders are at
least 16 ms apart. Events are coalesced under load, so an extension sees the latest working directory
rather than every one in between.

### The manifest

A module that runs commands declares them in a custom section named `terminal-extension-manifest`,
which Contour reads before it runs any of the module's code:

```
name = git
version = 1
command status = git --no-optional-locks -C {cwd} status --porcelain=v2 --branch
```

`run-command("status", [("cwd", "/home/me/src")])` then runs exactly that argv, the whole value of
`cwd` as one argument. In Rust, the `manifest` crate under [`extensions/manifest`][manifest] emits the
section:

```rust
manifest::declare_manifest!(
    "name = git\nversion = 1\ncommand status = git --no-optional-locks -C {cwd} status --porcelain=v2 --branch\n"
);
```

A module without a manifest loads and runs; every command it tries fails. The manifest's full grammar,
and the rules for the values filled into a command, are in the [world's README][wit].

#### Version 2

`version = 2` is the manifest of an extension that runs in the daemon. Every line is judged under the
manifest's version: a version-1 manifest that uses one of the lines below is refused. In version 2,
`version` comes before every line but `name`, and a name is declared before any line uses it: a root
before the commands, panes, `read` and `path:` arguments bound to it, an `arg` before its placeholder, a
command or pane before its environment; every name is declared once per kind. A line is read as written:
a carriage return right before the line feed belongs to the line end, a line of nothing but blanks is
skipped, and any other line that begins or ends with a blank is refused rather than trimmed. Words are
separated by single spaces (a tab is refused) and are never unquoted, so a quote in a command line is refused
rather than read as part of a word with a space; a brace appears only as a whole-word `{name}` naming a
declared `arg`, and a command's program is never a placeholder. An environment name is
`[A-Za-z_][A-Za-z0-9_]*`.

An extension sets only the environment Contour lists (`SettableEnvironment.hpp`), each variable to a value of
its listed shape; a manifest that sets anything else is refused when it is read. A variable reaches a program
the user granted, and one that makes it run something else -- `LD_PRELOAD`, `PATH`, `NODE_OPTIONS`, a git
configuration that names a program -- would grant everything. The list:

| Variable | Value |
| --- | --- |
| `GIT_TERMINAL_PROMPT`, `GIT_OPTIONAL_LOCKS` | `0` |
| `GIT_ALLOW_PROTOCOL` | a colon-separated list of `https`, `ssh`, `git`, `file` (never `ext`) |
| `GIT_CONFIG_COUNT` | a decimal number |
| `GIT_CONFIG_KEY_<n>`, `GIT_CONFIG_VALUE_<n>` | a key below, and a value it allows |
| `GIT_ATTR_SOURCE` | a full object name, 40 or 64 lower-case hex digits |
| `LC_ALL` | `C`, `C.UTF-8` or `POSIX` |

The git configuration keys: `push.default` (`nothing`, `current`, `upstream`, `simple`, `matching`),
`push.autoSetupRemote` (`true`, `false`), and three that only turn something off -- `core.fsmonitor` (`false`),
`core.hooksPath` and `core.attributesFile` (`/dev/null`). Any other key -- `core.sshCommand`, `core.pager`,
`credential.helper`, a `*.command`, `alias.*`, `include.*`, `filter.*`, `diff.external` -- is refused. A
command's environment is judged as the command gets it, `command-env-for` laid over `command-env`.

| Line | Example | Means |
| --- | --- | --- |
| `placement <status-line\|daemon>` | `placement daemon` | Where the extension runs; `status-line` without it. |
| `root <label> [create] = config:<pointer>` | `root worktrees create = config:worktree_root` | Directories from the extension's configuration: slash-separated keys, none empty, a whole-key `*` for every element of a list. `create` lets the host create the one directory if it is missing; it is refused on a pointer with `*`. |
| `read <root>` | `read worktrees` | The extension may read files under that root. |
| `arg <name> = <kind>` | `arg path = path:worktrees` | A typed placeholder: `number`, `uuid`, `name`, `ref`, `text`, or `path:<root>` (an absolute, normalised path inside that root). |
| `command <name> <short\|long> @<root> = argv…` | `command git-status short @worktrees = git status --porcelain` | A command, its timeout class (30 s or 10 min), and the root its working directory lies inside. It never runs through a shell. |
| `command-env <KEY> = value` | `command-env GIT_TERMINAL_PROMPT = 0` | Environment for every command. |
| `command-env-for <command> <KEY> = value` | `command-env-for git-status LC_ALL = C` | Environment for one command, applied after `command-env`. |
| `pane <name> @<root> = argv…` | `pane agent @worktrees = claude --model {model}` | A program the extension may open in a tab. |
| `pane-env <pane> <KEY> = value` | `pane-env agent GIT_CONFIG_COUNT = 1` | Environment for one pane. |
| `inbox <name> = text` | `inbox note = A reviewer left a comment.` | Reserved: refused until a version of the manifest supports it. |
| `serve <scheme>` | `serve orchestrator` | The extension serves `<scheme>://` tabs. |
| `action <id> = title` | `action open = Open Agents` | A row in the command palette. |

A conformance corpus of valid and invalid version-2 manifests, which Contour's parser is tested
against, lives in [`extensions/orchestrator/contract/manifest-v2`][corpus].

### The daemon world

An extension with `placement daemon` implements `terminal-extensions:daemon`, at
[`wit/daemon/`][daemon-wit], instead of the status-line world. It runs in `contour daemon` with no
window attached, and its README is the contract's other half.

- `load(config-json, info)`: once, first, with the extension's configuration as JSON, and the roots its
  manifest declares, resolved, with the home directory and the UTC offset.
- `update(hint)`: once per hint, `wake`, `procs`, `panes`, `tabs` or `actions`, each coalesced into one
  pending slot. A hint carries nothing; the extension asks for the current state.

Its 24 imports live under the module `terminal-extensions:daemon/host@0.1.0`. All of them return at once
but `store-set` and `store-remove`, which return once the store is synced; the call's deadline is paused
across them, and a callback may make at most 64 of them or spend 2 s in them.

- `log`, `wake(after-ms)`, `now()`: logging, one pending wake-up, and the clocks;
- `proc-start`, `procs`, `proc-take`, `proc-release`: declared commands, run directly, their results
  kept until released and read in 64 KiB pages;
- `store-get`, `store-set`, `store-remove`, `store-keys`: a durable store of at most 1024 values of up to
  64 KiB each (a larger value or a key past the last is `limit`), keyed by at most 128 bytes of
  `[a-z0-9/._-]`: no leading `/`, and no component that is empty, ends in `.`, contains `.tmp-`, or is a
  reserved device name (`con`, `prn`, `aux`, `nul`, `com1`–`com9`, `lpt1`–`lpt9`, alone or before a `.`).
  Every platform applies the same rules; any other key is `bad-argument` from `store-get`, `store-set` and
  `store-remove`, while `store-keys` takes any prefix. The store's directory is private (mode 0700) and held by one daemon
  at a time through an exclusive `flock` on its lock file; on NFS, `flock` maps to `fcntl` byte-range locks;
- `pane-open`, `pane-close`, `panes`: panes the extension owns;
- `tab-open`, `tab-close`, `tab-reveal`, `tabs`, `tab-read`, `tab-write`: tabs it serves;
- `actions-take`, `summary-set`, `pane-reveal`: its command-palette actions and its summary;
- `file-read`: regular files inside a root declared readable.

A declared command runs only where its manifest binds it, and sees only what its manifest declares:

- **Working directory.** It is opened by handle and judged by the path read back from that handle, so a
  symlink leads nowhere it could not lead by name. A directory belongs to its most specific root: a root
  nested inside another keeps the outer root's commands out. Two roots resolving to the same directory are
  refused when the extension starts.
- **`path:` arguments.** The value is resolved through its longest existing ancestor and must lie strictly
  below its root. Missing directories above its last component are created by Contour, one at a time beneath
  a handle, following no link; a `create` root is created the same way, and never when it would lie inside
  another root. The last component alone reaches the program as a name, because the program (git, say)
  takes a path: a process that can write inside the root can put a symlink there between Contour's check and
  the program's use of it. That is the residual, and it is confined to what is already writable inside the
  root.
- **Environment.** Contour's own variables (`CONTOUR_*`), git's (`GIT_*`, but `GIT_SSH_COMMAND`, `GIT_SSH`
  and `GIT_ASKPASS`) and gh's repository and host selection (`GH_REPO`, `GH_HOST`, `GH_ENTERPRISE_TOKEN`) are
  withheld; everything the manifest declares is added, `command-env-for` replacing `command-env` by name.
- **A missing program** is a refused start, never a vanished working directory.

An import Contour does not serve yet answers `not-supported-yet`, which an extension treats as a
capability it lacks, not a fault. Today that is every pane, tab, action and file import but
`summary-set`.

[wit]: https://github.com/contour-terminal/contour/tree/master/wit/statusline
[daemon-wit]: https://github.com/contour-terminal/contour/tree/master/wit/daemon
[bundled]: https://github.com/contour-terminal/contour/tree/master/extensions/bundled
[manifest]: https://github.com/contour-terminal/contour/tree/master/extensions/manifest
[corpus]: https://github.com/contour-terminal/contour/tree/master/extensions/orchestrator/contract/manifest-v2

## Performance

Extensions run on a small pool of worker threads, never on the threads that read the terminal's output,
parse it or draw it, and nothing on those threads ever waits for an extension: an event an extension's queue
cannot take is dropped, not waited for. A profile without extensions starts none of their machinery: no
WebAssembly runtime, no worker thread, no timer.

### Throughput

Measured on a 32-thread Linux machine with `bench-headless throughput`, a release build, 16 MiB per
workload fed from memory, the indicator status line shown and the terminal focused. Each cell is the
faster of two rounds, run interleaved (one configuration after the other, twice), each round the fastest
of 5 runs. The misbehaving
guest spins until its time limit stops it, on every call, for the whole run (the benchmark restarts it at
once and never disables it):

Workload (primary screen) | No extensions configured | `plasma` animating + a guest spinning to its time limit | Four such guests, every worker busy
--------------------------|-------------------------:|-------------------------------------------------------:|-----------------------------------:
`ascii`                   | 238 MiB/s                | 238 MiB/s                                              | 239 MiB/s
`colored`                 | 161 MiB/s                | 160 MiB/s                                              | 160 MiB/s
`sgr`                     | 95 MiB/s                 | 95 MiB/s                                               | 94 MiB/s
`utf8`                    | 36 MiB/s                 | 36 MiB/s                                               | 36 MiB/s
`tui`                     | 261 MiB/s                | 262 MiB/s                                              | 261 MiB/s

The one dip that repeated is on the alternate screen, which the table does not show: with four guests
keeping every worker busy, `sgr` there ran about 4 % slower in both rounds (98 against 94 MiB/s).

### Memory

Each extension runs as its own instance in every pane: its WebAssembly memory, and a small private
terminal its output is drawn on. Measured with `bench-headless memory` (a release build on Linux, each
pane 120×40 with 1000 lines of scrollback, the indicator shown and focused, refreshed at 60 Hz for 5 s,
the process's resident set before and after):

Panes | No extensions | All five bundled extensions (`plasma` taking the line over and animating) | Cost of the extensions
-----:|--------------:|--------------------------------------------------------------------------:|---------------------:
1     | 4.5 MiB       | 13.2 MiB                                                                   | 8.7 MiB
10    | 21.0 MiB      | 104.2 MiB                                                                  | 8.3 MiB per pane

One extension costs about 1.6 MiB per pane, `plasma` about 2.3 MiB (its image is drawn on its surface, and
again in the pane's status line). An empty module costs 1.4 MiB per pane too: most of an extension's cost
is the private terminal and the runtime's per-instance state every extension has, not what it does.
Shared by all panes, once, are the runtime and its workers: well under 1 MiB.

### Without extensions

A build without extensions compiled in runs within the same few percent: its parser executes the same
number of instructions (Callgrind) as one with them. An idle Contour with no extensions configured wakes
up exactly as often as one built without them (measured on Qt's offscreen platform).

## The WebAssembly runtime

Extensions run on [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)'s fast interpreter, but
nothing in Contour that hosts them names it. They are loaded, instantiated and called through a
runtime-neutral interface (`src/vtextension/wasm/WasmRuntime.hpp`), and the runtime behind it is a backend
chosen when Contour is built: the CMake cache variable `CONTOUR_WASM_RUNTIME` (today its one value is
`wamr`) selects the target `vtextension_<runtime>`. Only what composes a program links that backend, through
`vtextension_runtime`, and creates it with `vtextension::wasm::makeDefaultBackend()`: the GUI,
`contour daemon`, `bench-headless` and the test suites. The extension engine and the daemon's session host
see only the interface. The `check_wasm_runtime_containment` lint gate (`ctest -L lint`) holds them to
it: it fails when a file under `src/` outside the backend directory includes WAMR's C headers or the
backend's own headers, or when the `vtextension` or `vthost` library links WAMR. Bringing in another runtime is adding a backend directory
beside `src/vtextension/wasm/wamr/` and a value of `CONTOUR_WASM_RUNTIME`; the behaviour an extension sees is
pinned by the backend contract tests every backend must pass.

### Adding a backend

A backend for a runtime `<runtime>` is the directory `src/vtextension/wasm/<runtime>/`, and it must provide
all of the following; `src/vtextension/wasm/wamr/` is the one to copy from.

- **A `CMakeLists.txt` defining the target `vtextension_<runtime>`**, which links `vtextension` publicly and the
  runtime itself privately. `src/vtextension/CMakeLists.txt` adds the directory named by
  `CONTOUR_WASM_RUNTIME` and links its target into `vtextension_runtime`; nothing else may link the runtime.
- **A value of `CONTOUR_WASM_RUNTIME`**: `<runtime>` added to `_CONTOUR_WASM_RUNTIMES` in the top-level
  `CMakeLists.txt`, which refuses any value not on that list.
- **`vtextension::wasm::makeDefaultBackend()`** (declared in `src/vtextension/wasm/DefaultBackend.hpp`),
  defined in the backend's target and returning its `IWasmBackend` (WAMR: `DefaultBackend.cpp`).
- **`vtextension::wasm::testing::preparedGeneration()`** (declared in
  `src/vtextension/wasm/testing/PreparedGeneration.hpp`), defined in a `PreparedGeneration.cpp` added to
  `vtextension_test` only: the runtime generation the calling thread was prepared for, 0 if none.
- **The contract suite, instantiated in `vtextension_test`**: a `*_test.cpp` the directory adds to
  `vtextension_test`, holding a traits struct whose `make()` returns a new backend and the line
  `CONTOUR_WASM_BACKEND_CONTRACT(Traits)` (from `src/vtextension/wasm/testing/BackendContract.hpp`). Every
  case tagged `[wasm-contract]` must pass, among them the two rules no backend is exempt from: the runtime
  generation increases on each bring-up, and entering a thread guard fails `RuntimeUnavailable` while no
  runtime is up.
- **A passing containment gate**: `ctest -L lint` (`check_wasm_runtime_containment`) must pass. The gate
  knows WAMR's C headers, backend directory and link target by name (`scripts/check-wasm-runtime-containment.py`); teach it the
  new runtime's alongside, so that they too stay inside the backend directory.
