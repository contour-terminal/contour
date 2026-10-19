# Links in Terminal Output

Contour underlines URLs, file paths and patterns you define in the output of your
programs, the way it shows [OSC 8 hyperlinks](../vt-extensions/clickable-links.md),
about 100 ms after the output goes quiet. <kbd>Ctrl</kbd>+click opens them.

``` yaml
profiles:
  main:
    hyperlink_decoration:
      mode: always              # always | hover | modifier | never
      normal: dotted-underline
      hover: underline
    file_opener:
      command: 'code --goto {path}:{line}:{column}'
      placement: detached       # detached | split | tab
    hint_patterns:
      - name: url
        clickable: false        # stops underlining URLs: a built-in, changed by its name alone
      - name: jira
        regex: '\b[A-Z][A-Z0-9]+-\d+\b'
        clickable: true
        url: 'https://jira.example.com/browse/{0}'
```

Everything here has a default, so a configuration without any of these keys
detects links as described below.

## What is detected

- **URLs** with the schemes `http`, `https`, `ftp`, `file` and `mailto`.
  Trailing punctuation is left out of the link, so the full stop after a URL at
  the end of a sentence is not part of it; a closing bracket the URL opened
  itself is kept, as in `https://en.wikipedia.org/wiki/Foo_(bar)`. A `file://`
  URL is the file it names, and is a path in all that follows: underlined only
  once it is found on this machine, and opened as a file is. Contour never runs
  it; without a `file_opener` it goes to the application your system associates
  with it, which on Windows and macOS may run a program.
- **File paths**: absolute (`/usr/include/stdio.h`, `C:\Users\me\notes.txt`),
  relative (`src/main.cpp`, `../build/log.txt`), home-relative (`~/notes.md`),
  and bare file names (`Makefile`, `main.cpp`). A location printed after a path,
  `:line[:column]` or `(line[,column])` as compilers print them, is kept and
  passed on when the file is opened.
- **Patterns of your own**, see [below](#your-own-clickable-patterns).

A path is underlined only once Contour has checked that it exists on this
machine. A relative path is resolved against the directory of the command that
printed it, which the shell reports through
[shell integration](../vt-extensions/osc-133-shell-integration.md) (OSC 133 and
OSC 7), so a `cd` since then does not change what an earlier command's output
names. A path printed by a command that ran on another machine, over `ssh` or in
a container, names a file there and is never underlined, once Contour can tell:
the remote shell reports the directory it runs in, with its host, through its own
shell integration (OSC 7). Without that, everything an `ssh` session prints
belongs to the local `ssh` command, and its paths are looked up on this machine.

## When links are shown

`hyperlink_decoration` decides when links are decorated, for OSC 8 hyperlinks
and detected links alike:

| `mode`     | Links are decorated                                                     |
|------------|-------------------------------------------------------------------------|
| `always`   | always (the default)                                                    |
| `hover`    | only the one under the mouse pointer                                    |
| `modifier` | all of them while the modifier of Ctrl+click is held                    |
| `never`    | never; Ctrl+click still opens them                                      |

`normal` is the decoration of a link at rest and `hover` the one of the link
under the pointer. Each is one of `underline`, `dotted-underline`,
`double-underline`, `curly-underline`, `dashed-underline`, `overline`,
`crossed-out`, `framed` and `encircle`. The colours of both live in the colour
scheme, see [`hyperlink_decoration`](colors.md#hyperlink_decoration).

## Opening links

- **Ctrl+click**: the `FollowHyperlink` mouse binding. Inside an application
  that uses the mouse itself, such as `vim` or `tmux` with mouse support, add the
  global `bypass_mouse_protocol_modifier` (Shift by default): Shift+Ctrl+click.
- **The context menu**: right-click a link for **Open Link** and **Copy Link
  Address**.
- **Vi normal mode**: `gx` opens the link under the cursor, and `]x` / `[x` move
  the cursor to the next / previous link (see [Input Modes](../input-modes.md)).
- **Hint mode**: `gH` in vi normal mode, or a `HintMode` binding with
  `hint_action: Open` (see [Hint Mode](../demo/hint-mode.md)).

A web URL opens in your browser. A path, or a `file://` URL, opens as what it
names: a directory in your file manager, and a file with `file_opener`, or with
the application your system associates with it. That application is the
system's choice: on Windows and macOS, a program it is handed may run.

## `file_opener`

`file_opener` says how a detected file opens. The string form is the command
alone; the map form adds where it runs:

``` yaml
profiles:
  main:
    file_opener: 'code --goto {path}:{line}:{column}'
```

``` yaml
profiles:
  main:
    file_opener:
      command: 'nvim +{line} {path}'
      placement: split    # detached (the default), split or tab
```

- `{path}` is the file's absolute path, `{line}` and `{column}` the location
  printed after it, `1` where none was printed.
- No shell is involved: the command line is split into arguments once, and each
  placeholder is filled in inside its own argument, so a file name with spaces
  or quotes in it stays one argument and cannot run anything.
- `placement: detached` starts the command as a program of its own; `split` and
  `tab` run it in a new Contour pane, in the directory a relative path was
  resolved against or, for an absolute path, the file's own directory.
- A directory always opens in the file manager. Without `file_opener`, a file
  opens with the system's default application.
- Write the program out, without a placeholder. A program path with a space in
  it is read as one program where that path exists on this machine; where it does
  not, or under flatpak with `escape_sandbox`, it is split at the space. Use a
  wrapper script on a path without spaces in that case.
- On Windows, name an `.exe` as the program. A `.cmd` or `.bat` file is run by
  `cmd.exe`, which parses its command line again, so a file name could change
  what it runs.

## Your own clickable patterns

The patterns of [hint mode](../demo/hint-mode.md#custom-patterns),
`hint_patterns`, are detected in output too when you say so:

``` yaml
profiles:
  main:
    hint_patterns:
      - name: jira
        regex: '\b[A-Z][A-Z0-9]+-\d+\b'
        clickable: true
        url: 'https://jira.example.com/browse/{0}'
      - name: commit
        regex: '\b[0-9a-f]{7,40}\b'
        clickable: true
        url: 'https://github.com/contour-terminal/contour/commit/{0}'
```

- `clickable: true` detects the pattern in output, decorates it and opens it with
  Ctrl+click. Your own patterns are hint-mode-only unless they say so.
- `url` is what is opened: `{0}` is the whole match, `{1}` to `{9}` its capture
  groups. Without it, the match is opened as printed. A `url` naming a group the
  regex does not have is reported when the configuration is loaded and disables
  the pattern.
- Where the printed text chooses the scheme, there is a link only for a scheme
  Contour opens in URLs it finds itself: `http`, `https`, `ftp`, `mailto` or
  `file`. That is so without a `url`, and with a `url` that does not start with
  a scheme of its own, such as `{0}` or `{1}:{2}`. Printed text cannot open a
  `smb:` share or a `ms-msdt:` handler. A `url` that writes its scheme itself,
  as `https://jira.example.com/browse/{0}` does, may open any scheme: you chose
  it. Hint mode still offers such a match, to copy, and opens nothing.
- A group is percent-encoded only where a URL cannot hold a character: spaces,
  control characters and `` "#<>[\]^`{|} ``. `/ ? & = + @ : %` pass through, so a
  group placed in a query can add parameters, and one placed before the host can
  change it. Write the scheme and the host in the template, groups after them.
- An entry named like a built-in (`url`, `filepath`, `githash`, `ipv4`, `ipv6`)
  changes only the keys it gives, and may leave out its `regex`. A built-in takes
  no `url`; a `url` given to one is reported and ignored.

``` yaml
profiles:
  main:
    hint_patterns:
      - name: filepath
        clickable: false   # stop underlining paths; hint mode still offers them
```

A regex that overrides `filepath` names the path with capture group 1, and its
location with groups 2 and 3 (`:line[:column]`) and 4 and 5 (`(line[,column])`).

Patterns are ECMAScript regular expressions, with these limits:

- Back-references (`\1` to `\9`) are not supported: a pattern using one is
  reported when the configuration is loaded and disabled, on every platform.
- A non-ASCII character inside `[…]` matches byte by byte, not as one character;
  such a pattern is reported with a warning.
- On Linux, Contour is built with libstdc++, whose matcher runs breadth-first:
  where one alternative's match is a prefix of another's at the same place, as in
  `foo|foobar`, it takes the longer one, and lazy quantifiers may stop elsewhere,
  where macOS (libc++) and Windows take the first alternative ECMAScript names.
  Write patterns whose result does not depend on which alternative or which
  repetition count wins.
- A clickable pattern must not match the empty string; one that can is disabled.

## The settings page

Under **Profiles**, the **Links** group holds **Show links**
(`hyperlink_decoration.mode`), **Normal style**, **Hover style**, **Open files
with** (`file_opener.command`) and **Open in** (`file_opener.placement`). The
**Links: opening and patterns** section names the gesture that opens links and
lists the patterns: the built-ins, whose Clickable switch you can change, and the
profile's own, which you can add, edit and remove. Below the list, a sample text
shows what the selected pattern finds, checked and matched as the terminal does;
a problem with a pattern is shown under it.

## Performance

Detection stays off the path the terminal reads output on:

- The output is scanned once it has gone quiet, at most once a second under
  continuous output, on a background thread. The parser never runs a pattern.
- Whether a path exists is checked on a thread of its own, so a slow network
  mount delays the underlines and nothing else.
