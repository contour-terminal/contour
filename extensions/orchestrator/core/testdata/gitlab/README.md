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
python -I -X utf8 ../../../harness/scrub-fixture.py --shape glab-pipelines pipelines-recorded.json pipelines.json
python -I -X utf8 ../../../harness/scrub-fixture.py --shape glab-jobs jobs-recorded.json jobs-failed.json
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
| `pipelines.json` | `glab-pipelines` | 100 recorded pipelines, newest first (recorded 2026-11-25) |
| `jobs-failed.json` | `glab-failed-jobs` | 100 recorded failed jobs (recorded 2026-11-25) |
| `pipelines-hand.json` | `glab-pipelines` | **hand-made** against `mrs-open.json`'s first three entries (below) |
| `jobs-failed-hand.json` | `glab-failed-jobs` | **hand-made**, the failed jobs of `pipelines-hand.json` (below) |

**Hand-made.** The last entry of `mrs-open.json` (`iid` 5800, `Merge request 16`) is a copy of the recorded
`not_approved` one whose `source_project_id` differs from its `target_project_id`: none of the recorded merge
requests came from a fork. `issues-open.json` is the documented shape of
https://docs.gitlab.com/api/issues/#list-issues: the account the recording ran under had no issue assigned, so
`glab-issues` printed only `[]`.

**Hand-made pipelines and failed jobs.** `pipelines-hand.json` and `jobs-failed-hand.json` join with the first
three merge requests of `mrs-open.json` by their `sha`: 5752 (`111…`) has pipeline 900 `failed` and an older
pipeline 880 `success` on the same commit, so the higher id decides; 900's failed jobs are `test-windows` and
`lint`, `lint` twice (a retried job fails again under the same name), and 880's is `old`. 5746 (`222…`) has
pipeline 901 `success`, 5744 (`333…`) pipeline 902 `running`; no other merge request has a pipeline. The
recorded pair cannot show this join: the scrubber gives each entry's `sha` from its index in its own list.

**`--output json`, not `-F json`.** `glab mr list` and `glab repo view` take `-F` as `--output`, but
`glab issue list` takes it as `--output-format` (`details`, `ids`, `urls`), and `-F json` there prints a
sentence instead of JSON. `--output json` (`-O json` on `issue list`) prints JSON on all three, so the
commands spell it out.

**What the list holds.** `glab` prints a whole array of the REST API's objects, unprojected, with
`detailed_merge_status` (recorded: `draft_status`, `discussions_not_resolved`, `ci_still_running`,
`not_approved`, `conflict`). It carries no pipeline object and no closing issues: on its own, a merge request's CI
state is only what `detailed_merge_status` says (`ci_still_running` Pending, `ci_must_pass` Failing, anything else
None), it names no failing check, and it claims no issue.

**Pipelines and failed jobs.** `glab api projects/:id/pipelines?per_page=100&order_by=id&sort=desc` and
`glab api projects/:id/jobs?scope[]=failed&per_page=100` (`glab api` fills `:id` from the repository it runs in)
print the REST API's arrays unprojected. A merge request whose `sha` matches a listed pipeline takes the
highest-id one's `status` (`success`, `skipped` Passing; `failed`, `canceled` Failing; anything else Pending)
and names that pipeline's failed jobs; one without a match keeps what `detailed_merge_status` says. Recorded
pipeline statuses: `success`, `failed`, `canceled`, `running`; failed jobs' `failure_reason`: `script_failure`,
`job_execution_timeout`, `data_integrity_failure`.

Fields the mapping reads (GitLab.cpp): a merge request's `iid`, `title`, `web_url`, `source_branch`,
`target_branch`, `draft`, `source_project_id`, `target_project_id`, `detailed_merge_status`, `has_conflicts`,
`updated_at` and `sha`; an issue's `iid`, `title` and `web_url`; a project's `default_branch`; a pipeline's
`id`, `sha` and `status`; a job's `name` and `pipeline.id`. Every other key is skipped.

**Not run during the recording.** `glab mr checkout {n} --branch {b}` (its `--branch` is as
`glab mr checkout --help` documents it) and `glab auth status` (only its exit status is read).
