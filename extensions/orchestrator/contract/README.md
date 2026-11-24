# The orchestrator's contract with Contour

What the extension and Contour's host must agree on, kept as files both sides test against.

The other half of the contract, the `terminal-extensions:daemon` WIT world the extension is built
against, is Contour's public interface and lives with the host, at `wit/daemon/` in Contour's tree. The
guest's C bindings (`guest/bindings/`) are generated from it; `guest/README.md` has the command.

## `agent-transcript/`

The output of `contour agent transcript --json`, which the orchestrator reads to show a run's transcript:
its README describes format 1, and `format-1/*.json` are pages, each naming the session log it was printed
from. The extension's tests parse every page; Contour's test runs the verb on those logs and compares its
output with the pages byte for byte. A change of format is a `format-2/`, never an edit of format 1.

## `manifest-v2/`

A conformance corpus for version 2 of the extension manifest's line grammar.

- `valid/` holds manifests every conforming parser accepts: one file per line kind, and
  `orchestrator.manifest`, the extension's own manifest. The extension's tests regenerate the latter
  from its declared tables and compare byte for byte, so any change to a declared command, pane,
  argument or environment updates this file in the same commit.
- `valid/orchestrator.consent.txt` is what Contour's consent dialog shows for this manifest: Contour's
  consent test renders the question from `orchestrator.manifest` and compares it with this file, so a
  manifest change and its consent text change together, in one commit.
- `invalid/` holds manifests every conforming parser refuses, one fault per file, each named after it
  and explained by the comment on its first line.

The extension's test suite runs every file through its own line-grammar checker. Contour's host-side
manifest parser runs the same files, so the two sides cannot drift apart: a file that one accepts and
the other refuses fails one of the two suites.

The grammar in brief: a carriage return right before a line feed belongs to the line end, so a manifest
written with CRLF line ends reads as one written with LF (`valid/crlf.manifest`); `#` lines and lines of
nothing but blanks are skipped; any other line neither begins nor ends with a blank, which is refused
rather than trimmed (`invalid/indented-line.manifest`, `invalid/trailing-blank.manifest`); a carriage
return anywhere else, like any other control character, is refused (`invalid/stray-cr.manifest`);
`version` comes before every line but `name`; a line is `keyword words… = value` or `keyword words…`;
words are separated by single spaces (a tab is refused) and are never unquoted, so a quote in an argv is
refused rather than read as a word with a space; a brace appears only as a whole-word `{name}` naming a
declared `arg`; every `command` and `pane` is bound to a declared `@root`; an environment name is
`[A-Za-z_][A-Za-z0-9_]*`; a name is declared before it is used and never twice.

A `root` line may carry one word between its label and its `=`: `create`
(`root worktrees create = config:worktree_root`), which lets the host create the directory if it is
missing. Any other word is refused, and so is `create` on a pointer that walks a list (`*`): only a
single directory can be created. A pointer is slash-separated keys, none empty, and a `*` is a
whole key (`config:repos/*/path`), never part of one.

The `inbox` line kind is reserved: both sides refuse it (`invalid/inbox-reserved.manifest`) until a
version of the manifest supports it.

A `title = <text>` line (version 2) names the extension where Contour shows it: its served tab, its
palette rows and its frames; without one, the `name` stands in. A title is given once, holds at most 40
characters and nothing that would disguise it -- no control character, bidirectional override or isolate,
zero-width character or line separator (`invalid/title-too-long.manifest`,
`invalid/title-control-character.manifest`, `invalid/title-bidi-override.manifest`,
`invalid/title-repeated.manifest`). An `action` line's title follows the same rules
(`invalid/action-title-too-long.manifest`).

A `pane-env-set <pane> {<arg>} = <VAR> <VAR> …` or `command-env-set <command> {<arg>} = <VAR> …` line
(version 2) lets a declared pane or command take variables from one of the user's own variable sets:
the call's `<arg>` names the set, and only the listed variables are taken from it. The argument must be a
declared `arg <arg> = name` (`invalid/env-set-argument-not-name.manifest`,
`invalid/env-set-undeclared-argument.manifest`), written as a whole-word `{<arg>}`
(`invalid/env-set-without-placeholder.manifest`); the pane or command must be declared earlier
(`invalid/env-set-undeclared-target.manifest`) and bound once (`invalid/env-set-repeated.manifest`); and
every variable, listed once, must be on the host's user-settable list -- `CLAUDE_CONFIG_DIR`,
`COPILOT_HOME`, `ANTHROPIC_BASE_URL`, `HTTPS_PROXY`, `HTTP_PROXY` and `NO_PROXY`
(`invalid/env-set-forbidden-variable.manifest`).
