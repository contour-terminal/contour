# `acli` output — UNMEASURED

**Nothing here was printed by `acli`.** No `acli` was installed and no Jira site was at hand when the Jira
issue source was written, so these files follow the shape Atlassian documents (the URLs in `VERSION`): the
Jira REST API's issue objects, which `acli jira workitem search --json` is assumed to print as one array. The
issue source and its commands stay `Verification::Unmeasured` — refused by the configuration and absent from
the manifest — until real output replaces these files and `VERSION` names the `acli` that printed it.

| File | Command | Shape assumed |
| --- | --- | --- |
| `issues-open.json` | `acli-issues` | an array of two work items, `ABC-123` and `ABC-7` |
| `issues-bad-key.json` | `acli-issues` | four work items, three of whose keys are no Jira key (lower case, a `;`, an option) |

Fields the mapping reads (Jira.cpp): a work item's `key` and `fields.summary`; every other key is skipped. The
issue's page is never read from the output: it is built as `<site>/browse/<key>` from the configured `site`.

Assumed, and to be confirmed by the recording: that the output is a bare array rather than an object holding
`issues`; that `--limit 100` caps it at 100; that the summary sits at `fields.summary`; and which variables, if
any, select the site `acli` talks to (they would be withheld from commands and panes, as `GH_HOST` is).

Placeholders: users are `user`, the site is `example.atlassian.net`, there are no e-mail addresses
(`ctest -R fixture-privacy`).
