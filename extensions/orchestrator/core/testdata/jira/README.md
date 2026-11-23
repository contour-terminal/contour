# `acli` output — Recorded, then scrubbed

Recorded with the `acli` named in `VERSION` against a Jira Cloud site, as the declared `acli-issues` command
(core/Commands.cpp) prints it, and scrubbed before it was committed: the recording names real people, sites,
projects and titles. `../../../harness/scrub-fixture.py` rewrites every string into a placeholder — users are
`user`, the site is `example.atlassian.net`, keys are `ABC-<n>`, a summary is `Work item <n>` — and keeps what
the mapping decides on: every key, JSON type, number, boolean, null and date, and every enumerated string (a
status category, an issue type, a priority). There are no e-mail addresses (`ctest -R fixture-privacy`). From
this directory:

```
python -I -X utf8 ../../../harness/scrub-fixture.py --shape acli-issues issues-recorded.json issues-open.json
```

The script keeps an issue type's, a status's and a priority's name as recorded. Where the recording held a
site's own workflow name, the committed file carries a stock Jira name in its place (`New Feature`,
`Improvement`, `Under Review`, `Waiting for support`); nothing reads them.

| File | Command | What it holds |
| --- | --- | --- |
| `issues-open.json` | `acli-issues` | 29 recorded work items |
| `issues-empty.json` | `acli-issues` | `[]`, as recorded for a search matching nothing |
| `acli-bad-jql.stderr` | `acli-issues` | what it writes on stderr for a JQL it refuses, in recorded form with a placeholder field value |
| `issues-bad-key.json` | `acli-issues` | **hand-made**: four work items, three of whose keys are no Jira key (lower case, a `;`, an option) |

**What was measured.** `acli jira workitem search --jql … --limit 100 --json` prints one JSON array, indented by
two, of the Jira REST API's issue objects (`expand`, `id`, `key`, `self`, `fields`, and `changelog`, `editmeta`,
`names`, `operations`, `properties`, `renderedFields`, `schema`, `transitions`, `versionedRepresentations`,
`fieldsToInclude` as `null`); `fields` holds `summary`, `status`, `issuetype`, `priority` and `assignee`. A
search matching nothing prints `[]` and exits 0. A JQL it refuses exits 1 with nothing on stdout and one line
on stderr. `acli jira auth status` exits 0 when logged in.

**Assumed, not measured.** That `--limit 100` caps the array at 100 entries: the recorded search matched fewer.

Fields the mapping reads (Jira.cpp): a work item's `key` and `fields.summary`; every other key is skipped. The
issue's page is never read from the output: it is built as `<site>/browse/<key>` from the configured `site`.
`site` builds those links and nothing else: `acli` searches whichever site it is logged in to
(`acli jira auth status` shows it), so the two must match.
