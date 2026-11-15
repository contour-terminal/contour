# The orchestrator's contract with Contour

What the extension and Contour's host must agree on, kept as files both sides test against.

The other half of the contract, the `terminal-extensions:daemon` WIT world the extension is built
against, is Contour's public interface and lives with the host, at `wit/daemon/` in Contour's tree.

## `manifest-v2/`

A conformance corpus for version 2 of the extension manifest's line grammar.

- `valid/` holds manifests every conforming parser accepts: one file per line kind, and
  `orchestrator.manifest`, the extension's own manifest. The extension's tests regenerate the latter
  from its declared tables and compare byte for byte, so any change to a declared command, pane,
  argument or environment updates this file in the same commit.
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
