# Orchestrator: GitLab (`glab`) and Jira (`acli`) Recorded — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the orchestrator accepts `forge: gitlab` repositories and `source: jira` issue sources, because their
mappings are tested against output that `glab` 1.119.0 and `acli` 1.3.41 actually print.

**Architecture:** the provider row (`core/Provider.cpp`), the issue-source row (`core/IssueSources.cpp`) and
their command rows (`core/Commands.cpp`) flip from `Verification::Unmeasured` to `Recorded`. That alone puts the
commands in the manifest and lets the configuration accept them. The fixtures in `core/testdata/{gitlab,jira}/`
are replaced by **scrubbed recordings**: real output with every value that is free text, an identity or a URL
replaced by a placeholder, keeping keys, nesting, types, and enumerated values (`state`,
`detailed_merge_status`, booleans, ids) exactly as printed.

**Tech Stack:** C++23, glaze, Catch2; Python 3 for the scrub script (as `harness/scale-fixture.py`).

**Spec:** `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md` (phase 1 and its
"Findings from the recording").

## Global Constraints

- Follow `AGENT.md` and `.clang-tidy`; `-Werror` builds; no `NOLINT` beyond the existing glaze `value` pattern.
- Never commit real output: titles, descriptions, names, usernames, e-mail, URLs, hosts, project paths, SHAs
  and Jira keys are placeholders (`user`, `gitlab.example.com`, `example.atlassian.net`, `ABC-…`).
  `ctest -R fixture-privacy` must pass.
- The recordings in the session scratchpad are inputs to the scrub script only; nothing outside
  `core/testdata/` and `harness/` is written from them.
- One commit per task; manifest golden (`contract/manifest-v2/valid/orchestrator.manifest`) and consent text
  (`orchestrator.consent.txt`) change in the same commit as the rows that change them.
- Builds/tests on Windows: `cmake --build --preset clangcl-debug --target orchestrator_test` then run
  `orchestrator_test`; the full `ctest --preset clangcl-debug -L daemon` before the last commit. (Linux:
  `clang-asan`.) If the Windows tree has no `orchestrator_test` target, reconfigure with
  `-DORCHESTRATOR_TESTING=ON`.

## Review Focus

1. **An empty `glab issue list`** must map to zero issues, not a parse error — pinned by the `issues-empty.json`
   fixture (`[]`, as `--output json` prints it) in Task 2.
2. **`glab` and `acli` stderr/exit 1** (bad JQL, auth expired) must surface as the source's problem line, not as
   an empty list — pinned by a Jira_test case on `acli-bad-jql.stderr` with empty stdout in Task 3.
3. **A recorded MR whose `detailed_merge_status` is unknown to the table** (`conflict`, `draft_status`,
   `discussions_not_resolved`) maps to `ReviewState::None`, never throws — pinned in Task 2's table test.
4. **Pretty-printed (multi-line) `acli` output cut at the host's limit** reads up to the last whole entry —
   pinned in Task 3 by cutting the recorded fixture mid-entry.
5. **Environment redirection:** `GITLAB_HOST`/`GL_HOST`-style variables in the daemon's environment must not
   redirect `glab` to another host, as `GH_HOST` cannot redirect `gh` — Task 4.

---

### Task 1: The scrub script

**Files:**
- Create: `extensions/orchestrator/harness/scrub-fixture.py`

**Interfaces:**
- Produces: `scrub-fixture.py --shape {glab-mrs,glab-issues,glab-repo,acli-issues} IN OUT` — reads JSON, writes
  JSON in the input's layout (compact one-line for `glab-*`, `indent=2` for `acli-*`), deterministic (same input
  → same bytes).

- [ ] **Step 1:** Write the script. Per shape, a table of JSON paths → rule. Rules: `keep` (enums, booleans,
  numbers, null), `text:<prefix>` (→ `"<prefix> <n>"`, n = 1-based entry index), `user` (every object with
  `username`/`name`/`avatar_url`/`web_url`/`email` → `user`, `https://gitlab.example.com/user`, no email),
  `url` (host → `gitlab.example.com` / `example.atlassian.net`, path → `group/project/-/merge_requests/<iid>`),
  `sha` (→ 40 × `0`…`f` derived from index), `key` (Jira key → `ABC-<n>`). Any string at a path not in the table
  becomes `"x"`; the script prints the list of such paths to stderr so a new glab field is noticed.
- [ ] **Step 2:** Run it on the scratchpad recordings (`rec/mrs-real.json`, `rec/repo.json`, `rec/acli.json`)
  into a temporary directory; check with a grep for the recording's own host, group and site names, and for `@`, that nothing real survives (expect no
  output).
- [ ] **Step 3: Commit** — `harness: scrub a recorded glab or acli reply into a fixture`.

### Task 2: GitLab recorded

**Files:**
- Modify: `extensions/orchestrator/core/Commands.cpp:252-269,402-432` (argv, verification)
- Modify: `extensions/orchestrator/core/Provider.cpp:33-48` (verification, comment)
- Modify: `extensions/orchestrator/core/GitLab.cpp` (CI from `detailed_merge_status`), `core/GitLab.hpp` (doc)
- Replace: `core/testdata/gitlab/{mrs-open.json,mrs-100.json,issues-open.json,default-branch.json}`, `VERSION`,
  `README.md`; Create: `core/testdata/gitlab/issues-empty.json` (`[]`)
- Test: `core/GitLab_test.cpp`, `core/Provider_test.cpp:195-231`, `core/Config_test.cpp:347`
- Golden: `contract/manifest-v2/valid/orchestrator.manifest`, `orchestrator.consent.txt`

**Interfaces:**
- Produces: `gitlab::ciStateOf(std::string_view detailedMergeStatus) noexcept -> CiState` beside
  `reviewStateOf`, a `Spelling` table: `ci_still_running` → `Pending`, `ci_must_pass` → `Failing`, everything
  else → `None`.

- [ ] **Step 1: Write failing tests.**
  - `"GitLab: the recorded merge requests map to the model"`: from the scrubbed `mrs-open.json`, assert the count
    (15 minus any skipped), that every `number` is non-zero, `.ci == CiState::Pending` for the
    `ci_still_running` entries and `None` for `draft_status`; `.readiness == Readiness::Draft` exactly for
    `draft == true`.
  - `"CI states by table"`: `ciStateOf("ci_still_running") == Pending`, `ciStateOf("ci_must_pass") == Failing`,
    `ciStateOf("conflict") == None`, `ciStateOf("") == None`.
  - `"review states by table"` gains `conflict`, `draft_status`, `discussions_not_resolved` → `None`.
  - `"GitLab: an empty issue list"`: `issues(issues-empty.json, Whole)` has 0 items, 0 skipped.
  - `Provider_test`: `findProvider(Forge::GitLab)->verification == Verification::Recorded`; a config with
    `"forge": "gitlab"` parses.
  - `Commands_test`: `glab-mrs`, `glab-issues`, `glab-default-branch` argv contain `--output`, `json` and no
    `-F`.
- [ ] **Step 2:** Run `orchestrator_test "[gitlab],[provider],[commands]"`; expect the new cases to fail.
- [ ] **Step 3:** Implement. argv: replace `"-F"sv, "json"sv` by `"--output"sv, "json"sv` in the three rows;
  `Verification::Recorded` on the five glab rows and the provider row; rewrite the "written from the
  documentation" comments to say what was recorded. `ciStateOf` as above, used in `changeRequestOf`.
  Regenerate fixtures with Task 1's script, `mrs-100.json` with `harness/scale-fixture.py --shape glab-mrs`.
  `VERSION`: `glab 1.119.0 (f5016eda2), recorded 2026-11-23, scrubbed by harness/scrub-fixture.py`. README:
  "Recorded, then scrubbed", the `--output` finding, and what the list lacks (pipeline, closing issues).
- [ ] **Step 4:** The `Unrecorded` configuration problem loses its last real row. Keep it tested: the Config
  and Provider rejection tests use an injected table holding one Unmeasured row (the `findProvider(span, …)` /
  `findIssueSource(span, …)` overloads exist; if `Config.cpp` reads the global tables directly, give the parse
  entry point the tables as a parameter defaulted at its one production call site — not a global).
- [ ] **Step 5:** Regenerate the manifest golden through the test (`orchestrator_manifest_matches_corpus`'s
  `ACTUAL` file copied over `EXPECTED`) and the consent text through `vtextension_test "*consent*"` as its
  failure message says. Check by eye that the consent text gains exactly the five glab commands.
- [ ] **Step 6:** Run `orchestrator_test`, `ctest -R "orchestrator|consent|fixture-privacy"`; all pass.
- [ ] **Step 7: Commit** — `feat(orchestrator): GitLab, recorded from glab 1.119`.

### Task 3: Jira recorded

**Files:**
- Modify: `core/Commands.cpp:433-445`, `core/IssueSources.cpp:26-36`, `core/Jira.hpp` (doc comment)
- Replace: `core/testdata/jira/issues-open.json` (scrubbed, pretty-printed), `VERSION`, `README.md`;
  keep `issues-bad-key.json` (hand-made, say so in the README); Create: `issues-empty.json` (`[]`),
  `acli-bad-jql.stderr` (`✗ Error: failed to parse JQL query: the value 'nope' does not exist for the field
  'project'.`)
- Test: `core/Jira_test.cpp`, `core/Config_test.cpp:351,594-603`, `core/Engine_test.cpp` (one case)
- Golden: manifest and consent text, as Task 2.

- [ ] **Step 1: Write failing tests.**
  - `"Jira: the recorded work items map to the model"`: count matches the fixture; every item is a `JiraRef`;
    url is `https://example.atlassian.net/browse/<key>`.
  - `"Jira: an empty search"`: `issues-empty.json` → 0 items.
  - `"Jira: a cut reply keeps the whole entries before the cut"` gains a case on the recorded (multi-line)
    fixture cut in the middle of its second entry → exactly 1 item, completeness Cut.
  - Engine_test `"a failing acli shows its problem line"`: the fake process answers `acli-issues` with exit 1,
    empty stdout and `acli-bad-jql.stderr`; the dashboard's problems hold one line naming Jira, and the issue
    list of that source is not replaced by an empty one.
  - Config: a `jira` source with `jql` and `site` parses; `"is not a supported issue source: forge, jira"`
    stays.
- [ ] **Step 2:** Run `orchestrator_test "[jira],[config],[engine]"`; expect failures.
- [ ] **Step 3:** Implement: `Verification::Recorded` on `acli-auth`, `acli-issues` and the `jira` source
  row; comments say recorded. Fixture via Task 1's script; `VERSION`: `acli version 1.3.41-stable, recorded
  2026-11-23, scrubbed by harness/scrub-fixture.py`. README: confirmed bare array, `[]` when empty, exit 1 +
  stderr on bad JQL, `auth status` exits 0 when logged in; the `--limit 100` cap is assumed, not measured.
- [ ] **Step 4:** Regenerate goldens as Task 2 Step 5; consent text gains exactly the two acli commands.
- [ ] **Step 5:** Run `orchestrator_test`, `ctest -R "orchestrator|consent|fixture-privacy"`; all pass.
- [ ] **Step 6: Commit** — `feat(orchestrator): Jira through acli, recorded from acli 1.3.41`.

### Task 4: Withhold glab's host selection, and the docs

**Files:**
- Modify: `src/vtextension/ChildEnvironment.hpp` (the withheld-name table), `src/vtextension/ChildEnvironment_test.cpp`
- Modify: `docs/extensions.md` (Environment bullet; Limitations: drop "GitHub only"; the GitLab/Jira
  configuration paragraphs), `extensions/orchestrator/README.md` if it states GitHub only

- [ ] **Step 1:** Find glab's host and repository selection variables in glab 1.119's documentation
  (`glab help` topics or https://gitlab.com/gitlab-org/cli `docs/`): at least `GITLAB_HOST`, `GL_HOST`,
  `GITLAB_URI`, `GITLAB_API_HOST`, `GITLAB_REPO`/`GL_REPO` where they exist — list exactly those glab reads.
  Do not withhold `GITLAB_TOKEN` (it authenticates, as `GH_TOKEN` is not withheld).
- [ ] **Step 2: Failing test** in `ChildEnvironment_test.cpp`, same shape as the `GH_HOST` case: each listed
  variable in the daemon's environment is absent from a command's and a pane's environment.
- [ ] **Step 3:** Add the names to the table; run `vtextension_test "[environment]"` (or the file's tag); pass.
- [ ] **Step 4:** Docs as listed; the README's GitLab note says CI comes from `detailed_merge_status`
  (`ci_still_running` → pending, `ci_must_pass` → failing) since the list carries no pipeline.
- [ ] **Step 5:** `ctest --preset clangcl-debug -L daemon` (Linux: `clang-asan`); all pass. `/simplify`.
- [ ] **Step 6: Commit** — `feat(vtextension): withhold glab's host selection from extension commands`.
