# Orchestrator default permissions: implementation plan (Fleet + Focus, phase 0)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Every agent the orchestrator starts runs with a permission profile, passed through Claude Code's
`--settings`, so routine work (edits, builds, tests, local git) does not wait for approval.

**Architecture:**
- **Core:** a new module, `core/Permissions`, holds the built-in profiles as a data table. It resolves `extends`
  chains and writes a profile as the settings JSON.
- **Config:** reads `permissions` (the default profile's name), `permission_profiles` (added or replaced profiles)
  and a task's `permissions`.
- **Dispatch:** stamps the profile's name and its JSON on the run.
- **Panes:** both agent templates gain `--settings {settings}`.
- **Details:** show the profile.

**Tech stack:** C++23, glaze (JSON), Catch2, core-cpp TUI (golden screens).

Spec: `docs/superpowers/specs/2026-11-22-orchestrator-fleet-focus-design.md`, "Default permissions".

## Global constraints

- **Config shape.** The schema walk in `Config.cpp` knows strings and lists of objects, not maps, so profiles are
  a list:

  ```yaml
  permissions: standard                # the default profile; omitted: standard
  permission_profiles:                 # added, or replacing a built-in profile of the same name
    - { name: careful, extends: standard, mode: default, allow: [], deny: ["Bash(git commit:*)"] }
  tasks:
    - { id: fix-ci, …, permissions: trusted }
  ```

- **Built-in profiles** (`BuiltinProfiles`, a table):
  - `standard`: mode `acceptEdits`; the allow and deny lists in the spec.
  - `trusted`: extends `standard`, and allows `Bash(git push:*)`, `Bash(gh pr create:*)` and
    `Bash(gh pr edit:*)`.
  - `ask`: mode `default`, no rules.
- **Modes:** `default`, `acceptEdits` and `plan`. `bypassPermissions` is refused with its own problem, as is any
  other word.
- **Rules:** `Tool` or `Tool(specifier)`. `Tool` is `[A-Za-z][A-Za-z0-9_-]*` (MCP tools such as
  `mcp__srv__tool` fit); the specifier is non-empty, has no `(` or `)`, and has no control character. At most 64
  rules per list.
- **Resolution:**
  - mode: the profile's own, else its parent's;
  - allow and deny: the parent's, then the profile's own, without duplicates;
  - an unknown parent is refused (`UnknownProfile`), and so is a cycle (`ProfileCycle`).
- **Settings JSON:** `{"permissions":{"defaultMode":"<mode>","allow":[…],"deny":[…]}}`, written by glaze. A
  profile whose JSON exceeds `MaxTextArgumentBytes` (8 KiB) is refused at parse time (`SettingsTooLarge`).
- **Persistence:** `Run::permissions` (the profile name) and `Run::settings` (the JSON) are persisted. A run
  stored before they existed reads both as empty, and its pane opens with `--settings {}`.
- **Code rules:** zero warnings, clang-tidy clean, `enum class` and `std::expected`, no C-style loops. Goldens
  are rewritten only deliberately. Build with `-j4`.

## File map

| File | Change |
| --- | --- |
| `core/Permissions.{hpp,cpp}`, `core/PermissionsJson.hpp` | **new:** `PermissionMode`, `PermissionProfile`, `BuiltinProfiles`, `resolveProfiles()`, `settingsJsonOf()`, `isPermissionRule()` |
| `core/Permissions_test.cpp` | **new:** table tests |
| `core/ConfigJson.hpp`, `core/Config.{hpp,cpp}` | the three keys; `ConfigProblem`s `UnknownProfile`, `ProfileCycle`, `NotPermissionMode`, `BypassRefused`, `NotPermissionRule`, `SettingsTooLarge`; `Config::profileFor(taskId)`, `Config::findProfile(name)` |
| `core/Config_test.cpp` | parsing and refusals |
| `core/Engine.hpp`, `core/EngineDispatch.cpp` | `Dispatch::permissions` (empty: the task's or the default); the run gets `permissions` and `settings` |
| `core/Run.{hpp,cpp}`, `core/RunJson.hpp` | `Run::permissions`, `Run::settings`; `agentPaneArguments()` adds `settings` |
| `core/Commands.cpp` | `arg settings = text`; `--settings {settings}` in both pane argvs |
| `contract/manifest-v2/valid/orchestrator.{manifest,consent.txt}` | regenerated, plus the consent sentence |
| `view/Sections.cpp`, goldens | the `perms` detail line |
| `src/vthost/AgentRun_test.cpp` | the new argv |
| `README.md`, `docs/extensions.md` | the keys, the profiles, and the consent line |

## Task 1: Profiles and their configuration

**Interfaces produced:**

```cpp
enum class PermissionMode : uint8_t { Default = 0, AcceptEdits, Plan };
struct PermissionProfile {
    std::string name;
    std::optional<std::string> extends;          // as configured; resolved profiles have none
    std::optional<PermissionMode> mode;          // resolved: always set
    std::vector<std::string> allow, deny;
};
inline constexpr std::string_view DefaultProfileName = "standard";
[[nodiscard]] std::span<PermissionProfile const> builtinProfiles();
[[nodiscard]] bool isPermissionRule(std::string_view rule) noexcept;
enum class ProfileError : uint8_t { UnknownProfile, ProfileCycle };
struct ProfileProblem { std::string profile; ProfileError error; };
[[nodiscard]] std::expected<std::vector<PermissionProfile>, ProfileProblem>
    resolveProfiles(std::span<PermissionProfile const> configured);   // built-ins, replaced by name, then added
[[nodiscard]] std::string settingsJsonOf(PermissionProfile const& resolved);
// Config
[[nodiscard]] PermissionProfile const* findProfile(std::string_view name) const noexcept;
[[nodiscard]] PermissionProfile const& profileFor(std::string_view taskId) const noexcept;
```

- [ ] **Tests first:**
  - `Permissions_test`:
    - `isPermissionRule` table: `Read` ✓, `Bash(git push:*)` ✓, `mcp__a__b` ✓, `Bash()` ✗, `Bash(a(b))` ✗,
      `1Bash` ✗, `Bash(x` ✗, a rule with a control character ✗;
    - `resolveProfiles({})`: three profiles, with `trusted` holding `standard`'s rules plus its own three, mode
      `acceptEdits`;
    - replacing `standard` changes `trusted`;
    - a cycle (`a → b → a`) → `ProfileCycle`, naming `a`;
    - an unknown parent → `UnknownProfile`;
    - `settingsJsonOf(ask)` == `{"permissions":{"defaultMode":"default","allow":[],"deny":[]}}`;
    - `settingsJsonOf(standard)` round-trips through `glz::read` with the same lists.
  - `Config_test`:
    - absent keys → the default `standard`, and `profileFor("fix-ci")` is `standard`;
    - `permissions: ask` → the default `ask`;
    - a task's `permissions: trusted` → `profileFor` gives `trusted`;
    - unknown names (`permissions: nope`, a task's `nope`) → `UnknownProfile` at the key;
    - `mode: bypassPermissions` → `BypassRefused`, and `mode: yolo` → `NotPermissionMode`;
    - `allow: ["Bash("]` → `NotPermissionRule` at `permission_profiles[0].allow[0]`;
    - a duplicate profile name in the list → `Duplicate`;
    - 64 long rules → `SettingsTooLarge`.
- [ ] **Run them:** `cmake --build out/clang-release -j4 --target orchestrator_test &&
  out/clang-release/extensions/orchestrator/orchestrator_test "[permissions],[config]"`. Expected: they fail to
  compile.
- [ ] **Implement:**
  - `Permissions.{hpp,cpp}`, and `PermissionsJson.hpp` for the glaze mirror of the settings JSON;
  - the three config keys (`JsonPermissionProfile`, its `Field` table, `ObjectKind::PermissionProfile`, the
    `matchesMirror` assert);
  - the problems and their sentences in `ProblemTexts`;
  - `permissionsOf()`, called after `tasksOf()`, since the tasks' names are checked against the resolved
    profiles.
- [ ] **Run them again.** Expected: PASS, with the whole `orchestrator_test` green.
- [ ] **Commit:** `feat(orchestrator): permission profiles for the agents it starts`.

## Task 2: Every run carries its profile into `--settings`

- [ ] **Tests first:**
  - `Run_test`:
    - `agentPaneArguments` includes `settings`;
    - a run with empty `settings` gives `{}`;
    - `toJson`/`fromJson` round-trips `permissions` and `settings`;
    - a stored run without them reads as empty.
  - `Engine_test`:
    - a dispatch under the default config stamps `permissions == "standard"` and `settings ==
      settingsJsonOf(standard)`;
    - a task with `permissions: trusted` stamps `trusted`;
    - `Dispatch::permissions = "ask"` overrides both;
    - an unknown override is refused, with a new `DispatchRefusal::UnknownProfile`;
    - the `OpenAgentPane` effect's arguments carry `settings`.
  - `Commands_test` / the manifest contract test:
    - the pane lines read `claude --model {model} --session-id {session} --settings {settings}
      --append-system-prompt {board} {prompt}` and `claude --model {model} --session-id {session} --settings
      {settings} {prompt}`.
- [ ] **Implement:**
  - `Run` fields and `RunJson`;
  - `agentPaneArguments`;
  - `EngineDispatch` (the profile from `Dispatch::permissions`, else `Config::profileFor(task)`);
  - the `ArgumentTable` row and the two argvs.
- [ ] **Regenerate:**
  - `out/clang-release/extensions/orchestrator/orchestrator_manifest >
    extensions/orchestrator/contract/manifest-v2/valid/orchestrator.manifest`;
  - update `orchestrator.consent.txt` by hand: the two pane lines, and the sentence "Its agents start with the
    permission profile your configuration names (`permissions`), which may let them edit, build, test and commit
    without asking."
- [ ] **Update** `src/vthost/AgentRun_test.cpp` for the argv.
- [ ] **Run** `orchestrator_test`, `vtextension_test` and `vthost_test`. Expected: PASS.
- [ ] **Commit:** `feat(orchestrator): agents start with their permission profile`.

## Task 3: Show it, and document it

- [ ] **Tests first:**
  - `Sections_test`: a run's details gain `perms     standard · 19 allowed · 4 denied`, or `perms     ask ·
    Claude Code asks` for a profile with no rules and mode `default`. A run stored before profiles shows no
    `perms` line.
  - `Dashboard_test` goldens: the details of `details-board-120x40`, rewritten with
    `ORCHESTRATOR_UPDATE_GOLDENS=1` and checked by eye.
- [ ] **Implement** the detail line in `Sections.cpp` from `Run::permissions` and the counts in `Run::settings`.
  Read the counts with glaze from the stored JSON, not from the config, so a changed config does not rewrite what
  a run was given.
- [ ] **Documentation:**
  - README: a "Permissions" paragraph, the three profiles, and how to opt out (`permissions: ask`);
  - `docs/extensions.md`: the `permissions`, `permission_profiles` and task `permissions` keys, and the consent
    line.
- [ ] **Run the whole `orchestrator_test`, plus `vthost_test`.** Expected: PASS.
- [ ] **Commit:** `feat(orchestrator): a run's details name its permission profile`.

## Verification before the phase is done

- [ ] **Check the settings JSON against Claude Code once, by hand:**

  ```sh
  claude --settings "$(out/clang-release/extensions/orchestrator/orchestrator_test --print-settings standard)" \
      -p "run: git status"
  ```

  It should run without a permission prompt. If `--print-settings` does not exist, take the JSON from a
  `Run_test` failure message. If Claude Code refuses a rule, such as `Skill(…)`, drop it from `standard` and say
  so.
- [ ] **Run `/simplify`** over the phase's commits.
