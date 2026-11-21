# `glab` output — UNMEASURED

**Nothing here was printed by `glab`.** No `glab` was installed and no GitLab account was at hand when the
GitLab provider row was written, so these files follow the GitLab REST API's **documented** response shapes
(the URLs in `VERSION`), which `glab … -F json` is documented to print unprojected. The provider row and its
commands stay `Verification::Unmeasured` — refused by the configuration and absent from the manifest — until
real output replaces these files and `VERSION` names the `glab` that printed it.

| File | Command | Shape assumed |
| --- | --- | --- |
| `mrs-open.json` | `glab-mrs` | an array of merge requests: a same-project ready one (`mergeable`), a draft from a fork (`source_project_id` ≠ `target_project_id`, `not_approved`), one with `requested_changes` against `release/1.2` |
| `mrs-empty.json` | `glab-mrs` | `[]` |
| `mrs-100.json` | `glab-mrs` | `mrs-open.json` scaled to the 100 the command asks for (`--per-page 100`): `../../../harness/scale-fixture.py --shape glab-mrs mrs-open.json mrs-100.json` |
| `issues-open.json` | `glab-issues` | an array of two issues |
| `default-branch.json` | `glab-default-branch` | one project object |

Fields the mapping reads (GitLab.cpp): a merge request's `iid`, `title`, `web_url`, `source_branch`,
`target_branch`, `draft`, `source_project_id`, `target_project_id` and `detailed_merge_status`; an issue's
`iid`, `title` and `web_url`; a project's `default_branch`. Every other key is skipped.

Assumed, and to be confirmed by the recording: that `glab` prints a whole array (not one object per line);
that it prints `detailed_merge_status`; that the list carries no pipeline status and no closing issues (so
GitLab change requests show no CI state and claim no issue); that the `--author` and `--assignee` filters
meaning "me" are accepted in the spelling `glab-mrs` and `glab-issues` use.

Placeholders: users are `user`, the host is `gitlab.example.com`, there are no e-mail addresses
(`ctest -R fixture-privacy`).
