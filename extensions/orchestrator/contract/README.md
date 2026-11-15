# The orchestrator's contract with Contour

What the extension and Contour's host must agree on, kept as files both sides test against.

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

The grammar in brief: `#` lines and blank lines are skipped; a line is `keyword words… = value` or
`keyword words…`; argv words are separated by single spaces and are never unquoted, so a quote in an
argv is refused rather than read as a word with a space; a brace appears only as a whole-word
`{name}` naming a declared `arg`; every `command` and `pane` is bound to a declared `@root`; a name is
declared before it is used and never twice.
