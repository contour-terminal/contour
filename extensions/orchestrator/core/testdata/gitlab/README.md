# `glab` output — Recorded, then scrubbed

Recorded with the `glab` named in `VERSION` against a GitLab instance, as the declared commands
(core/Commands.cpp) print it, and scrubbed before it was committed: the recording names real people, hosts,
projects, branches and titles. `../../../harness/scrub-fixture.py` rewrites every string into a placeholder —
users are `user`, the host is `gitlab.example.com`, the project is `group/project`, a title is
`Merge request <n>`, a source branch `feature/branch-<n>` — and keeps what the mapping decides on: every key,
JSON type, number, boolean, null and date, and every enumerated string (`state`, `detailed_merge_status`, …).
There are no e-mail addresses (`ctest -R fixture-privacy`). From this directory:

```
python -I -X utf8 ../../../harness/scrub-fixture.py --shape glab-mrs mrs-recorded.json mrs-open.json
python -I -X utf8 ../../../harness/scrub-fixture.py --shape glab-repo repo-recorded.json default-branch.json
python -I -X utf8 ../../../harness/scale-fixture.py --shape glab-mrs mrs-open.json mrs-100.json
```

| File | Command | What it holds |
| --- | --- | --- |
| `mrs-open.json` | `glab-mrs` | 15 recorded merge requests, then one hand-made (below) |
| `mrs-empty.json` | `glab-mrs` | `[]` |
| `mrs-100.json` | `glab-mrs` | `mrs-open.json` scaled to the 100 the command asks for (`--per-page 100`) |
| `issues-open.json` | `glab-issues` | **hand-made**: two issues in the REST API's documented shape |
| `issues-empty.json` | `glab-issues` | `[]`, as recorded |
| `default-branch.json` | `glab-default-branch` | one recorded project |

**Hand-made.** The last entry of `mrs-open.json` (`iid` 5800, `Merge request 16`) is a copy of the recorded
`not_approved` one whose `source_project_id` differs from its `target_project_id`: none of the recorded merge
requests came from a fork. `issues-open.json` is the documented shape of
https://docs.gitlab.com/api/issues/#list-issues: the account the recording ran under had no issue assigned, so
`glab-issues` printed only `[]`.

**`--output json`, not `-F json`.** `glab mr list` and `glab repo view` take `-F` as `--output`, but
`glab issue list` takes it as `--output-format` (`details`, `ids`, `urls`), and `-F json` there prints a
sentence instead of JSON. `--output json` (`-O json` on `issue list`) prints JSON on all three, so the
commands spell it out.

**What the list holds.** `glab` prints a whole array of the REST API's objects, unprojected, with
`detailed_merge_status` (recorded: `draft_status`, `discussions_not_resolved`, `ci_still_running`,
`not_approved`, `conflict`). It carries no pipeline object and no closing issues: a merge request's CI state is
only what `detailed_merge_status` says (`ci_still_running` Pending, `ci_must_pass` Failing, anything else
None), it names no failing check, and it claims no issue.

Fields the mapping reads (GitLab.cpp): a merge request's `iid`, `title`, `web_url`, `source_branch`,
`target_branch`, `draft`, `source_project_id`, `target_project_id` and `detailed_merge_status`; an issue's
`iid`, `title` and `web_url`; a project's `default_branch`. Every other key is skipped.

**Not run during the recording.** `glab mr checkout {n} --branch {b}` (its `--branch` is as
`glab mr checkout --help` documents it) and `glab auth status` (only its exit status is read).
