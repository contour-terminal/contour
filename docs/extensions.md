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

### Building a C++ guest

A guest can be C++23 too, compiled to a `wasm32-wasip1` core module with [wasi-sdk] 34 or later. Contour
builds its own C++ guests that way when configured with `-DCONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST=ON`:
`cmake/ContourWasiCxx.cmake` takes the toolchain from `WASI_SDK_PREFIX` (the CMake variable, else the
environment variable), else from the system's clang, `wasm-ld`, `llvm-ar` and `/usr/share/wasi-sysroot`,
proves it by building a C++23 reactor, and fails the configuration naming what is missing. Each guest is
then a CMake project of its own, built by `contour_add_wasm_cxx_guest(NAME … SOURCE_DIR … OUTPUT_VAR …)`
with `cmake/toolchains/wasm32-wasip1.cmake`, offline, into `<build>/wasm/<name>.wasm`.

```sh
cmake --preset clang-asan -DCONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST=ON \
      -DWASI_SDK_PREFIX=$HOME/opt/wasi-sdk-34.0-x86_64-linux
```

The toolchain file compiles with `-fno-exceptions -fno-rtti -fvisibility=hidden` and links with
`-mexec-model=reactor -Wl,--gc-sections -Wl,--strip-debug -Wl,-z,stack-size=65536`. A guest built any
other way must end up with the same shape:

- **A reactor**, not a command: no `main`; `_initialize` runs the static constructors, and the host calls
  it under the load deadline. The module has no start section and does not export `__wasm_call_ctors`.
- **Exports** carry their WIT names, `[[clang::export_name("load")]]`, with `cabi_realloc` beside them.
- **Imports** come from the world's module and nothing else but WASI's allow-list, each declared with
  `__attribute__((import_module("terminal-extensions:daemon/host@0.1.0"), import_name("summary-set")))`.
- **The manifest** is exactly one custom section, written by module-level assembly; the `section` attribute
  produces nothing for a custom section, and `--strip-debug` (not `--strip-all`) keeps it:

  ```cpp
  asm(R"(.section ".custom_section.terminal-extension-manifest","",@
  .ascii "name = hello\nversion = 2\nplacement daemon\n"
  .text
  )");
  ```

[`extensions/testing/cxx-daemon-probe/`][cxx-probe] is the smallest complete example: three imports and
the world's exports declared by hand in the Canonical ABI's flat form, no bindings. `vtextension_test`
checks the built module for the shape above and runs it on the daemon engine.

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
| `title = <text>` | `title = Agents` | What Contour names the extension's served tab, palette rows and frames; the `name` when absent. Given once, at most 40 characters, nothing that disguises text (controls, bidirectional overrides and isolates, zero-width characters, line separators). |
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
| `action <id> = title` | `action open = Open Agents` | A row in the command palette; its title follows the `title` line's rules. |

A conformance corpus of valid and invalid version-2 manifests, which Contour's parser is tested
against, lives in [`extensions/orchestrator/contract/manifest-v2`][corpus].

Each `action` line is a row of the command palette of every window attached to the daemon. What choosing
it does follows where the extension stands: a running one is handed the action (`actions-take`), a stopped
one is started first, one waiting for its grant asks the window for it again (the palette shows that row as
"*title*: grant access…"), and a disabled one is restarted ("*title*: restart extension"). A key binding
invokes the same row, naming the extension as the daemon's configuration does and the action's id:

```yaml
input_mapping:
    - { mods: [Control, Shift], key: A, action: ExtensionAction, extension: orchestrator, id: open }
```

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
across them, and a callback may make at most 64 of them or spend 2 s in them. Past that budget, the next
`store-set` or `store-remove` of the callback answers `limit` and writes nothing: `limit` here means "try
again in a later callback", which starts with a fresh budget, not "the store is full".

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
- `pane-open`, `pane-close`, `panes`: agent panes the extension owns, each running one of its declared
  `pane` templates (the checks a declared command gets: its arguments, a working directory inside the
  template's root, opened by handle and entered by that handle, or not at all). The title is at most 64
  code points, none a control or disguising one; the tag `[A-Za-z0-9][A-Za-z0-9._-]*` of at most 128
  bytes. The pane withholds what a declared command withholds of the daemon's environment (git's and gh's
  repository selection), but keeps the `GIT_CONFIG_COUNT`/`GIT_CONFIG_KEY_n`/`GIT_CONFIG_VALUE_n` pairs and
  appends its template's `pane-env` pairs after them. `pane-open` returns before the pane exists; the pane
  opens beside the served tab `near` names, else where the window focused last looks, without taking the
  focus, and is listed from then on until its session ends. `panes` answers `limit` until the daemon has
  restored its sessions; at most 32 panes per extension. `pane-open` answers `not-supported-yet` where
  Contour cannot start a program in a directory it holds open (a Flatpak whose shells run on the host, and
  Windows), and `gone` for a worktree removed since it was opened;
- `tab-open`, `tab-close`, `tab-reveal`, `tabs`, `tab-read`, `tab-write`: tabs it serves;
- `actions-take`, `summary-set`: its command-palette actions and its summary;
- `pane-reveal`: brings one of its agent panes in front of the window an action came from (its `origin`):
  made the active tab where that window shows the pane's workspace; in another workspace the window asks the
  user first, and a refusal changes nothing. An origin authorises one reveal, within 2 s of the action the
  user invoked: the first reveal consumes it, and the user's next action allows the next one -- a reveal
  hours later would raise a window while the user types elsewhere, and their keys would follow it. A key
  typed or a mouse button pressed into one of its served tabs is a user action too: for at most 2 s after
  it, a reveal without such an origin (0 included) goes to the window that key or click came from -- which
  is how a dashboard reveals from its own key bindings, whose input carries no origin; `tab-reveal` and
  `tab-open` follow the same rule, and a tab `tab-open` creates without such an action behind it opens in the
  background -- created, but no window raised, nothing activated, no focus taken. Past that, an origin it was never handed (0, or
  another extension's), one stale or consumed, or one whose window has gone raises no window, activates no
  tab, and answers `gone`: a reveal needs a user action behind it, just now. A pane it closed answers `gone` until its session ended, `not-owned` after;
- `file-read`: regular files inside a root declared readable, of at most 64 KiB.

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
- **The program** is found before the spawn, on the PATH's absolute entries only, and that path is what
  runs -- for a command and an agent pane alike. An empty entry (the working directory, as `execvp()` reads
  it) or a relative one would find a program an agent dropped into its worktree, so both are skipped, as Go's
  `os/exec` skips them; a program found nowhere else is not found.

`file-read(root, path)` reads beneath the root's directory and nowhere else:

- **The path** is relative and names one entry per component: an empty or absolute path, an empty, `.` or
  `..` component, or a NUL is `bad-argument`. A path into another root nested inside the readable one is
  `not-granted`, as is a root not declared `read`: by its spelling, and in the walk by the nested root's
  directory itself (device and inode), so a case-insensitive filesystem's other spelling is refused too.
- **The root's directory** is opened by handle on the first read and judged by the path read back from it:
  one that resolves elsewhere (swapped for a symlink) is `escapes`. The handle is held from then on, so a
  mount placed over the path later never reaches a read; it is opened again only when it did not exist yet,
  or was removed while held. The nested roots beneath it are identified from that handle once found and kept,
  so renaming one away and back does not lift its fence. A root of several directories is read in
  declaration order; the first in which the path's directories exist answers.
- **The walk** opens each directory beneath the one above it, following no link, and the file without
  blocking: a symlink anywhere on the path is `escapes`, and so is a file with more than one name (a hard
  link may be a second name of a file outside the root) or on another device than the root's (a mount
  brings another tree in). On Linux each component is opened with `openat2(RESOLVE_BENEATH |
  RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV | RESOLVE_NO_MAGICLINKS)`, so a mount point is refused before the
  mounted filesystem is asked anything: a stalled FUSE mount inside a worktree cannot stall the daemon, and
  a bind mount of the same filesystem is refused like any other. Elsewhere, and on kernels without
  `openat2`, the device is compared once a directory is opened. The file is looked at before it is opened
  (an `O_PATH` descriptor on Linux), so a device node is not opened; a pipe, a socket, a device or a
  directory is `not-regular`, and a pipe never blocks the daemon.
- **`directory-gone`** means a directory on the path, the root's own or one below it, does not exist. A
  missing file in an existing directory is `not-regular`: an extension tells a removed worktree from a
  worktree whose `.git` file is gone. A root removed while the read holds it is `directory-gone` too.
- **Fail-closed corners.** A btrfs subvolume or a ZFS dataset inside a worktree has a device of its own, so
  a file in it is `escapes`, as a mount would be.
- **At most 64 KiB** is returned: a file is read up to one byte past that, and one that holds more is
  `too-large`, whatever size it claimed when it was opened.
- On Windows, `file-read` answers `not-supported-yet`.

An import Contour does not serve yet answers `not-supported-yet`, which an extension treats as a
capability it lacks, not a fault. Today that is `file-read` on Windows, and `pane-open` where Contour cannot
start a program in a directory it holds open (a Flatpak whose shells run on the host, and Windows).

### Daemon extensions: granting

A daemon extension runs only once its user granted it, and the grant is bound to its whole module -- its code
as well as its manifest -- **and** the directories its roots resolved to: a rebuilt module, a moved root or
one more repository asks again. (Status-line extensions' grants are bound to the whole module too.) When
an extension is to start (at the daemon's start, or on demand) and has no grant, the daemon asks every
attached Contour window, and again each window that attaches while the question is open:

```
“orchestrator” wants to run in the background
Module: orchestrator@/usr/lib/contour/extensions/orchestrator.wasm
Directory (worktrees, created if missing): /home/u/worktrees
Directory (repos): /home/u/repo/contour
It may run only these, which can reach the network as you:
Each of them with:
  GIT_TERMINAL_PROMPT=0
  …
gh-auth (short, in repos): gh auth status
…
git-status (short, in worktrees): git status --porcelain --untracked-files=normal --ignore-submodules=none
  GIT_CONFIG_COUNT=3
  …
It may open tabs running: agent (in worktrees): claude --model {model} --session-id {session} {prompt}
  GIT_CONFIG_COUNT=2
  GIT_CONFIG_KEY_0=push.default
  …
It may read files in: worktrees
It shows its own tab: orchestrator://
```

Every variable a command or pane is given is shown beneath it, one `NAME=value` line each, and those every
command shares beneath `Each of them with:` -- what is granted is what is shown.

**Only a window grants.** The dialog's Return is Deny, and Allow stays disabled while its window is not
active, and for half a second each time a question is shown or the window becomes active; Escape and the close
control put it away unanswered. The first window to answer settles the question, and the others close theirs.

A window proves it is one: once a daemon holds its socket it makes a fresh secret key for that run, in a
private file of the run's own under its state directory (`consent/<socket hash>-<run>.key`, mode 0600 in a
0700 directory that may not be a link), and removes the files earlier runs at that socket left. A Contour
window on the same machine reads the file of the run the daemon's hello names, and answers the hello's nonce
with an HMAC bound to that run and the socket. A start that never serves -- a `--background` parent, a second
daemon that lost the bind -- never touches the serving daemon's key. Only a connection that
proved the key is asked, and only an answer from a connection that was asked is recorded. So no extension,
no control call or `contour` verb, and no process that merely inherits a pane's environment can grant: the
key's path is in no pane's environment, and the key never crosses the socket. A process of the same user that
reads the key file on purpose can still pass for a window -- as it could fake key presses -- so the user
account is the trust boundary. A window attached to a daemon on another machine holds no key and is not
asked.

```sh
contour extension list            # every daemon extension, its state, and a pending one's question
contour extension status NAME     # what it shows of itself (see below)
contour extension grant NAME      # asks the attached windows again; prints "confirm in a Contour window"
contour extension revoke NAME     # forgets the grant and stops the extension; it asks again
```

`extension grant` with nothing pending fails with `notPending`; with no window attached it says so, and
the question waits for the next window. `extension revoke` stops a running extension and asks again; one that
does not run is asked when it is next started. A refusal is remembered for that module and those
directories, in the daemon's own grants file (`extension-grants.daemon-scoped.txt` beside the configuration).
That file is the daemon's: it is read as the daemon starts and written by it alone, so an edit made while the
daemon runs counts for nothing and is overwritten by its next answer.

### Daemon extensions: what they show

A daemon extension shows itself with `summary-set(short, detail)`: a one-line summary of at most 120
characters and a detail of at most 16 KiB, both counted as the extension gives them; a longer one is refused
with `limit`, and the previous summary stays. Contour neutralizes both before showing them: control, bidi and
zero-width characters read as `<U+XXXX>`, and the detail keeps its line breaks. Neutralizing can make a text
up to eight times longer (a one-byte control character becomes eight bytes), so a detail as shown is at most
128 KiB.

The summary is a status-line segment, `{DaemonExtension:<name>}`, in any profile of a session the daemon
hosts. Every attached window shows it, and it is redrawn as the extension publishes or changes state, once per
daemon loop turn however often it does. It is at most 40 columns wide, cut at its end with `…` past that;
`{DaemonExtension:Name=<name>,MaxWidth=<columns>}` sets another width. A window's own sessions, which no daemon
hosts, show nothing there.

```yaml
profiles:
    main:
        status_line:
            indicator:
                right: "{DaemonExtension:orchestrator} {Clock}"
```

What the segment shows follows from where the extension stands: its summary while it runs; `<name> disabled`
once it faulted too often; `<name> failed` when it was granted and started but cannot run (its module does
not load, say), and `contour extension status` says why; nothing while it is stopped, waiting for its grant,
or denied. A revoked extension's summary goes with its grant.

```sh
contour extension status NAME     # its summary and detail; why it failed, if it did
```

`extension status` prints the summary and then the detail, and exits 0 while the extension runs. One that does
not run exits 1 and says so on standard error: `NAME is not running`, `NAME failed: <why>`, or what it waits
for.

[wit]: https://github.com/contour-terminal/contour/tree/master/wit/statusline
[daemon-wit]: https://github.com/contour-terminal/contour/tree/master/wit/daemon
[bundled]: https://github.com/contour-terminal/contour/tree/master/extensions/bundled
[manifest]: https://github.com/contour-terminal/contour/tree/master/extensions/manifest
[corpus]: https://github.com/contour-terminal/contour/tree/master/extensions/orchestrator/contract/manifest-v2
[cxx-probe]: https://github.com/contour-terminal/contour/tree/master/extensions/testing/cxx-daemon-probe
[wasi-sdk]: https://github.com/WebAssembly/wasi-sdk

## The agent orchestrator

The agent orchestrator is a daemon extension for handing pull-request and issue work to Claude Code agents.
Its tab, `orchestrator://`, lists your open pull requests with their checks and review, the issues your
searches find, the agents you started and the runs that finished. Starting an agent is a key and a
confirmation: the orchestrator makes a git worktree on a branch of its own, opens a tab running `claude` in
it, and removes the worktree when the run ends, unless it still holds work. From the same tab you read what an agent
reports on its board and page through its transcript, also after the run has finished.

Contour's `.deb`, `.rpm`, AppImage and macOS `.dmg` packages carry it as a bundled extension,
`orchestrator.wasm`; the Flatpak build does not include it. Being installed
runs nothing: the daemon loads it only for a `daemon.extensions.orchestrator` entry, and runs it only once a
window has [granted it](#daemon-extensions-granting). A build from source includes it with
`-DCONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST=ON` (see [Building a C++ guest](#building-a-c-guest)).

### Configuring the orchestrator

The orchestrator is configured under `daemon.extensions`, without a `path`, which selects the bundled
module:

```yaml
daemon:
    extensions:
        orchestrator:
            autostart: true
            programs:
                claude: ~/.local/bin/claude
            config:
                worktree_root: ~/worktrees
                models: [opus, sonnet, haiku]
                repos:
                    - path: ~/repo/contour
                      forge: github
                      issues:
                          - { source: forge, query: "assignee:@me" }
                tasks:
                    - { id: fix_ci,         key: f, label: "fix CI",         applies_to: pr,    model: sonnet,
                        prompt: "/contour-workflows:fix-ci {url}" }
                    - { id: address_review, key: r, label: "address review", applies_to: pr,    model: opus,
                        prompt: "/contour-workflows:address-review {url}" }
                    - { id: issue,          key: w, label: "work on issue",  applies_to: issue, model: opus,
                        prompt: "/contour-workflows:work-issue {url}" }
```

Key         | Meaning
------------|------------------------------------------------------------------------------------------------
`path`      | The module to load; without it, the bundled orchestrator.
`autostart` | `true`: start it as the daemon starts. Otherwise it starts when something asks for it, such as its command-palette row.
`programs`  | A map from a program its commands and agent panes run, such as `claude` or `gh`, to the executable to run for it: an absolute path, or one starting with `~/`. Without an entry a program is looked up on the daemon's `PATH`, and a daemon started by systemd or launchd often has one without the user's own bin directories (`~/.local/bin`, a version manager's shims): the orchestrator then reports that `claude` is not installed.
`config`    | The orchestrator's configuration, below. One that does not parse is shown on its tab, one line naming the key, and nothing is read or started until it is fixed.

Its `config`:

Key                   | Meaning
----------------------|------------------------------------------------------------------------------------------
`worktree_root`       | Required. The directory every agent's worktree is made in, created if it is missing. No repository may lie inside it.
`models`              | The models a run may choose. Default `[opus, sonnet, haiku]`. A model with a bracketed variant, such as `opus[1m]`, is refused.
`max_agents`          | The most agents running at once. Default `4`.
`max_agents_per_repo` | The most agents running at once in one repository. Default `2`.
`poll_every`          | How often the forges are asked for the lists. Default `2m`, at least `30s`.
`status_board_after`  | How long an agent runs before the orchestrator starts looking for its board, its `STATUS.md`. Default `15m`; `0` never looks, while `b` still shows a board.
`end_done_after`      | How long an agent stays done before the orchestrator ends its run, as `e` would: its tab closes and its worktree is removed unless it holds work. The run is listed as completed. An agent that goes back to work, because you answered it, waits the whole time again. Done is a turn that ended with nothing in flight, which the dashboard shows as *waiting for you*: it also covers an agent that asked you a question in plain text, which this ends unanswered. An agent waiting on its own background work or a scheduled wake-up is not done, and shows what it waits for. A task can set its own (below). Default `0`: a done run waits for you.
`finished_retention`  | How long a finished run stays in the Finished list. Default `30d`. Every finished run is also appended, for good, to `agent-runs.jsonl` in the daemon's state directory, with what its agent spent and did.
`repos`               | Required. The repositories, one entry each (below).
`tasks`               | Required. What the dashboard offers to start on an item, one entry each (below).

A duration is a number and a unit, `s`, `m`, `h` or `d` (`90s`, `2m`, `1h`, `30d`), or `0`. A limit of `0` is
refused.

A `repos` entry:

Key      | Meaning
---------|------------------------------------------------------------------------------------------------------
`path`   | Required. The repository's checkout; `~` is the home directory.
`name`   | What the dashboard calls it. Default: the name of its directory.
`forge`  | Required. Where it lives: `github`, which the orchestrator reaches with `gh`.
`remote` | The git remote that points at the forge. Default `origin`.
`base`   | The branch an issue's worktree starts from. Default: the forge's default branch.
`issues` | The searches that list the issues offered for work; none lists no issues. `{ source: forge, query: "<search>" }` searches the repository's forge, with `gh issue list --search`.

**GitLab and Jira are enabled once their CLI output is recorded.** The orchestrator knows how to ask `glab`
and Atlassian's `acli`, but it reads no tool's output that was never recorded from the real tool, so for now
the configuration refuses `forge: gitlab` and an issue source `{ source: jira, jql: "<JQL>", site:
"https://<site>" }`, with a line such as `repos[0].issues[0].source: is not yet supported: jira has no
recorded fixture`.

A `tasks` entry:

Key              | Meaning
-----------------|-----------------------------------------------------------------------------------------------
`id`             | Required. The task's name, unique among the tasks.
`key`            | Required. The dashboard key that starts it: one printable character, none of the dashboard's own (the configuration names those when it refuses one).
`label`          | Required. What the dashboard calls it.
`applies_to`     | Required. `pr` for a pull request, `issue` for an issue.
`model`          | Required. The model it runs with, one of `models`; the dispatch form can change it.
`prompt`         | Required. What the agent is started with; `{url}` stands for the item's URL, and must be there.
`end_done_after` | Its runs' own `end_done_after`, over the global one: a task whose agents rarely ask anything, such as fixing CI, can end itself (`5m`) while the rest wait for you, and `0` keeps its runs waiting whatever the global one says. Default: the global one.

An action of the extension opens the dashboard from a key binding, and its summary is a status-line segment;
[Version 2](#version-2) and [Daemon extensions: what they show](#daemon-extensions-what-they-show) have the two
snippets. The binding there is the one to copy:

```yaml
input_mapping:
    - { mods: [Control, Shift], key: A, action: ExtensionAction, extension: orchestrator, id: open }
```

### The dashboard

The list shows pull requests, issues, agents and finished runs, each finished run marked `●` until you open
it; the right half, or the lower half of a narrow tab, shows the selected item's details. `?` lists the keys,
your tasks' among them.

Key                     | Does
------------------------|---------------------------------------------------------------------------------------
`↑` `↓` `j` `k` `Home` `End` | move the selection
`Enter`                 | on a pull request or an issue, open the dispatch form, or show its agent's tab if one runs; on an agent, show its tab; on a finished run, open its details
`d`                     | open the dispatch form on the selected item; on a finished run, on its item, for a new run
a task's `key`          | open the dispatch form on that task
`b`                     | show the selected run's board
`t`                     | show the selected run's transcript
`e`                     | end the selected agent's run
`x`                     | check a kept worktree again
`Del`                   | drop a finished run from the list
`/`                     | filter the rows by text
`R`                     | read the lists again now
`Tab`                   | swap the details in for the list, in a narrow tab
`Esc`                   | go back

The dispatch form shows the item, the task, the model, the prompt, the worktree and the agent's exact command
line. `Enter` starts the agent; `Esc` cancels. Nothing starts without it.

### Boards

An agent that keeps a `STATUS.md` at its worktree's root has a board: `b` shows that file on the tab, with how
long ago it was written (`updated 3 min ago`), and reads it again every five seconds while it is shown. Once an
agent has run for `status_board_after` the orchestrator looks for the file, and the agent's row is marked `▤`
when it finds one.

The orchestrator does not ask an agent for a board; the task's prompt does:

```yaml
- { id: fix_ci, key: f, label: "fix CI", applies_to: pr, model: sonnet,
    prompt: "/contour-workflows:fix-ci {url} -- keep a STATUS.md at the worktree's root: what you are doing and what is left" }
```

A board is read-only and drawn inert: an escape sequence in it is shown as text, a board of more than 64 KiB
is refused with a line saying so, and one that leads outside the worktree through a link is not read. When the
run ends, an untracked `STATUS.md` is removed before the worktree is checked for work, so a board never keeps a
worktree. A finished run has a board only while its worktree was kept; it is read once, as its agent writes no
more, and a board left open when the worktree goes says it was removed when the run ended.

Key                     | Does, on a board
------------------------|---------------------------------------------------------------------------------------
`↑` `↓` `j` `k` `PgUp` `PgDn` `Home` `End` | scroll
`b`                     | read it again now
`t`                     | show the run's transcript
`d`                     | open the dispatch form on the run's item, for a new run
`Enter`                 | show the agent's tab
`Esc`                   | back to where it was opened from: the list or the transcript

### Transcripts

`t` shows the Claude Code transcript of a run, agent or finished, 30 entries a page: the prompts, the agent's
replies, its tool calls and what they returned, each cut at 2 KiB and sanitised. It reads Claude Code's own
session log, so it is there after the run has finished and its worktree is gone, for as long as Claude Code
keeps the log. Each entry shows the time it was written in the local time of that moment, so an entry from
before a daylight-saving change shows the clock as it was then; a time whose offset is not known is shown in
UTC and says so.

Key                     | Does, on a transcript
------------------------|---------------------------------------------------------------------------------------
`←` `→` `Home` `End`    | the previous, next, first and last page
`↑` `↓` `j` `k` `PgUp` `PgDn` | scroll
`b`                     | show the run's board
`d`                     | open the dispatch form on the run's item, for a new run
`Enter`                 | show the agent's tab
`Esc`                   | back to where it was opened from: the list or the board

The orchestrator reads the pages through `contour agent transcript`, a command its manifest declares, so the
grant dialog lists it; the daemon runs it as its own executable, so it works where no `contour` is on the
daemon's PATH, as for a .dmg, an AppImage or a launchd agent. It runs on its own too, its times in UTC:

```sh
contour agent transcript --session <uuid> --cwd ~/worktrees/<run>            # page 1, as text
contour agent transcript --session <uuid> --cwd ~/worktrees/<run> --page 2 --json
```

Option            | Meaning
------------------|------------------------------------------------------------------------------------------------
`--session UUID`  | The Claude Code session, a lower-case UUID.
`--cwd DIRECTORY` | The directory the session ran in: the run's worktree. A log is the session's only when at least one of its entries names the directory it was written in, and every one of them lies in `--cwd` or beneath it.
`--page N`        | The page, from 1. Default `1`.
`--json`          | Print the page as JSON, the format the orchestrator reads, instead of text.
`--format N`      | The JSON format asked for. This Contour prints format `1` and refuses another: `unsupported transcript format <n>; this contour prints 1`.

The log is looked for as `projects/*/<session>.jsonl` under `$CLAUDE_CONFIG_DIR`, else `~/.claude`. A log
that is a link or not a regular file is refused, and so are two logs of one session in the same directory. A
line the format does not know is counted and skipped, never fatal; the page says how many there were.

### Limitations

- **Linux and macOS.** On Windows the daemon opens no agent tab and reads no file for an extension, and the
  Windows packages do not carry the orchestrator.
- **No agent tabs under Flatpak.** Where the shells run on the host, Contour cannot start a program in a
  directory it holds open, so the dashboard lists the work but cannot start an agent.
- **GitHub only**, until GitLab's and Jira's command output is recorded (above).
- **Claude Code only.** The agents are `claude`, and the transcript reads Claude Code's session log.

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
