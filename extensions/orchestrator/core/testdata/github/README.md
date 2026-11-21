# Recorded `gh` output

Recorded with the `gh` named in `VERSION` against contour-terminal/contour, as the declared commands
(core/Commands.cpp) print it.

The `gh-prs` command projects its reply with `--jq` into one positional row per pull request. Its
replies were recorded without the projection (`*.raw.json`) and projected afterwards with the command's
own filter, taken from the golden manifest so that core/Commands.cpp stays its only source. From this
directory:

```
F=$(grep '^command gh-prs ' ../../../contract/manifest-v2/valid/orchestrator.manifest | sed 's/.* --jq //')
jq -c "$F" < prs-open.raw.json > prs-open.json
jq -c "$F" < prs-mixed-checks.raw.json > prs-mixed-checks.json
../../../harness/scale-fixture.py prs-open.json prs-100.json
```

`gh` evaluates the filter with gojq rather than jq: the output has the same structure, compact on one
line, but its bytes may differ in how a string is escaped. GitHub_test checks that every projected row,
closing issue and check has as many members as the filter builds.

`prs-mixed-checks.raw.json` is hand-made rather than recorded: commit statuses, a failing check, a
control sequence in a title and a base that is an option. `prs-100.json` is the largest reply the
command can print: `prs-open.json` scaled up to the 100 pull requests the command asks for.
