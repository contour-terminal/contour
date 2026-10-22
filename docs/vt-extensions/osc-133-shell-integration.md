# OSC 133 - Shell Integration

This documentation describes the OSC 133 sequence used for shell integration, inspired by FinalTerm.

## Installing Contour's shell integration

Contour ships a script for bash, zsh, fish, tcsh and PowerShell. Write it out once with
`contour generate integration shell <SHELL> to <FILE>`, then load it from your shell's start-up file:

| Shell | Write the script | Load it |
|-------|------------------|---------|
| bash | `contour generate integration shell bash to ~/.config/contour/shell-integration.bash` | at the **end** of `~/.bashrc`: `source ~/.config/contour/shell-integration.bash` |
| zsh | `contour generate integration shell zsh to ~/.config/contour/shell-integration.zsh` | in `~/.zshrc`: `source ~/.config/contour/shell-integration.zsh` |
| fish | `contour generate integration shell fish to ~/.config/fish/conf.d/contour.fish` | nothing to do: fish reads `conf.d/` by itself |
| tcsh | `contour generate integration shell tcsh to ~/.config/contour/shell-integration.tcsh` | in `~/.tcshrc`, after setting `prompt`: `source ~/.config/contour/shell-integration.tcsh` |
| PowerShell | `contour generate integration shell pwsh to "$(Split-Path $PROFILE)/contour-integration.ps1"` | in your `$PROFILE`, after anything that sets up the prompt: `. "$(Split-Path $PROFILE)/contour-integration.ps1"` |

Every script marks the start of the prompt (`A`), its end (`B`), the start of the command's output (`C`)
and the end of the command with its exit status (`D`), and reports the working directory with OSC 7. What
differs is the command line:

| Shell | Command line |
|-------|--------------|
| bash, zsh, fish, PowerShell | sent with `C` as `cmdline_url` |
| tcsh | not sent, as no tcsh hook sees it: Contour reads it off the screen instead (see [`C`](#c-command-output-start)) |

* **bash:** the script carries its own copy of [bash-preexec](https://github.com/rcaloras/bash-preexec),
  which installs itself by appending to `PROMPT_COMMAND`. A later `PROMPT_COMMAND=...` throws that away, and
  then no marks are sent at all, without an error: that is why the script belongs at the end of `~/.bashrc`,
  after anything that sets `PROMPT_COMMAND`. bash hands over a command line only once its history has
  recorded it, so a command kept out of the history (`HISTIGNORE`, `HISTCONTROL`, `set +o history`) is read
  off the screen instead.
* **tcsh:** the script extends `prompt` once, so it is sourced after `prompt` is set.
* **PowerShell:** the script is for PowerShell 7 and needs PSReadLine, which comes with it. Like VS Code's
  integration, it wraps `PSConsoleHostReadLine`, so the command line is reported when PSReadLine hands it
  back — after the newline, before the command runs — rather than from a key handler. Without PSReadLine
  there is no `C`, and so no `D` either.

### Shells that speak OSC 133 themselves

* **fish 4.0** and later mark their prompts natively: `A`, `C` and `D` from fish 4.0.0, the command line
  (`cmdline_url` on `C`) from 4.0.1, and `B` from 4.3.0; `set -Ua fish_features no-mark-prompt` (fish 4.0.6
  and later) turns fish's own marks off. Contour's fish script detects which marks your fish sends and adds
  only those it leaves out, so no mark is sent twice.
* **Nushell 0.111** and later integrate natively and need no script: make sure
  `$env.config.shell_integration.osc133` is `true` (the default); `contour generate integration shell nu`
  says so too. Nushell never sends the command line (`cmdline_url`), so Contour reads it off the screen
  (see [`C`](#c-command-output-start)). Nushell marks every continuation line of a multi-line
  command with `133;A;k=s` and a `133;B` after its indicator; Contour reads an `A` with `k=s` as part of
  the prompt in progress, not as a new command block (see [`A`](#a-prompt-start)). Older Nushell already
  marks its prompts and commands with plain `A`, `B`, `C` and `D`, but leaves the prompt indicator and the
  continuation lines unmarked.

### Windows

PowerShell's marks reach Contour through the Windows console layer (ConPTY) like any other output: the
ConPTY that Contour ships passes them through unchanged, so the PowerShell integration works on Windows as
it does elsewhere. `cmd.exe` has no prompt hook to integrate with; there, Contour behaves as it does without
shell integration.

### What Contour builds on these marks

Folding, the gutter's block column, scrollbar marks, the sticky command header, finish notifications,
copying a command's output and prompt-to-prompt navigation all read these marks —
see [Command blocks](../demo/command-blocks.md). A shell that sends none of them loses nothing it had:
Contour then behaves as a terminal without shell integration always has.

## Sequence Specification

**Format:** `OSC 133 ; <Command> [; <Parameters>...] ST`

Where:

* `OSC` is `\033]`.
* `ST` (String Terminator) is `\033\` (or BEL `\007`).
* `<Command>` is a single character identifier (A, N, B, I, P, C, D, L).

### Commands

#### A - Prompt Start

Sent before the shell prompt starts printing.

**Format:** `OSC 133 ; A [; <Key>=<Value>...] ST`

**Parameters:**

* `click_events=1`: Optional. Accepted for compatibility and currently ignored: Contour does not yet move the cursor on a click inside the prompt.
* `redraw=0`: Optional, as kitty reads it. The shell does not redraw this prompt when the window is resized,
  so Contour re-wraps it like any other text (see below). Each prompt says it for itself; any other value,
  or none, means the shell redraws it.
* `k=<Kind>`: Optional. `i`, or none, is the prompt itself. `c` and `s` start a further line of the prompt in
  progress — a continuation prompt such as bash's `PS2`; Nushell sends `k=s` before each — and `r` a
  right-hand prompt. An `A` (or `N`) of one of those three kinds starts no new prompt and no command block:
  it is read as `P` with the same kind. A kind Contour does not know counts as `i`.

**Behavior:**

* Marks the current line as a prompt line (conceptually similar to setting a "mark").
* Notifies the terminal that the prompt is beginning. A command still running has ended.
* Does no *fresh-line* (see `L`), though the semantic-prompts proposal asks for one: kitty, foot and Konsole
  do none either, and a line break the shell did not print would change what it drew.

**Resizing at the prompt:** a shell redraws its prompt when the window's width changes, moving up as many rows
as it drew the prompt and its input on. Re-wrapped under it as well, the prompt would span other rows than the
shell counts, so Contour leaves the *live* prompt as the shell drew it, as kitty does and the semantic-prompts
proposal recommends:

* The live prompt is the rows from the line the last `A` marked down to the bottom of the page, while the
  cursor stands in it and no `C` has followed.
* Each of those rows is cut at the new width or padded to it and stays a row of its own. The cursor keeps its
  row; the rows above re-wrap as ever. The columns `B`, `D` and `P;k=r` marked move with the cells they name, so
  the input is found where it stands until the shell's redraw marks it afresh.
* A prompt sent with `redraw=0` is re-wrapped like any other text, and so is one that began above the page,
  or that shares its line with output that did not end in a newline.

#### N - New Command

**Format:** `OSC 133 ; N [; <Key>=<Value>...] ST`

**Behavior:** As `A`. The proposal's "may first end the command before" is what `A` does already.

#### B - Prompt End

Sent after the shell prompt has finished printing and before user input begins.

**Format:** `OSC 133 ; B ST`

**Behavior:**

* Notifies the terminal that the prompt has ended.
* Marks the logical line the prompt ended on, together with the column the cursor stood at — the border
  between what the shell painted and what the user is about to type.

This border cannot be recovered after the fact: once the user starts typing, the prompt and the input are
one run of cells. It is what lets Contour tell an accessibility client where the *prompt area* is (see
[Accessibility](../accessibility.md)), and it is recorded whether or not
[DEC mode 2034](semantic-block-query.md) is enabled — the line marks are terminal memory, not part of the
semantic-block reader protocol.

The column is counted across the whole *logical* line, so a window resize that re-wraps the line into
different physical rows does not move it.

Contour's bundled integrations send `;B` at the very end of the prompt. The border is also what lets Contour
read a command line that was never reported off the screen (see [`C`](#c-command-output-start)).

#### I - Prompt End, Input to the End of the Line

**Format:** `OSC 133 ; I ST`

**Behavior:** As `B`, but the user's input ends with the line it begins on: a command line Contour reads off
the screen (see `C`) is that line's text alone. Where both `B` and `I` are sent on one line, the later counts.

#### P - Prompt Part Start

Sent where a further part of the prompt in progress starts, such as a right-aligned prompt.

**Format:** `OSC 133 ; P [; k=<Kind>] ST`

**Parameters:**

* `k=r`: a right prompt, drawn beside the input (Nushell sends it). Other kinds (`i`, `c`, `s`) are
  accepted and change nothing.

**Behavior:**

* Starts no new prompt and no command block.
* With `k=r`, Contour remembers where on its line the right prompt begins, so that the command line it reads
  off the screen when the shell does not report one (see `C`) leaves the right prompt out. Should the command
  later be drawn over the right prompt, as zsh does once the command reaches it, the mark no longer counts.
  On a line `B` marked and no `P;k=r` did, a run of text that ends at the right margin, or one column short of
  it, and stands at least two blanks clear of the command is taken for a right prompt.

#### C - Command Output Start

Sent after the user has finished typing the command (usually on Enter press) and before the command output begins.

**Format:** `OSC 133 ; C [; <Key>=<Value>...] ST`

**Parameters:**

* `cmdline_url=<EncodedURL>`: Optional. The command line being executed, percent-encoded (kitty's
  convention). Contour's bash, zsh, fish and PowerShell integrations send it. Its value runs to the end of
  the sequence, as kitty reads it: a `;` in it belongs to the command, encoded or not.
* `cmdline=<Quoted>`: Optional. The command line quoted for a shell, as kitty's bash and zsh integrations
  send it (`printf %q`), and read as kitty reads it: the first word of the quoted text, with backslash
  escapes, single and double quotes, and `$'...'` ANSI-C quoting. Its value runs to the end of the
  sequence: a `;` in it belongs to the command.

**Behavior:**

* Notifies the terminal that command execution is starting and output will follow.
* The `cmdline_url` and `cmdline` parameters tell the terminal exactly what command is being run. The first
  of them in the sequence is the one read, and as its value runs to the end of the sequence, a parameter
  after it is part of the command line: `OSC 133 ; C ; cmdline_url=;cmdline=ls ST` reports the command line
  `;cmdline=ls`. An empty one, or a `cmdline=` whose quoting is broken, counts as none.
* Contour records the command line with the [command block](../demo/command-blocks.md): the gutter's block
  tooltip, finish notifications and the sticky header of a command whose prompt has left the history show
  it, and `CopyCommandLine` copies it. Without one — tcsh, Nushell,
  or a hand-written prompt — Contour reads it off the screen when the prompt's end was marked with `B` (or
  `I`): from the `;B` column to the start of the output, leaving a right prompt out. So `;B` is worth
  sending even where the command line is not.
* A command line is untrusted input: anything that writes to the terminal can send one. Contour keeps
  at most 4 KiB of it, shows it with control and bidirectional-override characters replaced by a
  visible placeholder, and removes escape and other control characters (but tab and newline) from the
  copy `CopyCommandLine` puts on the clipboard.

#### D - Command Finished

Sent after the command has finished executing and before the next prompt starts.

**Format:** `OSC 133 ; D [; <ExitCode>] ST`

**Parameters:**

* `<ExitCode>`: The integer exit code of the command (e.g., `0` for success). A missing or non-numeric one
  reads as `0`.

**Behavior:**

* Notifies the terminal that the command has finished.
* Reports the exit code of the command.
* Contour records the exit status with the command block: it colors the block's marks in the gutter and
  on the scrollbar, labels the sticky header, and decides whether a finish notification set to
  `outcome: failure` is sent.

#### L - Fresh Line

**Format:** `OSC 133 ; L ST`

**Behavior:** Moves the cursor to the start of the next line, as `\r\n` does, unless it already stands at the
start of a line -- where a carriage return would leave it.

---

## Example Flow

```bash
# Prompt Start
printf "\033]133;A\033\\"

# ... print prompt ...
printf "user@host:~$ "

# Prompt End
printf "\033]133;B\033\\"

# ... user types command (e.g., 'ls') ...

# Command Output Start
printf "\033]133;C\033\\"

# ... command output ...
ls

# Command Finished (exit code 0)
printf "\033]133;D;0\033\\"
```

## Related Extensions

* **SETMARK (`CSI > M`)**: a Contour-only extension that marked the current line, equivalent to
`OSC 133 ; A`. Deprecated in favour of OSC 133 and **removed in 0.8.0** — use `OSC 133 ; A`.
