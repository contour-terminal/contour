#! /usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Scrubs a recorded `glab` or `acli` reply into a fixture that can be committed.

A reply recorded against a real GitLab or Jira names real people, hosts, projects, branches and titles. This
rewrites it into placeholders -- users are `user`, the host is `gitlab.example.com` or `example.atlassian.net`,
the project is `group/project`, Jira keys are `ABC-<n>` -- while keeping everything a mapping decides on: the
key set at every level, every JSON type, every number, boolean, null and date, every enumerated string
(`state`, `detailed_merge_status`, a status category, ...) and every empty string.

What happens to a string is decided by a table per shape (`SHAPES`), keyed by its JSON path: an object's keys
joined by `.`, an array's element written `[]` (`[].author.web_url`), `*` matching any one key. A rule is:

- `keep`            the recorded string, kept (an enumeration, a date, a branch every project has);
- `text:<prefix>`   `<prefix> <n>`, n the 1-based index of the entry (a recorded `Draft: ` prefix is kept);
- `fmt:<template>`  the template, filled from `{n}` (the entry's index), `{i}` (the index within the
                    innermost array), `{host}` (the shape's placeholder host) and any key of an enclosing
                    object, innermost first (`{iid}`);
- `url:<template>`  the same, for a URL: the template names `{host}`;
- `sha`             forty times one hexadecimal digit, chosen by the entry's index;
- `key`             a Jira key, as `ABC-<n>`: the same recorded key always becomes the same placeholder, and
                    a recorded key left anywhere in a kept string is replaced by its placeholder too;
- `redact`          `x` (free text no mapping reads);
- `user`            on an object: a person, rewritten by the `USER` table (`user`, a placeholder URL, no
                    e-mail address).

A non-empty string at a path in no table becomes `x` as well, and the path is printed on stderr, so that a
field a newer `glab` or `acli` adds is noticed. Numbers, booleans and nulls are kept wherever they are; an
empty string stays empty. The output is laid out as the tool lays it out (compact on one line for `glab-*`,
indented by two for `acli-*`, `<`, `>` and `&` escaped as Go escapes them, one trailing newline) and is
deterministic: the same input always yields the same bytes.

Usage: scrub-fixture.py --shape glab-mrs|glab-issues|glab-repo|acli-issues <recorded.json> <fixture.json>
    e.g. scrub-fixture.py --shape glab-mrs mrs-recorded.json core/testdata/gitlab/mrs-open.json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

GITLAB_HOST = "gitlab.example.com"
JIRA_HOST = "example.atlassian.net"

# A person, wherever a `user` rule finds one: GitLab's user object and Jira's.
USER = {
    "username": "fmt:user",
    "name": "fmt:user",
    "displayName": "fmt:user",
    "accountId": "fmt:user",
    "web_url": "url:https://{host}/user",
    "avatar_url": "url:https://{host}/uploads/-/system/user/avatar/{id}/avatar.png",
    "avatarUrls.*": "url:https://{host}/avatar/user.png",
    "self": "url:https://{host}/rest/api/3/user?accountId=user",
    "email": "fmt:",
    "public_email": "fmt:",
    "emailAddress": "fmt:",
    "timeZone": "fmt:UTC",
    "state": "keep",
    "accountType": "keep",
    "created_at": "keep",
}

# GitLab's milestone object, as merge requests and issues carry it.
MILESTONE = {
    "title": "text:Milestone",
    "description": "redact",
    "state": "keep",
    "due_date": "keep",
    "start_date": "keep",
    "created_at": "keep",
    "updated_at": "keep",
    "web_url": "url:https://{host}/group/project/-/milestones/{iid}",
}

# What GitLab's merge requests and issues share.
GITLAB_ITEM = {
    "[].description": "text:Description",
    "[].state": "keep",
    "[].created_at": "keep",
    "[].updated_at": "keep",
    "[].closed_at": "keep",
    "[].merged_at": "keep",
    "[].author": "user",
    "[].assignee": "user",
    "[].assignees[]": "user",
    "[].closed_by": "user",
    "[].labels[]": "fmt:label-{i}",
    "[].label_details[].name": "fmt:label-{i}",
    "[].label_details[].description": "redact",
    "[].label_details[].color": "keep",
    "[].label_details[].text_color": "keep",
    "[].time_stats.human_time_estimate": "keep",
    "[].time_stats.human_total_time_spent": "keep",
    "[].imported_from": "keep",
    **{f"[].milestone.{key}": rule for key, rule in MILESTONE.items()},
}

SHAPES = {
    "glab-mrs": {
        **GITLAB_ITEM,
        "[].title": "text:Merge request",
        "[].target_branch": "keep",
        "[].source_branch": "fmt:feature/branch-{n}",
        "[].detailed_merge_status": "keep",
        "[].merge_status": "keep",
        "[].merge_user": "user",
        "[].merged_by": "user",
        "[].reviewers[]": "user",
        "[].prepared_at": "keep",
        "[].merge_after": "keep",
        "[].sha": "sha",
        "[].merge_commit_sha": "sha",
        "[].squash_commit_sha": "sha",
        "[].web_url": "url:https://{host}/group/project/-/merge_requests/{iid}",
        "[].reference": "fmt:!{iid}",
        "[].references.short": "fmt:!{iid}",
        "[].references.relative": "fmt:!{iid}",
        "[].references.full": "fmt:group/project!{iid}",
    },
    "glab-issues": {
        **GITLAB_ITEM,
        "[].title": "text:Issue",
        "[].type": "keep",
        "[].issue_type": "keep",
        "[].severity": "keep",
        "[].due_date": "keep",
        "[].task_status": "keep",
        "[].service_desk_reply_to": "redact",
        "[].web_url": "url:https://{host}/group/project/-/issues/{iid}",
        "[].references.short": "fmt:#{iid}",
        "[].references.relative": "fmt:#{iid}",
        "[].references.full": "fmt:group/project#{iid}",
        "[]._links.self": "url:https://{host}/api/v4/projects/{project_id}/issues/{iid}",
        "[]._links.notes": "url:https://{host}/api/v4/projects/{project_id}/issues/{iid}/notes",
        "[]._links.award_emoji": "url:https://{host}/api/v4/projects/{project_id}/issues/{iid}/award_emoji",
        "[]._links.project": "url:https://{host}/api/v4/projects/{project_id}",
        "[]._links.closed_as_duplicate_of": "url:https://{host}/api/v4/projects/{project_id}/issues/1",
    },
    "glab-repo": {
        "description": "text:Project description",
        "default_branch": "keep",
        "visibility": "keep",
        "ssh_url_to_repo": "url:ssh://git.{host}/group/project.git",
        "http_url_to_repo": "url:https://{host}/group/project.git",
        "web_url": "url:https://{host}/group/project",
        "readme_url": "url:https://{host}/group/project/-/blob/main/README.md",
        "avatar_url": "url:https://{host}/group/project/avatar.png",
        "license_url": "url:https://{host}/group/project/-/blob/main/LICENSE",
        "name": "fmt:project",
        "path": "fmt:project",
        "name_with_namespace": "fmt:group / project",
        "path_with_namespace": "fmt:group/project",
        "namespace.name": "fmt:group",
        "namespace.path": "fmt:group",
        "namespace.full_path": "fmt:group",
        "namespace.kind": "keep",
        "namespace.avatar_url": "url:https://{host}/groups/group/avatar.png",
        "namespace.web_url": "url:https://{host}/groups/group",
        "owner": "user",
        "container_registry_image_prefix": "fmt:registry.{host}/group/project",
        "container_expiration_policy.cadence": "keep",
        "container_expiration_policy.older_than": "keep",
        "container_expiration_policy.next_run_at": "keep",
        "container_expiration_policy.name_regex": "keep",
        "container_expiration_policy.name_regex_delete": "keep",
        "container_expiration_policy.name_regex_keep": "keep",
        "created_at": "keep",
        "updated_at": "keep",
        "last_activity_at": "keep",
        "marked_for_deletion_on": "keep",
        "marked_for_deletion_at": "keep",
        "permissions.group_access.notification_level": "keep",
        "permissions.project_access.notification_level": "keep",
        "_links.self": "url:https://{host}/api/v4/projects/{id}",
        "_links.issues": "url:https://{host}/api/v4/projects/{id}/issues",
        "_links.merge_requests": "url:https://{host}/api/v4/projects/{id}/merge_requests",
        "_links.repo_branches": "url:https://{host}/api/v4/projects/{id}/repository/branches",
        "_links.labels": "url:https://{host}/api/v4/projects/{id}/labels",
        "_links.events": "url:https://{host}/api/v4/projects/{id}/events",
        "_links.members": "url:https://{host}/api/v4/projects/{id}/members",
        "_links.cluster_agents": "url:https://{host}/api/v4/projects/{id}/cluster_agents",
        "import_status": "keep",
        "import_type": "keep",
        "merge_method": "keep",
        "squash_option": "keep",
        "repository_storage": "keep",
        "resource_group_default_process_mode": "keep",
        "auto_cancel_pending_pipelines": "keep",
        "auto_devops_deploy_strategy": "keep",
        "build_git_strategy": "keep",
        "ci_restrict_pipeline_cancellation_role": "keep",
        "ci_pipeline_variables_minimum_override_role": "keep",
        "ci_id_token_sub_claim_components[]": "keep",
        "reviewer_assignment_strategy": "keep",
        "*_access_level": "keep",
        "topics[]": "fmt:topic-{i}",
        "tag_list[]": "fmt:topic-{i}",
        "service_desk_address": "redact",
        "runners_token": "redact",
        "import_url": "redact",
        "import_error": "redact",
        "ci_config_path": "redact",
        "build_coverage_regex": "redact",
        "issues_template": "redact",
        "merge_requests_template": "redact",
        "issue_branch_template": "redact",
        "suggestion_commit_message": "redact",
        "merge_commit_template": "redact",
        "squash_commit_template": "redact",
        "mr_default_title_template": "redact",
        "merge_request_title_regex": "redact",
        "merge_request_title_regex_description": "redact",
        "external_authorization_classification_label": "redact",
    },
    "acli-issues": {
        "[].expand": "keep",
        "[].id": "keep",
        "[].key": "key",
        "[].self": "url:https://{host}/rest/api/3/issue/{id}",
        "[].fields.summary": "text:Work item",
        "[].fields.assignee": "user",
        "[].fields.reporter": "user",
        "[].fields.creator": "user",
        "[].fields.created": "keep",
        "[].fields.updated": "keep",
        "[].fields.labels[]": "fmt:label-{i}",
        "[].fields.issuetype.id": "keep",
        "[].fields.issuetype.name": "keep",
        "[].fields.issuetype.description": "redact",
        "[].fields.issuetype.iconUrl": "url:https://{host}/images/icons/issuetypes/{id}.svg",
        "[].fields.issuetype.self": "url:https://{host}/rest/api/3/issuetype/{id}",
        "[].fields.priority.id": "keep",
        "[].fields.priority.name": "keep",
        "[].fields.priority.iconUrl": "url:https://{host}/images/icons/priorities/{id}.svg",
        "[].fields.priority.self": "url:https://{host}/rest/api/3/priority/{id}",
        "[].fields.status.id": "keep",
        "[].fields.status.name": "keep",
        "[].fields.status.description": "redact",
        "[].fields.status.iconUrl": "url:https://{host}/images/icons/statuses/{id}.png",
        "[].fields.status.self": "url:https://{host}/rest/api/3/status/{id}",
        "[].fields.status.statusCategory.key": "keep",
        "[].fields.status.statusCategory.name": "keep",
        "[].fields.status.statusCategory.colorName": "keep",
        "[].fields.status.statusCategory.self": "url:https://{host}/rest/api/3/statuscategory/{id}",
        "[].fields.project.key": "fmt:ABC",
        "[].fields.project.name": "fmt:Project",
    },
}

HOSTS = {"glab-mrs": GITLAB_HOST, "glab-issues": GITLAB_HOST, "glab-repo": GITLAB_HOST, "acli-issues": JIRA_HOST}

# A Jira key as the recording may spell it: an upper-case project key, a dash, a number.
JIRA_KEY = re.compile(r"[A-Z][A-Z0-9_]*-[0-9]+")

# The characters Go's encoding/json escapes inside a string, as `glab` and `acli` print them.
GO_ESCAPES = {"<": "\\u003c", ">": "\\u003e", "&": "\\u0026", "\u2028": "\\u2028", "\u2029": "\\u2029"}


class Context(dict):
    """What a template is filled from: the fixed names first, then the enclosing objects, innermost first."""

    def __init__(self, fixed: dict, objects: list[dict]):
        super().__init__(fixed)
        self.objects = objects

    def __missing__(self, name: str):
        for enclosing in reversed(self.objects):
            if name in enclosing:
                return enclosing[name]
        raise KeyError(f"a template names {{{name}}}, which no enclosing object holds")


class Scrubber:
    """Scrubs one reply of one shape, remembering the Jira keys it replaced and the paths no table names."""

    def __init__(self, shape: str):
        self.table = SHAPES[shape]
        self.host = HOSTS[shape]
        self.keys: dict[str, str] = {}
        self.unknown: set[str] = set()

    def rule_for(self, table: dict, path: str) -> str | None:
        """The rule @p table holds for @p path, matched exactly or with `*` standing for any part of a key."""
        if path in table:
            return table[path]
        parts = path.split(".")
        for pattern, rule in table.items():
            wanted = pattern.split(".")
            if len(wanted) == len(parts) and all(
                re.fullmatch(re.escape(w).replace(r"\*", ".*"), p) for w, p in zip(wanted, parts)
            ):
                return rule
        return None

    def scrub(self, value, table: dict, path: str, shown: str, objects: list[dict], n: int, i: int):
        """@p value scrubbed by @p table at @p path; @p shown is the full path for the stderr report."""
        rule = self.rule_for(table, path) if path else None
        if rule == "user" and isinstance(value, dict):
            return self.scrub(value, USER, "", shown, objects, n, i)
        if isinstance(value, dict):
            inner = objects + [value]
            return {
                key: self.scrub(item, table, f"{path}.{key}" if path else key, f"{shown}.{key}", inner, n, i)
                for key, item in value.items()
            }
        if isinstance(value, list):
            at = f"{path}[]"
            return [
                self.scrub(item, table, at, f"{shown}[]", objects, index if at == "[]" else n, index)
                for index, item in enumerate(value, 1)
            ]
        if not isinstance(value, str) or value == "":
            return value
        if rule is None:
            self.unknown.add(shown)
            return "x"
        return self.apply(rule, value, Context({"n": n, "i": i, "host": self.host}, objects))

    def apply(self, rule: str, value: str, context: Context) -> str:
        kind, _, argument = rule.partition(":")
        n = context["n"]
        if kind == "keep":
            return value
        if kind == "text":
            draft = "Draft: " if value.startswith("Draft: ") else ""
            return f"{draft}{argument} {n}"
        if kind in ("fmt", "url"):
            return argument.format_map(context)
        if kind == "sha":
            return "0123456789abcdef"[n % 16] * 40
        if kind == "key":
            return self.keys.setdefault(value, f"ABC-{len(self.keys) + 1}")
        if kind == "redact":
            return "x"
        raise ValueError(f"unknown rule {rule!r}")

    def replace_keys(self, value):
        """@p value with every recorded Jira key left in a kept string replaced by its placeholder."""
        if isinstance(value, dict):
            return {key: self.replace_keys(item) for key, item in value.items()}
        if isinstance(value, list):
            return [self.replace_keys(item) for item in value]
        if isinstance(value, str) and self.keys:
            return JIRA_KEY.sub(lambda match: self.keys.get(match.group(0), match.group(0)), value)
        return value

    def run(self, recorded):
        """@p recorded, scrubbed."""
        return self.replace_keys(self.scrub(recorded, self.table, "", "$", [], 1, 1))


def layout(value, shape: str) -> str:
    """@p value written the way the tool behind @p shape writes it."""
    if shape.startswith("acli-"):
        text = json.dumps(value, indent=2, ensure_ascii=False)
    else:
        text = json.dumps(value, separators=(",", ":"), ensure_ascii=False)
    for character, escaped in GO_ESCAPES.items():
        text = text.replace(character, escaped)
    return text + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--shape", choices=sorted(SHAPES), required=True, help="the command whose reply it is")
    parser.add_argument("recorded", type=Path, help="the recorded reply")
    parser.add_argument("output", type=Path, help="where to write the scrubbed fixture")
    arguments = parser.parse_args()

    recorded = json.loads(arguments.recorded.read_text(encoding="utf-8"))
    scrubber = Scrubber(arguments.shape)
    text = layout(scrubber.run(recorded), arguments.shape)
    arguments.output.write_text(text, encoding="utf-8", newline="\n")
    for path in sorted(scrubber.unknown):
        print(f"{arguments.recorded}: {path}: in no table, written as \"x\"", file=sys.stderr)
    print(f"{arguments.output}: {len(text.encode('utf-8'))} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
