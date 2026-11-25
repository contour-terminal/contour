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
site's own workflow name, the committed file carries a stock Jira name of the same status category in its
place: statuses are `To Do`, `Selected for Development`, `In Progress`, `In Review` and `Done`, issue types
`Bug`, `Task`, `Epic`, `New Feature` and `Improvement`. The mapping reads a status's name only to show it.

| File | Command | What it holds |
| --- | --- | --- |
| `issues-open.json` | `acli-issues` | 31 recorded work items |
| `issues-empty.json` | `acli-issues` | `[]`, as recorded for a search matching nothing |
| `acli-bad-jql.stderr` | `acli-issues` | what it writes on stderr for a JQL it refuses, in recorded form with a placeholder field value |
| `issues-bad-key.json` | `acli-issues` | **hand-made**: four work items, three of whose keys are no Jira key (lower case, a `;`, an option) |
| `issues-work.json` | `acli-issues` | **hand-made**, in the recorded shape: five `PRJ-` work items, one per status the Work board groups — `In Progress`, `Selected for Development` and `To Do` (both `new`), `In Review`, and a last `indeterminate` one, `PRJ-1302`, that no merge request names and whose status name starts with `\u001b[31m`, which sanitising removes |
| `issues-lookup-work.json` | `acli-issues` | **hand-made**, in the recorded shape: the key lookup's answer for the Work board's screens, one work item, `PRJ-1999`, `In Review` (`indeterminate`) |

**What was measured.** `acli jira workitem search --jql … --fields key,summary,status,issuetype --limit 100 --json`
prints one JSON array, indented by two, of the Jira REST API's issue objects (`expand`, `id`, `key`, `self`,
`fields`, and `changelog`, `editmeta`, `names`, `operations`, `properties`, `renderedFields`, `schema`,
`transitions`, `versionedRepresentations`, `fieldsToInclude` as `null`); `fields` holds `summary`, `status`
(with its `statusCategory`) and `issuetype`. Without `--fields` it holds `priority` and `assignee` too. acli
1.3.41's search allows only `key`, `summary`, `status`, `issuetype`, `assignee`, `priority`, `reporter`,
`creator`, `labels` and `description` in `--fields`: any other (`updated`, `created`, `*all`, …) exits 1 with
`Error: field '<name>' is not allowed` on stderr. So a work item's update time cannot be read from the search,
and the mapping leaves it unknown (`Issue::updatedWallMs` is 0). A search matching nothing prints `[]` and exits
0. A JQL it refuses exits 1 with nothing on stdout and one line
on stderr. `acli jira auth status` exits 0 when logged in.

**Assumed, not measured.** That `--limit 100` caps the array at 100 entries: the recorded search matched fewer.

Fields the mapping reads (Jira.cpp): a work item's `key`, `fields.summary`, `fields.status.name` and
`fields.status.statusCategory.key` (`new`, `indeterminate`, `done`); every other key is skipped. Each work
item's position in the array is its `listRank`, so the search's own order (its JQL's `ORDER BY`) survives. The
issue's page is never read from the output: it is built as `<site>/browse/<key>` from the configured `site`.
`site` builds those links and nothing else: `acli` searches whichever site it is logged in to
(`acli jira auth status` shows it), so the two must match.
