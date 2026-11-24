# Orchestrator Agent Workers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each orchestrator task names a *worker* — an agent kind (`claude` or `copilot`) plus a variable set the
user defines under `daemon.extensions.<name>.env_sets` — and everything the orchestrator runs for that worker (its
tab, transcript, stats, hooks check, plugin commands) gets that set's values from the host, never from the guest.

**Architecture:** Bottom-up. The host (`src/vtextension`) gains a second, user-facing settable-variable table with
value shapes, two manifest lines (`pane-env-set`, `command-env-set`) that bind a pane or command to "the set the
call names, restricted to these variables", and applies the named set at `pane-open`/`proc-start`; the user's sets
are part of what the consent dialog shows and what the grant digest covers. `contour_config` reads and writes
`env_sets`. The orchestrator declares the bindings and a `copilot` pane, reads `workers` and `tasks[].worker`,
records a run's worker, agent and set, passes the set on every call for it, runs the hooks and plugin checks once
per distinct Claude set, ends a Copilot run `Closed` when its tab closes, and shows the worker on the dashboard.
The WIT does not change, so no guest rebuilds for it.

**Tech Stack:** C++23, Catch2 (`vtextension_test`, `contour_test`, `orchestrator_test`), yaml-cpp, glaze (orchestrator
JSON), the manifest-v2 corpus, git plumbing for the WIP-preserving docs commit, PowerShell/robocopy for the deploy.

**Spec:** `docs/superpowers/specs/2026-11-24-orchestrator-agent-workers-design.md` (approved). Read it before any task.

## Global Constraints

- **The user's uncommitted work.** `docs/extensions.md` and `cmake/ContourWasiCxx.cmake` carry the user's own
  uncommitted hunks (the CPM-fetched wasi-sdk and its documentation in "Building a C++ guest"). For the whole plan:
  - never `git add -A`, `git add .`, `git add -u`, `git commit -a`, or a pathspec commit (`git commit -- <path>`,
    `git commit <path>`: it takes the **working-tree** content); stage named paths only, and run
    `git diff --cached --name-only` before every commit — it must list exactly the task's files;
  - never `git stash`, `git checkout -- <file>`, `git restore`, `git reset --hard`, or a formatter/`sed -i` on
    either file;
  - `cmake/ContourWasiCxx.cmake` is not edited by this plan at all;
  - `docs/extensions.md` is changed only by Task 12's procedure.
- **Pre-existing state.** `out/build/clangcl-release` is configured with the guest on; its wasi-sdk came from the
  user's uncommitted CPM fetch (`_deps/wasi-sdk-src`). Use that tree; do not reconfigure it; do not delete `_deps`.
- **Builds:** one at a time (another agent may share the machine), 10-minute timeout, rerun on timeout:
  `cmake --build --preset clangcl-release --target <target>`; test binaries are
  `out/build/clangcl-release/bin/<target>.exe`.
- **AGENT.md:** C++23; range-for only; `[[nodiscard]]`; Doxygen on new public API; `enum class : uint8_t` over
  `bool` with the zero enumerator the negative/default; data-driven tables (one row per settable variable, one row
  per agent kind); `std::expected` for fallible API; constructor-complete objects; no `NOLINT`; zero warnings under
  `-Werror`; `clang-format` every C++ file you touch; headers `.hpp` with `#pragma once`. Never edit
  `vendor/core-cpp`.
- **The orchestrator stays wasm32-wasi portable.** After every task that touches `extensions/orchestrator/{core,view,
  guest}`: `python scripts/check-guest-portability.py` exits 0 (no throw/try/catch, no RTTI, no run-time format,
  no host-only header, no ambient clock/env/random). `python scripts/check-include-budget.py` exits 0 too.
- **No real user data in committed files.** No user name, home path, host, repository or account. Examples use
  `C:\Users\me\…`, `/home/u/…`, `proxy.example.com`. Only Task 13 writes real paths, and only into the user's
  local config, never into the repository.
- **Every valid configuration today stays valid and behaves the same** (spec, Goals): no `workers` means one
  implicit worker `claude` (agent `claude`, no set), and the orchestrator then passes no `env-set` argument
  anywhere — its `OpenPane`/`RunCommand` arguments are byte-identical to today's.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Subagent models (user's rule):** Haiku for Tasks 1, 2, 6, 7, 11 (fully specified); Opus for Tasks 3, 4, 5, 8,
  9, 10, 12, 13 (cross-layer, harness judgment, the WIP-preserving procedure, the user's machine). Every review on
  Opus. A Haiku task that comes back blocked, or reaches a 4th fix round, is re-dispatched on Opus.

## Decisions (the builder's, per the spec's "Left to the builder")

1. **Where the variable list and value shapes are checked.** `contour_config` links no `vtextension` (which only
   exists with `CONTOUR_EXTENSIONS`), so the config reader checks *structure* (a set name is a Name, a variable
   name is `[A-Za-z_][A-Za-z0-9_]*`, a value is a scalar) and the daemon checks *the list and the shapes* when it
   composes its extensions (`checkEnvironmentSets`, Task 1, called in Task 4). Both log to the daemon's error log
   naming `daemon.extensions.<name>.env_sets.<set>.<VAR>`; the set is kept without the variable. `daemon.extensions`
   is read only by the daemon, so the user sees the same thing the spec asks for, at the same moment.
2. **Value limits.** Paths go through `rootSpellingOf()` (its `MaxPathArgumentBytes`); URLs and `NO_PROXY` at most
   2048 bytes, printable ASCII without blanks. A URL with credentials (`user:pass@`) is refused: no secrets in the
   configuration, and none on the consent dialog.
3. **Call-time failures** are `ArgumentRejected` → `bad-argument`: an unknown set, a set value that fails the
   re-check, an `env-set` argument passed to a pane or command without a binding.
4. **The set argument is taken out** of the call's arguments before the argv is substituted
   (`substituteDeclared` refuses a name the template has no placeholder for).
5. **Grant digest.** `grantDigestOf` appends the sets only when there are any, so every grant given before this
   change stays valid.
6. **Unknown `tasks[].worker`** is a non-fatal *notice* (`Config::notices()`), shown as a problem line; the task
   runs on the first worker. **`permissions` on a Copilot worker is fatal** (`ProfileOnCopilot`): a deny rule the
   user wrote would silently not apply. (Open question for the user; see the controller's list.)
7. **Stored runs.** `RunSchema` stays 3; `worker`, `agent`, `envSet` are optional keys, absent meaning a legacy
   Claude run with no set. `contour agent record`'s format 1 gains the value `closed` for `ended` and no new key.
8. **Copilot runs** still get a session UUID (`isStorable` needs one); it is never passed. No stats or transcript
   is read for them; their board instruction is empty.
9. **Setup-check tags.** `poll/hooks` and `poll/plugins` stay the tags for the no-set worker; a set adds
   `:<set>` (`poll/hooks:claude-work`), a separator no repository name can hold.
10. **Problem lines.** With only the implicit worker, the hooks line is today's `HooksMissingProblem` verbatim;
    with configured workers it names them (`… missing for worker claude-work: …`).

## Review Focus

1. **A grant given before this change** — an extension with no `env_sets` must not be asked again after the
   upgrade: its grant digest is unchanged (Task 5 pins it).
2. **A run stored before workers existed** loads as a Claude run with no set, opens the same pane with the same
   arguments, and is written back without the new keys (Task 8 pins it).
3. **A configuration without `workers`** produces `OpenPane` and every `RunCommand` with exactly today's
   arguments — no `env-set` anywhere (Task 8 pins it).
4. **A set the user removed after a run started**: the run's tab-open fails `bad-argument` and a problem line names
   the set, rather than a bare "bad-argument" (Task 8 pins it).
5. **A value dropped at load** (a relative `CLAUDE_CONFIG_DIR`, a misspelt variable) leaves the agent on the
   daemon's own Claude account; the consent dialog must show the set *without* it, so the user can see it is gone
   (Task 5 pins it).

---

### Task 1: The user-settable variables and their value shapes (host) — Haiku

**Files:**
- Create: `src/vtextension/UserSettableEnvironment.hpp`, `src/vtextension/UserSettableEnvironment.cpp`
- Create: `src/vtextension/UserSettableEnvironment_test.cpp`
- Modify: `src/vtextension/CMakeLists.txt` (sources, headers, `vtextension_test`), `src/vtextension/testing/PublicHeaders.cpp`

**Interfaces:**
- Consumes: `rootSpellingOf(std::string_view raw, std::string_view home, vtpty::PathStyle)` (`RootPointer.hpp`),
  `EnvironmentEntry` (`Manifest.hpp`).
- Produces (all in namespace `vtextension`, header `UserSettableEnvironment.hpp`):

```cpp
/// What a user-set variable's value must be.
enum class UserValueShape : uint8_t { AbsolutePath, LocalOrSecureUrl, ProxyUrl, HostList };

/// One row of the user-settable table.
struct UserSettableVariable { std::string_view name; UserValueShape shape; };

/// The variables a user's `env_sets` may hold: a separate list from SettableVariables, which a manifest's
/// literal environment uses. No API key is offered.
inline constexpr auto UserSettableVariables = std::array {
    UserSettableVariable { .name = "CLAUDE_CONFIG_DIR", .shape = UserValueShape::AbsolutePath },
    UserSettableVariable { .name = "COPILOT_HOME", .shape = UserValueShape::AbsolutePath },
    UserSettableVariable { .name = "ANTHROPIC_BASE_URL", .shape = UserValueShape::LocalOrSecureUrl },
    UserSettableVariable { .name = "HTTPS_PROXY", .shape = UserValueShape::ProxyUrl },
    UserSettableVariable { .name = "HTTP_PROXY", .shape = UserValueShape::ProxyUrl },
    UserSettableVariable { .name = "NO_PROXY", .shape = UserValueShape::HostList },
};

/// The longest URL or host list a user may set, in bytes.
inline constexpr std::size_t MaxUserValueBytes = 2048;

/// Why a user-set value is refused.
enum class UserValueFault : uint8_t { NotSettable, NotAbsolutePath, NotAllowedUrl, NotUrl, NotHostList };

/// One row per UserValueFault, indexed by its value (static_assert as ManifestFaultTexts does).
struct UserValueFaultText { UserValueFault fault; std::string_view text; };
inline constexpr auto UserValueFaultTexts = std::array { /* one row each, texts below */ };

/// A named set of variables from the user's configuration (`daemon.extensions.<name>.env_sets.<set>`).
struct EnvironmentSet
{
    std::string name;                        ///< The set's name: an ArgumentKind::Name.
    std::vector<EnvironmentEntry> variables; ///< In the configuration's order.
    bool operator==(EnvironmentSet const&) const = default;
};

/// A variable dropped from a set, and why.
struct EnvironmentSetProblem { std::string set; std::string variable; UserValueFault fault; };

/// The sets as the host applies them, and what was dropped.
struct CheckedEnvironmentSets { std::vector<EnvironmentSet> sets; std::vector<EnvironmentSetProblem> problems; };

[[nodiscard]] UserSettableVariable const* userSettableRowOf(std::string_view name) noexcept;
[[nodiscard]] std::expected<std::string, UserValueFault> normalisedUserValue(
    std::string_view name, std::string_view value, std::string_view home, vtpty::PathStyle style);
[[nodiscard]] bool isNormalisedUserValue(std::string_view name, std::string_view value, vtpty::PathStyle style);
[[nodiscard]] std::string_view userValueFaultText(UserValueFault fault) noexcept;
[[nodiscard]] CheckedEnvironmentSets checkEnvironmentSets(
    std::span<EnvironmentSet const> sets, std::string_view home, vtpty::PathStyle style);
```

Rules (`normalisedUserValue`; name compared exactly, case-sensitive):
- not in the table → `NotSettable`.
- `AbsolutePath`: `rootSpellingOf(value, home, style)`; any error → `NotAbsolutePath`; the result is the spelling.
- `LocalOrSecureUrl` (`ANTHROPIC_BASE_URL`): `https://<authority>[/…]`, or `http://` whose host is `localhost`,
  `127.0.0.1` or `[::1]` (an optional `:<digits>` port after it); else `NotAllowedUrl`.
- `ProxyUrl`: scheme `http`, `https`, `socks5` or `socks5h`, then `://<authority>[/…]`; else `NotUrl`.
- Both URL shapes: lower-case scheme; at most `MaxUserValueBytes`; every byte printable ASCII `0x21`–`0x7E`;
  authority (up to the first `/`) non-empty, its host non-empty, no `@`. Returned unchanged.
- `HostList` (`NO_PROXY`): split at `,`; trim ASCII spaces off each item; every item non-empty and either `*` or
  only `[A-Za-z0-9._:/\[\]-]`; at most `MaxUserValueBytes`; else `NotHostList`. Returned as the trimmed items
  joined by `,`.
- `isNormalisedUserValue(name, value, style)` ≡ `normalisedUserValue(name, value, "", style) == value`.
- `checkEnvironmentSets`: keeps every set and its order; each variable replaced by its normalised value, or
  dropped with one `EnvironmentSetProblem`. A set left empty is kept.
- Fault texts: `NotSettable` "is not a variable a set may hold: CLAUDE_CONFIG_DIR, COPILOT_HOME, ANTHROPIC_BASE_URL,
  HTTPS_PROXY, HTTP_PROXY or NO_PROXY"; `NotAbsolutePath` "is not an absolute path, or one starting with ~";
  `NotAllowedUrl` "is not an https:// URL, or an http:// one to localhost, 127.0.0.1 or [::1], without credentials";
  `NotUrl` "is not an http, https or socks5 URL without credentials"; `NotHostList` "is not a comma-separated list
  of host names, domains or addresses".

- [ ] **Step 1: Write the failing tests** in `UserSettableEnvironment_test.cpp`, tag `[vtextension][settable][user]`:

```cpp
TEST_CASE("UserSettableEnvironment: values are normalised or refused by their row's shape", "[vtextension][settable][user]")
{
    using vtpty::PathStyle;
    struct Row { std::string_view name, value, home; PathStyle style; std::expected<std::string, UserValueFault> expected; };
    auto const rows = std::array {
        Row { "CLAUDE_CONFIG_DIR", "~/.claude-work", "/home/u", PathStyle::Posix, "/home/u/.claude-work" },
        Row { "CLAUDE_CONFIG_DIR", "C:/Users/me/.claude-work", "C:\\Users\\me", PathStyle::Windows, "C:\\Users\\me\\.claude-work" },
        Row { "COPILOT_HOME", "~\\.copilot-work", "C:\\Users\\me", PathStyle::Windows, "C:\\Users\\me\\.copilot-work" },
        Row { "CLAUDE_CONFIG_DIR", ".claude-work", "/home/u", PathStyle::Posix, std::unexpected(UserValueFault::NotAbsolutePath) },
        Row { "CLAUDE_CONFIG_DIR", "~/.claude-work", "", PathStyle::Posix, std::unexpected(UserValueFault::NotAbsolutePath) },
        Row { "ANTHROPIC_BASE_URL", "https://llm.example.com/v1", "", PathStyle::Posix, "https://llm.example.com/v1" },
        Row { "ANTHROPIC_BASE_URL", "http://localhost:8080", "", PathStyle::Posix, "http://localhost:8080" },
        Row { "ANTHROPIC_BASE_URL", "http://127.0.0.1:4000/x", "", PathStyle::Posix, "http://127.0.0.1:4000/x" },
        Row { "ANTHROPIC_BASE_URL", "http://[::1]:4000", "", PathStyle::Posix, "http://[::1]:4000" },
        Row { "ANTHROPIC_BASE_URL", "http://llm.example.com", "", PathStyle::Posix, std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "https://me:pw@llm.example.com", "", PathStyle::Posix, std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "https://", "", PathStyle::Posix, std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "HTTPS_PROXY", "http://proxy.example.com:3128", "", PathStyle::Posix, "http://proxy.example.com:3128" },
        Row { "HTTP_PROXY", "socks5://127.0.0.1:1080", "", PathStyle::Posix, "socks5://127.0.0.1:1080" },
        Row { "HTTPS_PROXY", "proxy.example.com:3128", "", PathStyle::Posix, std::unexpected(UserValueFault::NotUrl) },
        Row { "HTTPS_PROXY", "http://u:p@proxy.example.com", "", PathStyle::Posix, std::unexpected(UserValueFault::NotUrl) },
        Row { "HTTPS_PROXY", "http://proxy example", "", PathStyle::Posix, std::unexpected(UserValueFault::NotUrl) },
        Row { "NO_PROXY", "localhost, .example.com,10.0.0.0/8", "", PathStyle::Posix, "localhost,.example.com,10.0.0.0/8" },
        Row { "NO_PROXY", "*", "", PathStyle::Posix, "*" },
        Row { "NO_PROXY", "a,,b", "", PathStyle::Posix, std::unexpected(UserValueFault::NotHostList) },
        Row { "NO_PROXY", "a;b", "", PathStyle::Posix, std::unexpected(UserValueFault::NotHostList) },
        Row { "PATH", "/usr/bin", "", PathStyle::Posix, std::unexpected(UserValueFault::NotSettable) },
        Row { "claude_config_dir", "/x", "", PathStyle::Posix, std::unexpected(UserValueFault::NotSettable) },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name, row.value);
        CHECK(normalisedUserValue(row.name, row.value, row.home, row.style) == row.expected);
    }
    CHECK(isNormalisedUserValue("NO_PROXY", "a,b", vtpty::PathStyle::Posix));
    CHECK_FALSE(isNormalisedUserValue("NO_PROXY", "a, b", vtpty::PathStyle::Posix));
    CHECK_FALSE(isNormalisedUserValue("CLAUDE_CONFIG_DIR", "~/x", vtpty::PathStyle::Posix));
}

TEST_CASE("UserSettableEnvironment: a set keeps what may be set and reports the rest", "[vtextension][settable][user]")
{
    auto const sets = std::vector {
        EnvironmentSet { .name = "work", .variables = { { "CLAUDE_CONFIG_DIR", "~/.claude-work" }, { "PATH", "/bin" } } },
        EnvironmentSet { .name = "empty", .variables = { { "HTTPS_PROXY", "nope" } } },
    };
    auto const checked = checkEnvironmentSets(sets, "/home/u", vtpty::PathStyle::Posix);
    REQUIRE(checked.sets.size() == 2);
    CHECK(checked.sets[0] == EnvironmentSet { .name = "work", .variables = { { "CLAUDE_CONFIG_DIR", "/home/u/.claude-work" } } });
    CHECK(checked.sets[1].name == "empty");
    CHECK(checked.sets[1].variables.empty());
    REQUIRE(checked.problems.size() == 2);
    CHECK(checked.problems[0].set == "work");
    CHECK(checked.problems[0].variable == "PATH");
    CHECK(checked.problems[0].fault == UserValueFault::NotSettable);
    CHECK(checked.problems[1].fault == UserValueFault::NotUrl);
}
```

- [ ] **Step 2:** Add the files to `src/vtextension/CMakeLists.txt` (library sources and headers lists, and
  `vtextension_test`), add the header to `testing/PublicHeaders.cpp`; build `vtextension_test` → fails to compile
  (no header contents yet) or the cases fail.
- [ ] **Step 3: Implement** the header and `.cpp` as the Interfaces block says. A URL is split by hand (scheme up to
  `://`, authority up to the next `/`); no regex.
- [ ] **Step 4:** `cmake --build --preset clangcl-release --target vtextension_test`;
  `out/build/clangcl-release/bin/vtextension_test.exe "[user]"` → all pass; `vtextension_test.exe "[settable]"` → all
  pass. `clang-format -i` the three new files.
- [ ] **Step 5: Commit**

```bash
git add src/vtextension/UserSettableEnvironment.hpp src/vtextension/UserSettableEnvironment.cpp \
        src/vtextension/UserSettableEnvironment_test.cpp src/vtextension/CMakeLists.txt \
        src/vtextension/testing/PublicHeaders.cpp
git diff --cached --name-only   # exactly these five
git commit -m "feat(vtextension): the variables a user's set may hold, and their value shapes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: `env_sets` — read, written, and handed to the daemon — Haiku

**Files:**
- Modify: `src/contour/config/DaemonExtensionConfig.hpp` (new structs, new member)
- Modify: `src/contour/config/ConfigReader.hpp` (declare the reader beside `readDaemonExtensionPrograms`, ~:516;
  `daemonExtensions()` writer, ~:970)
- Modify: `src/contour/config/Config.cpp` (`readDaemonExtension`, ~:3981; the new reader after
  `readDaemonExtensionPrograms`, ~:4050)
- Modify: `src/contour/config/ConfigDocumentation.hpp` (the `extensions` comment of `DaemonConfig`, ~:1877)
- Modify: `src/vtextension/DaemonExtensionRuntime.hpp` (`DaemonExtensionDeclaration`)
- Modify: `src/contour/config/ExtensionDeclarations.cpp` (`daemonExtensionDeclarationOf`)
- Test: `src/contour/config/DaemonSettings_test.cpp`, `src/contour/config/ExtensionDeclarations_test.cpp`

**Interfaces:**
- Consumes: `vtextension::EnvironmentSet`, `vtextension::EnvironmentEntry` (Task 1).
- Produces:

```cpp
// contour/config/DaemonExtensionConfig.hpp
struct DaemonEnvironmentVariable { std::string name; std::string value; bool operator==(DaemonEnvironmentVariable const&) const = default; };
struct DaemonEnvironmentSet { std::string name; std::vector<DaemonEnvironmentVariable> variables; bool operator==(DaemonEnvironmentSet const&) const = default; };
// DaemonExtensionConfig:
std::vector<DaemonEnvironmentSet> environmentSets {}; ///< Its `env_sets`, in the configuration's order; values as written.

// ConfigReader.hpp, YAMLConfigReader:
[[nodiscard]] std::vector<DaemonEnvironmentSet> readDaemonExtensionEnvironmentSets(
    YAML::Node const& sets, std::string_view name, std::string_view keyPath) const;

// vtextension/DaemonExtensionRuntime.hpp, DaemonExtensionDeclaration:
/// The user's variable sets (`env_sets`), as configured: checked and normalised when the extension composes.
std::vector<EnvironmentSet> environmentSets = {};
```

Reader rules: `env_sets` not a map → one error, none read. A set whose name is not
`[A-Za-z0-9][A-Za-z0-9._-]{0,63}`, or whose value is not a map → skipped with an error naming it. A variable whose
name is not `[A-Za-z_][A-Za-z0-9_]*`, or whose value is not a scalar → skipped with an error
`"Skipping variable '{var}' of set '{set}' of extension '{name}' of {keyPath}: …; the set is kept without it."`.
A set given twice → the first stands. Values are kept as written (`~` is expanded by the host, Decision 1).
Writer, after `programs`, before `config`:

```
            env_sets:
                'claude-work':
                    'CLAUDE_CONFIG_DIR': '/opt/claude-work'
```

(each name and value through `SingleQuoted`; an empty set is written `'name': {}`).

- [ ] **Step 1: Write the failing tests** in `DaemonSettings_test.cpp`:

```cpp
TEST_CASE("daemon.extensions: env_sets name the variables a worker runs with", "[config][daemon]")
{
    auto const config = loadConfigFromYaml("daemon:\n"
                                           "  extensions:\n"
                                           "    orchestrator:\n"
                                           "      env_sets:\n"
                                           "        claude-work: { CLAUDE_CONFIG_DIR: '~/.claude-work', HTTPS_PROXY: 'http://proxy.example.com:3128' }\n"
                                           "        claude-personal: { CLAUDE_CONFIG_DIR: '~/.claude-personal' }\n");
    auto const& sets = config.daemon.value().extensions.front().environmentSets;
    REQUIRE(sets.size() == 2);
    CHECK(sets[0] == DaemonEnvironmentSet { .name = "claude-work",
                                            .variables = { { "CLAUDE_CONFIG_DIR", "~/.claude-work" },
                                                           { "HTTPS_PROXY", "http://proxy.example.com:3128" } } });
    CHECK(sets[1].name == "claude-personal");

    SECTION("and through the writer")
    {
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        CHECK(loadConfigFromYaml(written).daemon.value().extensions == config.daemon.value().extensions);
    }
}

TEST_CASE("daemon.extensions: a malformed set or variable costs only itself", "[config][daemon]")
{
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadConfigFromYaml("daemon:\n"
                                           "  extensions:\n"
                                           "    orchestrator:\n"
                                           "      env_sets:\n"
                                           "        'bad name': { CLAUDE_CONFIG_DIR: /x }\n"
                                           "        listed: [a, b]\n"
                                           "        ok: { 'not-a-var': x, NESTED: { a: b }, CLAUDE_CONFIG_DIR: /x }\n");
    auto const& sets = config.daemon.value().extensions.front().environmentSets;
    REQUIRE(sets.size() == 1);
    CHECK(sets[0] == DaemonEnvironmentSet { .name = "ok", .variables = { { "CLAUDE_CONFIG_DIR", "/x" } } });
    CHECK(capture.text().contains("set 'ok'"));
    CHECK(capture.text().contains("kept without it"));

    SECTION("an env_sets that is no map is ignored, the entry kept")
    {
        auto const list = loadConfigFromYaml("daemon:\n  extensions:\n    orchestrator:\n      env_sets: [a]\n");
        REQUIRE(list.daemon.value().extensions.size() == 1);
        CHECK(list.daemon.value().extensions.front().environmentSets.empty());
    }
}
```

  And in `ExtensionDeclarations_test.cpp`, extend "a daemon extension, as the daemon world takes it" (or add a
  case) so that a `DaemonExtensionConfig` with one set maps to `declaration.environmentSets ==
  std::vector { vtextension::EnvironmentSet { .name = "work", .variables = { { "CLAUDE_CONFIG_DIR", "/x" } } } }`.
- [ ] **Step 2:** `cmake --build --preset clangcl-release --target contour_test`;
  `contour_test.exe "daemon.extensions: *"` → the new cases fail (no member / not read).
- [ ] **Step 3: Implement** the structs, the reader (called from `readDaemonExtension` beside `programs`), the
  writer lines, the declaration member and its mapping. Add one sentence to the `extensions` comment of
  `DaemonConfig`: `env_sets names sets of variables (CLAUDE_CONFIG_DIR, …) the extension's workers run with;` keep
  the comment lines within the file's width.
- [ ] **Step 4:** Rebuild `contour_test`; `contour_test.exe "[daemon]"` and `contour_test.exe "[config]"` → all pass
  (the latter catches a documentation golden, if one compares the comment). Rebuild `vtextension_test` (the
  declaration changed) → compiles. `clang-format -i` every touched C++ file.
- [ ] **Step 5: Commit** — stage exactly the eight files above; `git diff --cached --name-only`; message
  `feat(config): read and write a daemon extension's env_sets`.

### Task 3: `pane-env-set` and `command-env-set` in the manifest — Opus

**Files:**
- Modify: `src/vtextension/Manifest.hpp` (binding struct, members, fault), `src/vtextension/Manifest.cpp`
  (handlers, `LineKinds`, the doc comment of `parseManifestText`)
- Create (corpus, `extensions/orchestrator/contract/manifest-v2/`): `valid/pane-env-set.manifest`,
  `valid/command-env-set.manifest`, `invalid/env-set-undeclared-target.manifest`,
  `invalid/env-set-undeclared-argument.manifest`, `invalid/env-set-argument-not-name.manifest`,
  `invalid/env-set-forbidden-variable.manifest`, `invalid/env-set-repeated.manifest`,
  `invalid/env-set-without-placeholder.manifest`
- Modify: `extensions/orchestrator/contract/README.md` (one paragraph on the two lines)
- Test: `src/vtextension/Manifest_test.cpp` (`InvalidRows`, a parse case), `extensions/orchestrator/core/Commands_test.cpp`
  (the line-grammar checker: `Fault`, `LineKinds`, its checks, `CorpusRows`)

**Interfaces:**
- Consumes: `userSettableRowOf()` (Task 1).
- Produces:

```cpp
/// A pane or command that takes variables from a user's set (`pane-env-set`, `command-env-set`).
struct EnvironmentSetBinding
{
    std::string argument;               ///< The `arg` (of kind name) whose value names the set.
    std::vector<std::string> variables; ///< What it may take from the set, each on UserSettableVariables.
    bool operator==(EnvironmentSetBinding const&) const = default;
};
// DeclaredCommand and PaneTemplate each gain:
std::optional<EnvironmentSetBinding> environmentSet {};
// ManifestFault gains, last (move the static_assert to it):
EnvSetNotName, ///< A pane-env-set or command-env-set whose argument is not of kind name.
// ManifestFaultTexts row: "a variable-set argument that is not of kind name"
```

Line grammar (version 2, value `Text`): `pane-env-set <pane> {<arg>} = <VAR> <VAR> …` and
`command-env-set <command> {<arg>} = <VAR> …`, words at single spaces. Checks in this order, each a fault:
head not exactly two words → `UnknownLine`; target not declared earlier → `EnvForUndeclared`; second head word not
a whole-word `{name}` → `UnknownLine`; argument not declared earlier → `UndeclaredPlaceholder`; argument's kind not
`Name` → `EnvSetNotName`; target already bound → `Duplicate`; a variable not `[A-Za-z_][A-Za-z0-9_]*` →
`MalformedEnvironmentName`; a variable twice → `Duplicate`; a variable not on `UserSettableVariables` →
`ForbiddenEnvironment`. The manifest digest (the module's SHA-256) covers these lines like any other; nothing to do
for it.

Corpus contents (every invalid file starts with `# <its one fault>`):
- `valid/pane-env-set.manifest`: `name = sets` / `version = 2` / `root worktrees = config:worktree_root` /
  `arg env-set = name` / `pane shell @worktrees = sh` /
  `pane-env-set shell {env-set} = CLAUDE_CONFIG_DIR HTTPS_PROXY`.
- `valid/command-env-set.manifest`: the same with `command who short @worktrees = id` and
  `command-env-set who {env-set} = CLAUDE_CONFIG_DIR NO_PROXY`.
- invalid, one change each from the valid pane file: target `other` (undeclared target); `{missing}`
  (undeclared argument); `arg env-set = text` (argument not name); variable `PATH` (forbidden variable); the
  `pane-env-set` line twice (repeated); `env-set` without braces (without placeholder).

- [ ] **Step 1: Write the corpus files** and the failing tests: `Manifest_test.cpp` `InvalidRows` gain
  `env-set-undeclared-target`/`EnvForUndeclared`, `env-set-undeclared-argument`/`UndeclaredPlaceholder`,
  `env-set-argument-not-name`/`EnvSetNotName`, `env-set-forbidden-variable`/`ForbiddenEnvironment`,
  `env-set-repeated`/`Duplicate`, `env-set-without-placeholder`/`UnknownLine`; and a new case:

```cpp
TEST_CASE("Manifest v2: a pane and a command bound to a user's set", "[vtextension][manifest]")
{
    auto const pane = parseManifestText(readCorpusFile(corpusDirectory() / "valid" / "pane-env-set.manifest"));
    REQUIRE(pane.has_value());
    CHECK(pane->panes.front().environmentSet
          == EnvironmentSetBinding { .argument = "env-set", .variables = { "CLAUDE_CONFIG_DIR", "HTTPS_PROXY" } });
    auto const command = parseManifestText(readCorpusFile(corpusDirectory() / "valid" / "command-env-set.manifest"));
    REQUIRE(command.has_value());
    CHECK(command->commands.front().environmentSet->variables == std::vector<std::string> { "CLAUDE_CONFIG_DIR", "NO_PROXY" });
    CHECK_FALSE(command->panes.size());
    // A version-1 manifest does not know the lines.
    CHECK(faultOf("name = a\nversion = 1\ncommand x = id\npane-env-set x {a} = NO_PROXY\n") == ManifestFault::Version2LineInVersion1);
}
```

  In `Commands_test.cpp`'s checker: `Fault` gains `EnvSetNotName` and `ForbiddenVariable` (a variable not among
  the six user-settable names, listed in the test as a `constexpr std::array`); `LineKinds` gain rows for both
  keywords (`headWords = 3`, `keyWords = 2`, valued, since 2, no root); the checks mirror the host's order above
  (`UndeclaredTarget` for the target); `CorpusRows` gain the eight files (valid → `Fault::None`; the invalid ones →
  `UndeclaredTarget`, `UndeclaredPlaceholder`, `EnvSetNotName`, `ForbiddenVariable`, `Duplicate`, `UnknownLine`).
- [ ] **Step 2:** Build `vtextension_test` and `orchestrator_test`, one after the other.
  `vtextension_test.exe "[manifest]"` and `orchestrator_test.exe "[corpus]"` → the new rows fail.
- [ ] **Step 3: Implement** the host side: the struct, the two handlers (one template over the target vector, as
  `addTargetEnvironment`), two `LineKinds` rows, the fault and its text row; update `parseManifestText`'s doc
  comment. Then the orchestrator checker. Update `contract/README.md`: a paragraph after the `title` one saying
  what the two lines mean, that the argument must be a declared `arg … = name`, and that the variables are the
  host's user-settable list.
- [ ] **Step 4:** Rebuild both; `vtextension_test.exe "[manifest]"`, `vtextension_test.exe "[consent]"`,
  `vtextension_test.exe "[settable]"`, `orchestrator_test.exe "[manifest]"` → all pass.
  `python scripts/check-guest-portability.py` → exit 0. `clang-format -i` the touched C++ files.
- [ ] **Step 5: Commit** — the two host files, the two test files, the eight corpus files and the contract README;
  `git diff --cached --name-only` lists exactly those 13; message
  `feat(vtextension): manifest lines that bind a pane or command to a user's variable set`.

### Task 4: A set reaches the pane and the command it is named for — Opus

**Files:**
- Create: `src/vtextension/EnvironmentSets.hpp`, `src/vtextension/EnvironmentSets.cpp`
- Modify: `src/vtextension/DeclaredPanes.hpp/.cpp` (`DeclaredPanesConfig`, `open`)
- Modify: `src/vtextension/DeclaredProcesses.hpp/.cpp` (`DeclaredProcessesConfig`, `start`)
- Modify: `src/vtextension/DaemonExtensionRuntime.cpp` (`compose`, `makePanes`; the `Extension` struct)
- Modify: `src/vtextension/CMakeLists.txt`, `src/vtextension/testing/PublicHeaders.cpp`
- Test: `src/vtextension/DeclaredPanes_test.cpp`, `src/vtextension/DeclaredProcesses_test.cpp`,
  `src/vtextension/DaemonExtensionRuntime_test.cpp`

**Interfaces:**
- Consumes: `EnvironmentSetBinding` (Task 3), `EnvironmentSet`, `checkEnvironmentSets`, `isNormalisedUserValue`,
  `userValueFaultText` (Task 1), `DaemonExtensionDeclaration::environmentSets` (Task 2).
- Produces:

```cpp
// EnvironmentSets.hpp
/// A call's arguments with the set argument taken out, and the variables the named set gives.
struct SetSelection
{
    ConfigMap arguments;                       ///< What the template's placeholders are filled from.
    std::vector<EnvironmentEntry> environment; ///< The binding's variables the set holds, in the binding's order.
};

/// Applies a pane's or command's `*-env-set` binding to one call.
/// @return The selection; ArgumentFault::NotItsKind when the set argument is not a Name, names no set, or a
///         listed value fails isNormalisedUserValue(). Without a binding, or with the set argument left out, the
///         arguments unchanged and no environment (an `env-set` passed to a template without a binding is then
///         refused by substituteDeclared() as NotAPlaceholder).
[[nodiscard]] std::expected<SetSelection, ArgumentFault> selectEnvironmentSet(
    std::optional<EnvironmentSetBinding> const& binding,
    ConfigMap const& args,
    std::span<EnvironmentSet const> sets,
    vtpty::PathStyle style);

// DeclaredPanesConfig and DeclaredProcessesConfig each gain:
std::vector<EnvironmentSet> environmentSets = {}; ///< The user's sets, checked (checkEnvironmentSets()).
```

Behaviour:
- `DeclaredPanes::open`: after the template is found and before `substituteDeclared`, call
  `selectEnvironmentSet(declared->environmentSet, args, _config.environmentSets, _directories.pathStyle())`; a
  fault → `PaneError::ArgumentRejected`. Substitute from `selection.arguments`. The spawn's environment is
  `paneEnvironment(declared->environment ++ selection.environment, inherited)`.
- `DeclaredProcesses::start`: the same after the grant check; `ProcessError::ArgumentRejected`; the command's
  layer passed to `childEnvironment` is `declared->environment ++ selection.environment`.
- Variables the binding lists but the set does not hold, and every other variable, are not touched: inherited
  from the daemon unless withheld (spec).
- `DaemonExtensionRuntime::compose`: `checkEnvironmentSets(declaration.environmentSets, _home,
  _services.directories.pathStyle())`; log each problem with `daemonExtensionLog()` as
  `"daemon.extensions.{name}.env_sets.{set}.{VAR} {faultText}; the set is kept without it."`; keep the checked
  sets on the `Extension` (`environmentSets`) and pass them into `DeclaredProcessesConfig` and, in `makePanes`,
  `DeclaredPanesConfig`.

- [ ] **Step 1: Write the failing tests.** `DeclaredPanes_test.cpp` (reuse its fixture and fake host; parse a
  manifest text holding `arg env-set = name`, `pane agent @worktrees = claude --model {model}` and
  `pane-env-set agent {env-set} = CLAUDE_CONFIG_DIR NO_PROXY`; sets `{ work: CLAUDE_CONFIG_DIR=/home/u/.claude-work,
  COPILOT_HOME=/home/u/.copilot }`):

```cpp
TEST_CASE("pane-open applies the listed variables of the set the call names", "[vtextension][panes][sets]")
{
    // open("agent", { {"model","opus"}, {"env-set","work"} }, …) succeeds;
    // spawned.argv == { "claude", "--model", "opus" }  (the set argument is not a placeholder)
    // spawned.environment contains { "CLAUDE_CONFIG_DIR", "/home/u/.claude-work" }
    // and no "COPILOT_HOME" (the set holds it, the binding does not list it) and no "NO_PROXY" (not in the set)
}
TEST_CASE("pane-open without a set argument applies no set", "[vtextension][panes][sets]")
{
    // open("agent", { {"model","opus"} }, …) succeeds; no CLAUDE_CONFIG_DIR in spawned.environment
}
TEST_CASE("pane-open refuses a set it cannot apply", "[vtextension][panes][sets]")
{
    // each → PaneError::ArgumentRejected, and nothing spawned:
    //   {"env-set","gone"} (no such set); {"env-set","bad name"} (not a Name);
    //   a set whose CLAUDE_CONFIG_DIR is "relative" in the config (fails the re-check);
    //   a pane template with no binding given {"env-set","work"} (not a placeholder)
}
```

  `DeclaredProcesses_test.cpp`: the same three for `start()` with `command-env-set`, checking the runner's
  `ProcessSpec.environment` and `ProcessError::ArgumentRejected`. `DaemonExtensionRuntime_test.cpp`:

```cpp
TEST_CASE("DaemonExtensionRuntime: a set's dropped variable is logged and the set kept", "[daemon][runtime][sets]")
{
    // declaration.environmentSets = { { "work", { {"CLAUDE_CONFIG_DIR","relative"}, {"NO_PROXY","localhost"} } } };
    // create() reports no problem (the declaration composes); the error log (ErrorCapture) holds
    // "daemon.extensions.good.env_sets.work.CLAUDE_CONFIG_DIR" and "kept without it".
}
```

- [ ] **Step 2:** Build `vtextension_test`; `vtextension_test.exe "[sets]"` → fails.
- [ ] **Step 3: Implement** `EnvironmentSets.hpp/.cpp`, the two config members, the call sites, and `compose`.
- [ ] **Step 4:** Rebuild; `vtextension_test.exe "[sets]"`, `"[panes]"`, `"[processes]"`, `"[runtime]"` → all pass;
  then the whole `vtextension_test.exe` → all pass. Build `vthost_test` (it constructs declarations) and run the
  whole `vthost_test.exe` → all pass. `clang-format -i`.
- [ ] **Step 5: Commit** — the new pair, the four modified sources and their headers, the runtime, CMakeLists,
  PublicHeaders and the three tests; `git diff --cached --name-only`; message
  `feat(vtextension): apply the user's variable set a pane or command call names`.

### Task 5: The consent dialog shows the sets, and a changed set asks again — Opus

**Files:**
- Modify: `src/vtextension/ExtensionConsent.hpp/.cpp` (`DaemonConsentRequest`, `daemonConsentQuestion` and its doc)
- Modify: `src/vtextension/RootPointer.hpp/.cpp` (`grantDigestOf`)
- Modify: `src/vtextension/DaemonExtensionRuntime.cpp` (`compose`: pass the checked sets to both)
- Test: `src/vtextension/ExtensionConsent_test.cpp`, `src/vtextension/RootPointer_test.cpp`,
  `src/vtextension/DaemonExtensionRuntime_test.cpp`

**Interfaces:**
- Consumes: `EnvironmentSet` (Task 1), `EnvironmentSetBinding` (Task 3), the checked sets on `Extension` (Task 4).
- Produces:

```cpp
// DaemonConsentRequest gains, last:
std::vector<EnvironmentSet> environmentSets = {}; ///< The user's sets, checked: what the bound panes and commands may take.

/// @return As before, with sets appended: `set\t<name>\n` then `<NAME>=<value>\n` per variable, per set, in order,
///         after the roots -- nothing appended when @p sets is empty, so a grant given without sets stays valid.
[[nodiscard]] std::string grantDigestOf(std::string_view manifestDigest,
                                        std::span<ResolvedRoot const> roots,
                                        std::span<EnvironmentSet const> sets = {});
```

Consent text (exact; every name and value through `neutralizedForDisplay`; all `ConsentWeight::CanRunCommands`):
- after a command's own environment lines, and after a pane's, when it has a binding:
  `  From the variable set the extension names: CLAUDE_CONFIG_DIR, ANTHROPIC_BASE_URL` (the binding's variables,
  `, `-joined, in order);
- right after the last pane (before the `pane-input` line), when `environmentSets` is not empty:
  `Variable sets from your configuration:`, then per set `  {set}:` and per variable `    {NAME}={value}`; a set
  with no variable left shows `  {set}: (empty)`.

- [ ] **Step 1: Write the failing tests.**
  - `RootPointer_test.cpp`: `grantDigestOf(d, roots, {}) == grantDigestOf(d, roots)` and equals the value the
    existing test pins (the pre-change digest); a set changes the digest; a changed value changes it again; the
    order of two sets changes it.
  - `ExtensionConsent_test.cpp`, a case on a small manifest (one pane with `pane-env-set`, one command with
    `command-env-set`) and two sets: the joined question contains, in order, the pane line, its
    `  From the variable set the extension names: CLAUDE_CONFIG_DIR, NO_PROXY`, the sets block
    (`Variable sets from your configuration:` / `  work:` / `    CLAUDE_CONFIG_DIR=/home/u/.claude-work` /
    `  empty: (empty)`); each of those lines weighs `CanRunCommands`. The existing orchestrator golden case still
    passes unchanged (its manifest has no binding yet, its request no sets).
  - `DaemonExtensionRuntime_test.cpp`, after the "a moved root asks again" pattern:

```cpp
TEST_CASE("DaemonExtensionRuntime: a set added, changed or removed asks again", "[daemon][runtime][consent][sets]")
{
    // Grant "good" declared with sets { work: CLAUDE_CONFIG_DIR=/a }; keep digest.
    // SECTION "unchanged sets do not ask again": recreate with the same sets → pending() empty.
    // SECTION "a changed value asks again": CLAUDE_CONFIG_DIR=/b → pending().size()==1, digest differs,
    //   and its question holds "    CLAUDE_CONFIG_DIR=/b".
    // SECTION "a removed set asks again": no sets → pending().size()==1.
    // SECTION "an added set asks again": sets { work, other } → pending().size()==1.
}
TEST_CASE("DaemonExtensionRuntime: a grant from before sets existed holds", "[daemon][runtime][consent][sets]")
{
    // A declaration without sets: its pending digest == grantDigestOf(manifestDigest, roots) (the old formula).
}
TEST_CASE("DaemonExtensionRuntime: a dropped variable is missing from the question", "[daemon][runtime][consent][sets]")
{
    // sets { work: CLAUDE_CONFIG_DIR=relative, NO_PROXY=localhost } → the question shows "    NO_PROXY=localhost"
    // and no line containing "CLAUDE_CONFIG_DIR=" (Review Focus 5).
}
```

- [ ] **Step 2:** Build `vtextension_test`; `vtextension_test.exe "[consent]"` and `"[roots]"` → the new cases fail.
- [ ] **Step 3: Implement** the defaulted parameter, the request member, the text, and in `compose` pass
  `extension->environmentSets` to both `grantDigestOf` and the request. Update `daemonConsentQuestion`'s doc
  comment with the two new line kinds.
- [ ] **Step 4:** Rebuild; the whole `vtextension_test.exe` → all pass. `clang-format -i`.
- [ ] **Step 5: Commit** — the eight files (five sources, three tests); `git diff --cached --name-only`; message
  `feat(vtextension): show the user's variable sets on the consent dialog, and ask again when they change`.

### Task 6: The orchestrator's manifest declares the bindings and a Copilot pane — Haiku

**Files:**
- Modify: `extensions/orchestrator/core/Commands.hpp/.cpp` (constants, `setVariables` on the two decls, tables,
  `isNamedInManifest`, `manifestLines` and its doc)
- Modify: `extensions/orchestrator/contract/manifest-v2/valid/orchestrator.manifest`,
  `extensions/orchestrator/contract/manifest-v2/valid/orchestrator.consent.txt` (regenerated goldens)
- Test: `extensions/orchestrator/core/Commands_test.cpp`

**Interfaces:**
- Consumes: the manifest grammar (Task 3), the consent text (Task 5).
- Produces (`Commands.hpp`):

```cpp
/// The argument whose value names the user's variable set a call takes (`pane-env-set`, `command-env-set`).
inline constexpr std::string_view EnvSetArgument = "env-set";
/// The pane template a Copilot worker runs in.
inline constexpr std::string_view CopilotPaneTemplate = "copilot";
// CommandDecl and PaneDecl each gain, last:
std::span<std::string_view const> setVariables = {}; ///< What it may take from the set the call names; empty: no binding.
```

The manifest gains exactly these lines (order: `arg env-set = name` last among the `arg` lines; the
`command-env-set` lines after every `command-env-for` line, in command-table order; `pane copilot` after
`pane agent-plain`; its `pane-env` lines after `agent-plain`'s; the `pane-env-set` lines after every `pane-env`):

```
arg env-set = name
command-env-set agent-setup-check {env-set} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY
command-env-set claude-plugins {env-set} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY
command-env-set claude-marketplace-add {env-set} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY
command-env-set claude-plugin-install {env-set} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY
command-env-set agent-transcript {env-set} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY
command-env-set agent-stats {env-set} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY
pane copilot @worktrees = copilot --model {model} -i {prompt}
pane-env copilot GIT_CONFIG_COUNT = 2          (and the four GIT_CONFIG_* lines the agent panes have)
pane-env-set agent {env-set} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY
pane-env-set agent-plain {env-set} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY
pane-env-set copilot {env-set} = COPILOT_HOME HTTPS_PROXY HTTP_PROXY NO_PROXY
```

`agent-record` gets no binding (it writes the daemon's runs file, not Claude's state). Two `constexpr` arrays hold
the variable lists (`ClaudeSetVariables`, `CopilotSetVariables`); `isNamedInManifest` also counts a name a binding
uses.

- [ ] **Step 1: Write the failing test** in `Commands_test.cpp`:

```cpp
TEST_CASE("manifestText binds the Claude commands and panes to a worker's set", "[orchestrator][commands][manifest]")
{
    auto const lines = manifestLines();
    auto const has = [&](std::string_view line) { return std::ranges::contains(lines, std::string(line)); };
    CHECK(has("arg env-set = name"));
    for (auto const name: { "agent-setup-check"sv, "claude-plugins"sv, "claude-marketplace-add"sv,
                            "claude-plugin-install"sv, "agent-transcript"sv, "agent-stats"sv })
        CHECK(has(std::format("command-env-set {} {{env-set}} = CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY NO_PROXY", name)));
    CHECK_FALSE(std::ranges::any_of(lines, [](std::string const& line) { return line.starts_with("command-env-set agent-record "); }));
    CHECK(has("pane copilot @worktrees = copilot --model {model} -i {prompt}"));
    CHECK(has("pane-env-set copilot {env-set} = COPILOT_HOME HTTPS_PROXY HTTP_PROXY NO_PROXY"));
    CHECK(findPaneTemplate(CopilotPaneTemplate) != nullptr);
}
```

- [ ] **Step 2:** Build `orchestrator_test`; run the case → fails.
- [ ] **Step 3: Implement** the tables. Regenerate the goldens: build the `orchestrator_manifest` target and write
  its output over `valid/orchestrator.manifest` from **Git Bash**, never PowerShell (whose `>` writes UTF-16):
  `out/build/clangcl-release/bin/orchestrator_manifest.exe >
  extensions/orchestrator/contract/manifest-v2/valid/orchestrator.manifest` (the tool writes LF bytes); then build
  `vtextension_test`, run `vtextension_test.exe "ExtensionConsent: the daemon question for the orchestrator"`, and
  update `orchestrator.consent.txt` to the rendered text (the case prints it on failure; the only differences may
  be the new `copilot` pane, its environment lines and the seven `From the variable set …` lines). Read the diff
  of both goldens: nothing else may change.
- [ ] **Step 4:** `orchestrator_test.exe` (all), `vtextension_test.exe "[corpus]"`, `vtextension_test.exe
  "[consent]"`, `vtextension_test.exe "[settable]"`, `ctest --test-dir out/build/clangcl-release -R
  orchestrator_manifest_matches_corpus` → all pass. `python scripts/check-guest-portability.py` → 0.
  `clang-format -i` the C++ files.
- [ ] **Step 5: Commit** — the two Commands files, the test, the two goldens; message
  `feat(orchestrator): declare the worker set bindings and a Copilot pane`.

### Task 7: Workers in the orchestrator's configuration — Haiku

**Files:**
- Create: `extensions/orchestrator/core/Agents.hpp`, `extensions/orchestrator/core/Agents.cpp`
- Modify: `extensions/orchestrator/core/ConfigJson.hpp` (`JsonWorker`, `JsonTask::worker`, `JsonConfig::workers`)
- Modify: `extensions/orchestrator/core/Config.hpp/.cpp` (glaze meta, field tables and their mirror asserts,
  `WorkerDef`, `TaskDef::worker`, accessors, problems and their `describe()` texts)
- Modify: `extensions/orchestrator/CMakeLists.txt` (`orchestrator_core` sources)
- Test: `extensions/orchestrator/core/Config_test.cpp`

**Interfaces:**
- Consumes: `AgentPaneTemplate`, `AgentPlainPaneTemplate`, `CopilotPaneTemplate` (Task 6).
- Produces:

```cpp
// Agents.hpp -- one row per agent kind.
enum class AgentKind : uint8_t { Claude = 0, Copilot };
enum class StatusReporting : uint8_t { Silent = 0, Reported }; ///< Whether its agent reports working/blocked/done.
enum class ClaudeState : uint8_t { None = 0, Read };           ///< Whether it reads Claude Code's state: hooks, plugins, transcripts, stats.
enum class SettingsProfile : uint8_t { NotTaken = 0, Taken };  ///< Whether it takes a permission profile (`--settings`).
struct AgentRow
{
    AgentKind kind;
    std::string_view name;       ///< As `workers[].agent` and the stored run spell it.
    std::string_view boardPane;  ///< Its pane template when a board is asked for.
    std::string_view plainPane;  ///< Its pane template otherwise.
    StatusReporting status;
    ClaudeState claudeState;
    SettingsProfile settings;
};
inline constexpr auto AgentRows = std::array {
    AgentRow { .kind = AgentKind::Claude, .name = "claude", .boardPane = AgentPaneTemplate, .plainPane = AgentPlainPaneTemplate,
               .status = StatusReporting::Reported, .claudeState = ClaudeState::Read, .settings = SettingsProfile::Taken },
    AgentRow { .kind = AgentKind::Copilot, .name = "copilot", .boardPane = CopilotPaneTemplate, .plainPane = CopilotPaneTemplate,
               .status = StatusReporting::Silent, .claudeState = ClaudeState::None, .settings = SettingsProfile::NotTaken },
};
[[nodiscard]] AgentRow const& agentRowOf(AgentKind kind) noexcept;
[[nodiscard]] AgentRow const* findAgent(std::string_view name) noexcept;

// Config.hpp
/// The worker a configuration without `workers` has: today's behaviour.
inline constexpr std::string_view ImplicitWorkerId = "claude";
struct WorkerDef
{
    std::string id;                    ///< Unique; an ArgumentKind::Name.
    AgentKind agent = AgentKind::Claude;
    std::string envSet = {};           ///< A set name, passed as is (the host checks it); empty: none.
    [[nodiscard]] bool operator==(WorkerDef const&) const = default;
};
// TaskDef gains: std::string worker = {}; ///< The worker that runs it: always one of Config::workers().
// ConfigProblem gains, last: UnknownAgent, UnknownWorker, ProfileOnCopilot.
// Config gains:
[[nodiscard]] std::span<WorkerDef const> workers() const noexcept;            ///< Never empty.
[[nodiscard]] WorkerDef const* findWorker(std::string_view id) const noexcept;
[[nodiscard]] WorkerDef const& workerFor(std::string_view taskId) const noexcept; ///< The first worker for an unknown task.
[[nodiscard]] std::span<ConfigError const> notices() const noexcept;          ///< What was accepted with a fallback.
```

Rules: `workers` absent → `{ { ImplicitWorkerId, Claude, "" } }`; present and `[]` → `workers` `Empty`;
`workers[i].id` missing → `Missing`, not a Name → `NotAName`, repeated → `Duplicate`; `agent` missing → `Missing`,
unknown → `UnknownAgent`; `env_set` not a Name → `NotAName`; unknown key → `UnknownKey`. `tasks[i].worker` absent
→ the first worker; not a Name → `NotAName` (fatal); naming no worker → a *notice* `{ "tasks[i].worker",
UnknownWorker, <the name> }` and the first worker. A task whose resolved worker's row has `SettingsProfile::NotTaken`
and that has its own `permissions` → fatal `{ "tasks[i].permissions", ProfileOnCopilot }`. `describe()` texts:
`UnknownAgent` "is not an agent: claude or copilot"; `UnknownWorker` "names no worker; the first worker runs it";
`ProfileOnCopilot` "is a permission profile, which a Copilot worker does not take".

- [ ] **Step 1: Write the failing tests** in `Config_test.cpp`: `Config.defaults` gains
  `CHECK(config->workers().size() == 1); CHECK(config->workers()[0] == WorkerDef { .id = "claude", .agent =
  AgentKind::Claude, .envSet = {} }); CHECK(config->tasks()[0].worker == "claude"); CHECK(config->notices().empty());`.
  `RejectRows` gain (find/replace on `Example`, inserting `"workers": [...],` after `"max_agents": 4,`):

```cpp
RejectRow { "an empty workers", { .find = R"("max_agents": 4,)", .replace = R"("max_agents": 4, "workers": [],)" }, "workers", ConfigProblem::Empty },
RejectRow { "a worker without id", { .find = R"("max_agents": 4,)", .replace = R"("max_agents": 4, "workers": [ { "agent": "claude" } ],)" }, "workers[0].id", ConfigProblem::Missing },
RejectRow { "a worker id twice", { .find = R"("max_agents": 4,)", .replace = R"("max_agents": 4, "workers": [ { "id": "a", "agent": "claude" }, { "id": "a", "agent": "copilot" } ],)" }, "workers[1].id", ConfigProblem::Duplicate },
RejectRow { "an unknown agent", { .find = R"("max_agents": 4,)", .replace = R"("max_agents": 4, "workers": [ { "id": "a", "agent": "codex" } ],)" }, "workers[0].agent", ConfigProblem::UnknownAgent },
RejectRow { "an env_set that is no Name", { .find = R"("max_agents": 4,)", .replace = R"("max_agents": 4, "workers": [ { "id": "a", "agent": "claude", "env_set": "a b" } ],)" }, "workers[0].env_set", ConfigProblem::NotAName },
```

  and a new case:

```cpp
TEST_CASE("workers: each task names one, an unknown one falls back with a notice", "[config][workers]")
{
    // Example with "workers": [ {id: work, agent: claude, env_set: claude-work}, {id: copilot, agent: copilot} ],
    // fix_ci "worker": "copilot", address_review "worker": "nobody", issue without worker:
    //   workers().size()==2; workers()[0].envSet=="claude-work"; workers()[1].agent==AgentKind::Copilot;
    //   tasks()[0].worker=="copilot"; tasks()[1].worker=="work"; tasks()[2].worker=="work";
    //   notices().size()==1; notices()[0].key=="tasks[1].worker"; .problem==UnknownWorker; .subject=="nobody";
    //   workerFor("fix_ci").agent==AgentKind::Copilot; workerFor("gone").id=="work".
    // The same with fix_ci also "permissions": "default" → error key "tasks[0].permissions", ProfileOnCopilot.
}
```

- [ ] **Step 2:** Build `orchestrator_test`; `orchestrator_test.exe "[config]"` → the new rows fail.
- [ ] **Step 3: Implement** `Agents.hpp/.cpp` and the configuration. Follow the file's existing pattern exactly:
  a `Field` table per object kind with its `matchesMirror` static_assert, `checked(...)` helpers, the `Values`
  aggregate, and the `glz::meta` mapping `env_set` → `envSet`.
- [ ] **Step 4:** Rebuild; `orchestrator_test.exe "[config]"` and the whole `orchestrator_test.exe` → all pass.
  `python scripts/check-guest-portability.py` and `python scripts/check-include-budget.py` → 0. `clang-format -i`.
- [ ] **Step 5: Commit** — the new pair, `ConfigJson.hpp`, `Config.hpp`, `Config.cpp`, `CMakeLists.txt`,
  `Config_test.cpp`; message `feat(orchestrator): workers in the configuration, named by each task`.

### Task 8: A run records its worker and set, and every call for it passes the set — Opus

**Files:**
- Modify: `extensions/orchestrator/core/Run.hpp/.cpp`, `RunJson.hpp` (fields, `agentPaneTemplateOf`,
  `agentPaneArguments`, `isStorable`, `toJson`/`runFromJson`)
- Modify: `extensions/orchestrator/core/EngineDispatch.cpp` (`preview`), `EngineTranscript.cpp`
  (`issueTranscript`), `EngineStats.cpp` (`issueStats`), `Engine.cpp` (start problems from `Config::notices()`;
  `rebuildProblems`; the pane-open failure)
- Test: `extensions/orchestrator/core/Run_test.cpp`, `extensions/orchestrator/core/Engine_test.cpp`

**Interfaces:**
- Consumes: `WorkerDef`, `Config::workerFor`, `Config::notices`, `AgentKind`, `agentRowOf` (Task 7);
  `EnvSetArgument`, `CopilotPaneTemplate` (Task 6).
- Produces:

```cpp
// Run gains, after `settings`:
std::string worker {};               ///< The worker's id, a Name; empty: a run stored before workers (the implicit one).
AgentKind agent = AgentKind::Claude; ///< Its agent: kept with the run, so a removed worker's runs still end right.
std::string envSet {};               ///< The worker's set when it started, a Name; empty: none.
// JsonRun gains: std::optional<std::string> worker, agent, envSet; written only when not the default
// (worker non-empty, agent "copilot", envSet non-empty); absent reads as the default.

/// @return The run's set as the argument a call for it passes; nothing without a set.
[[nodiscard]] std::optional<Argument> envSetArgumentOf(Run const& run);
```

Behaviour:
- `preview()`: `auto const& worker = _config->workerFor(task->id)`; the run gets `worker.id`, `worker.agent`,
  `worker.envSet`. For an agent whose row has `SettingsProfile::NotTaken`: `permissions` empty, `settings`
  `EmptySettingsJson`, `board` empty.
- `agentPaneTemplateOf(run)`: the row's `boardPane` when `run.board` is non-empty, else `plainPane`.
- `agentPaneArguments(run)`: Claude as today; Copilot only `model` and `prompt`; both followed by
  `envSetArgumentOf(run)` when it has one. Never `session`/`settings`/`board` for Copilot (the host refuses an
  argument its template does not name).
- `issueTranscript`, `issueStats`: append `envSetArgumentOf(run)` when present. No other `RunCommand` of a run
  carries it (git and forge commands have no binding).
- A run with a set whose `PaneOpenFailedFor` / transcript / stats call fails with `why == "bad-argument"` adds a
  problem line (kept until the next successful call for that set): `the variable set '{set}' of worker
  '{worker}' was refused: is it still under daemon.extensions.orchestrator.env_sets?`
- `Engine::start` puts one line per `Config::notices()` entry into `_startProblems`, through `message()` like the
  configuration's errors.

- [ ] **Step 1: Write the failing tests.**
  - `Run_test.cpp`: a run with `worker = "work"`, `agent = AgentKind::Copilot`, `envSet = "copilot-work"` round-trips
    through `toJson`/`runFromJson`; a run with the defaults writes JSON holding none of `"worker"`, `"agent"`,
    `"envSet"`; the stored text of an existing fixture run (one the file already reads, without the keys) reads
    back with `worker.empty()`, `agent == AgentKind::Claude`, `envSet.empty()` (Review Focus 2); `isStorable`
    refuses `worker = "a b"` and `envSet = "a b"`; `agentPaneArguments` of a Copilot run with a set is exactly
    `{ {"model",…}, {"prompt",…}, {"env-set","copilot-work"} }`, and `agentPaneTemplateOf` is `"copilot"`.
  - `Engine_test.cpp`:

```cpp
TEST_CASE("without workers every call is as it was", "[orchestrator][engine][workers]")
{
    // booted(); the PR dispatch to Running, then a transcript request and a stats read:
    // no OpenPane or RunCommand argument is named "env-set"; the OpenPane arguments' names are exactly
    // model, session, prompt, settings, board (Review Focus 3).
}
TEST_CASE("a worker's set reaches its tab, transcript and stats", "[orchestrator][engine][workers]")
{
    // booted(Settings { .limits = R"("workers": [ { "id": "work", "agent": "claude", "env_set": "claude-work" } ],)" });
    // PR dispatch: the run's worker=="work", envSet=="claude-work"; OpenPane has {"env-set","claude-work"};
    // git-fetch/git-worktree-add/gh-pr-checkout have no "env-set"; the transcript's and stats' RunCommand have it.
}
TEST_CASE("a run keeps its set when its worker is removed", "[orchestrator][engine][workers]")
{
    // Store a Running run with worker "gone", envSet "old-set"; boot with no workers; its transcript call
    // still carries {"env-set","old-set"}.
}
TEST_CASE("a refused set is a problem line naming it", "[orchestrator][engine][workers]")
{
    // A worker with env_set "gone"; dispatch; feed PaneOpenFailedFor { .why = "bad-argument" };
    // mentions(problems(), "variable set 'gone'") (Review Focus 4).
}
TEST_CASE("an unknown task worker is a problem line, and the first worker runs it", "[orchestrator][engine][workers]")
{
    // fixCiKeys = R"("worker": "nobody",)"; mentions(problems(), "tasks[0].worker"); the dispatch opens "agent".
}
```

- [ ] **Step 2:** Build `orchestrator_test`; `orchestrator_test.exe "[workers]"` and `"[run]"` → the new cases fail.
- [ ] **Step 3: Implement** as above. Keep `RunSchema` at 3.
- [ ] **Step 4:** Rebuild; the whole `orchestrator_test.exe` → all pass (existing cases pin today's arguments).
  `python scripts/check-guest-portability.py` → 0. `clang-format -i`.
- [ ] **Step 5: Commit** — the touched core files and the two tests; message
  `feat(orchestrator): a run keeps its worker and set, and passes the set on every call for it`.

### Task 9: The hooks check and the plugin commands, per Claude worker — Opus

**Files:**
- Modify: `extensions/orchestrator/core/Engine.hpp`, `EnginePoll.cpp` (`checkHooks`, `checkPlugins`,
  `installPlugins`, `issuePluginStep`, `onHooksResult`, `onPluginsResult`, `onSetupResult`, `isSetupTag`),
  `Engine.cpp` (`rebuildProblems`), `Hooks.hpp` (the per-worker line)
- Modify (callers of the accessors, if their meaning needs it): `extensions/orchestrator/view/DispatchForm.cpp`,
  `view/Actions.cpp`, `view/Dashboard.cpp`
- Test: `extensions/orchestrator/core/Engine_test.cpp`

**Interfaces:**
- Consumes: `Config::workers()`, `agentRowOf(...).claudeState` (Task 7); `EnvSetArgument` (Task 6).
- Produces:

```cpp
// Engine:
[[nodiscard]] HooksState hooks() const noexcept;                       ///< Missing if any Claude set's is; else Unknown if any is; else Present.
[[nodiscard]] HooksState hooksFor(std::string_view envSet) const noexcept; ///< One set's ("" = no set); Unknown before its check.
[[nodiscard]] std::span<PluginRequirement const> missingPlugins() const noexcept; ///< Union over the sets, each id once, in `plugins` order.
// Hooks.hpp:
/// @param workers The ids of the workers sharing the set, in configuration order.
/// @return `Claude Code's status hooks are missing for worker <a>` (or `for workers <a>, <b>`), then today's
///         `: blocked and permission states may not show; run `contour agent setup`` with that CLAUDE_CONFIG_DIR.
[[nodiscard]] std::string hooksMissingProblemFor(std::span<std::string const> workers);
```

Behaviour: the distinct sets of the workers whose row reads Claude's state, in configuration order (`""` for a
worker without a set). One hooks check per set (tag `poll/hooks` for `""`, `poll/hooks:<set>` otherwise), each
with `{ "env-set", <set> }` when the set is not empty; one plugins check per set likewise (`poll/plugins…`); one
plugin-install queue whose steps carry their set (`PluginStep::envSet`). No Claude worker → no hooks or plugins
check at all. Problem lines: only the implicit worker → `HooksMissingProblem` verbatim and today's plugin line;
configured workers → `hooksMissingProblemFor(<the set's workers>)` and `Claude Code (worker <a>) lacks the plugin
<id> · ^K installs it`. A plugin-install failure line names the worker too.

- [ ] **Step 1: Write the failing tests** in `Engine_test.cpp`, tag `[orchestrator][engine][workers][hooks]`:
  - two Claude workers with sets `a` and `b` and a third Claude worker on set `a`, plus a Copilot worker → the
    start issues exactly two `agent-setup-check` commands, with `env-set` `a` and `b` (none for Copilot); answering
    `a` with `setup-hooks-missing.json` and `b` with `setup-hooks-present.json` gives `hooksFor("a") == Missing`,
    `hooksFor("b") == Present`, `hooks() == Missing`, and exactly one problem line containing
    `missing for workers w1, w3`;
  - the plugins check follows per set, `claude-plugins` with the set's `env-set`; a set lacking a plugin makes
    `^K` (the existing install action) issue `claude-plugin-install` with that set's `env-set` and no other;
  - only a Copilot worker → no `agent-setup-check` and no `claude-plugins` is ever issued;
  - without workers the existing hooks and plugins cases pass unchanged (tags, problem text).
- [ ] **Step 2:** Build `orchestrator_test`; run `"[hooks]"` → the new cases fail.
- [ ] **Step 3: Implement.** Replace `_hooks` with a map from set to `HooksState`, and `_missingPlugins` with one
  from set to its missing list; check `parsePollTag` does not read `poll/hooks:<set>` as a repository's list.
  Update the view callers only if they need the union/worst semantics (they read `hooks()`/`missingPlugins()`).
- [ ] **Step 4:** Rebuild; the whole `orchestrator_test.exe` → all pass. `python scripts/check-guest-portability.py`
  and `python scripts/check-include-budget.py` → 0. `clang-format -i`.
- [ ] **Step 5: Commit** — the touched files; message
  `feat(orchestrator): check the hooks and plugins once per Claude worker set`.

### Task 10: Copilot runs, and the `Closed` end — Opus

**Files:**
- Modify: `extensions/orchestrator/core/Run.hpp/.cpp` (`Ended::Closed`, its spelling, `advance` on `PaneGone`)
- Modify: `extensions/orchestrator/core/Finished.cpp` (`EndedNames`), `EngineStats.cpp` (no stats for a run that
  reads no Claude state), `EngineTranscript.cpp` (no transcript call for it), `EnginePoll.cpp` or wherever the
  status board is read (as for `status_board: off`), `Reconcile.cpp` if a listed status could reach a silent agent
- Modify: `extensions/orchestrator/contract/agent-runs/README.md` (`closed` in the `ended` row; a Copilot run's
  `session` is the orchestrator's own id)
- Test: `extensions/orchestrator/core/Run_test.cpp`, `Engine_test.cpp`, `Finished_test.cpp`,
  `src/contour/config/DaemonSettings_test.cpp` (Windows only)

**Interfaces:**
- Consumes: `Run::agent`, `agentRowOf` (Tasks 7–8).
- Produces: `Ended::Closed` appended after `WorktreeGone` ("Its tab closed: an agent that reports no status");
  store spelling `closed`; Finished-list word `closed`.

Behaviour: a run whose row is `StatusReporting::Silent`, Running, on `PaneGone` → `toVerify(run,
endingFor(Ended::Closed), …)` whatever `lastStatus` is; a reporting run keeps `Completed`/`NeverReported`.
`Closed` counts as a normal finish wherever `Completed` does (the stats' `hadSession` filter, the Finished list's
counts). A Copilot run never issues `agent-stats` and never `agent-transcript`; asking for its transcript makes a
`TranscriptView` `Failed` at once with `failure == "Copilot runs keep no transcript Contour can read"`. Its record
line is written as any other's (`stats: null`).

- [ ] **Step 1: Write the failing tests.**
  - `Run_test.cpp`: `endedName(Ended::Closed) == "closed"`; a Finished `Closed` run round-trips; a Running Copilot
    run fed `PaneGone` ends with cause `Closed`; a Running Claude run that never reported still ends
    `NeverReported`.
  - `Engine_test.cpp` (`[workers][copilot]`): with `"workers": [ { "id": "copilot", "agent": "copilot" } ]` and
    `fix_ci` on it, a PR dispatch opens `"copilot"` with arguments named exactly `model`, `prompt`; the run stays
    `Running` across wakes; no `agent-stats` is ever issued; a transcript request issues nothing and its view is
    `FetchState::Failed` with the text above; closing its tab ends it `Closed` and its record line holds
    `"ended":"closed"`. `fix_ci` with `"permissions"` on the Copilot worker → one configuration problem line
    naming `tasks[0].permissions`.
  - `Finished_test.cpp`: a `Closed` run's line reads `closed`.
  - `DaemonSettings_test.cpp`, inside the existing `#ifdef _WIN32` block: `programs: { copilot:
    'C:\Users\me\AppData\Roaming\npm\copilot.cmd' }` is kept and warned about (`only an .exe`), without the
    `claude.exe` hint.
- [ ] **Step 2:** Build `orchestrator_test`, then `contour_test`; run the new cases → fail (the Windows config case
  may already pass: it pins today's generic warning).
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Rebuild; whole `orchestrator_test.exe` → pass; `contour_test.exe "[daemon]"` → pass.
  `python scripts/check-guest-portability.py` → 0. `clang-format -i`.
- [ ] **Step 5: Commit** — the touched files; message
  `feat(orchestrator): run Copilot workers, which end Closed when their tab closes`.

### Task 11: The dashboard names a run's worker — Haiku

**Files:**
- Modify: `extensions/orchestrator/view/Sections.cpp` (`runDetails`, ~:355)
- Test: `extensions/orchestrator/view/Sections_test.cpp`

**Interfaces:**
- Consumes: `Config::workers()`, `ImplicitWorkerId` (Task 7); `Run::worker` (Task 8).
- Produces: in `runDetails`, right after the `model` line and only when `lists.config != nullptr &&
  lists.config->workers().size() > 1`: `DetailLine { .label = "worker", .value = clean(run.worker.empty() ?
  std::string { ImplicitWorkerId } : run.worker) }`.

- [ ] **Step 1: Write the failing test** in `Sections_test.cpp` (use its fixtures): with two workers configured, a
  run with `worker = "work"` has a detail line labelled `worker` with value `work`, directly after `model`; a run
  with an empty worker shows `claude`; with one worker (and with none configured) no `worker` line appears.
- [ ] **Step 2:** Build `orchestrator_test`; run the case → fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Rebuild; `orchestrator_test.exe "[view]"` (or the file's tag) and the whole suite → pass.
  `python scripts/check-guest-portability.py` → 0. `clang-format -i`.
- [ ] **Step 5: Commit** — the two files; message `feat(orchestrator): show a run's worker beside its model`.

### Task 12: Docs — the orchestrator README, and `docs/extensions.md` keeping the user's work — Opus

**Files:**
- Modify: `extensions/orchestrator/README.md` (ordinary edit and commit)
- Modify: `docs/extensions.md` (committed version only; the working tree gets the same change merged in)

- [ ] **Step 1: The README.** Add a `## Workers` section after `## Permissions`: what a worker is (`id`, `agent`,
  `env_set`), that `tasks[].worker` picks it (absent: the first; unknown: the first, with a problem line), that
  without `workers` there is one implicit `claude` worker; the two-accounts example from the spec (with
  `C:\Users\me\…` paths) plus a Copilot worker; that a Copilot run shows *running* until its tab closes, ends
  `Closed`, has no transcript, statistics or board, and takes no `permissions`; that a worker's model must be in
  `models`; that each `CLAUDE_CONFIG_DIR` needs its own login and its own `contour agent setup` (the hooks line
  names the worker). Commit it alone: `git add extensions/orchestrator/README.md`, `git diff --cached
  --name-only`, message `docs(orchestrator): workers, two Claude accounts, and Copilot`.
- [ ] **Step 2: `docs/extensions.md` — the procedure** of
  `docs/superpowers/plans/2026-11-24-orchestrator-windows-packaging-and-docs.md`, Task 10, Steps 1–6, unchanged in
  its commands, with these adaptations. `S` is the scratchpad (absolute, Git Bash spelling). Before Step 1 save the
  fingerprints that Step 6 compares: `git diff -- docs/extensions.md | sha256sum > "$S/user-docs.sha"` and
  `git diff -- cmake/ContourWasiCxx.cmake | sha256sum > "$S/user-cmake.sha"`. The user's hunk is at about lines
  262–270; ours all lie past line 340 (Step 3's overlap check proves it). The commit message (Step 4) is
  `docs(extensions): user variable sets, the env-set manifest lines, and orchestrator workers`. Step 6 must print
  all its confirmations, and `git diff --stat -- docs/extensions.md` must show the same counts as before (4+, 2-
  at the time of writing; re-read them in Step 1).
- [ ] **Step 3: What Step 2 writes into `$S/extensions.ours.md`** (Edit tool on the scratch file only):
  1. **Version 2, after the settable-variable table** (~:345–356): a paragraph and table "Variables a user's set
     may hold": the six names with their shapes as Task 1 states them, and that no API key is offered.
  2. **The line table** (~:366–369): rows for `pane-env-set <pane> {<arg>} = <VAR> …` (example `pane-env-set agent
     {env-set} = CLAUDE_CONFIG_DIR NO_PROXY`, "Variables one pane may take from the user's set the call names") and
     `command-env-set <command> {<arg>} = <VAR> …`; one sentence below: the argument is an `arg … = name`, may be
     left out of a call (no set applies), and every variable must be on the user list.
  3. **The daemon world, environment** (~:486): the set's listed variables are applied after the manifest's own;
     an unknown set, or a value that no longer fits its shape, is `bad-argument`; a listed variable the set does
     not hold is inherited as any other.
  4. **Granting** (~:573–600): the dialog lists, per pane and command, the variables it may take from a set
     (`From the variable set the extension names: …`) and each configured set with its values; adding, removing
     or changing a set asks again.
  5. **Configuring the orchestrator** (~:697–740): add `env_sets` to the YAML example (`claude-work: {
     CLAUDE_CONFIG_DIR: 'C:\Users\me\.claude-work' }`) and to the key table ("A map from a set's name to the
     variables a worker using it runs with …; checked when the daemon starts: a variable not on the list, or a value
     of the wrong shape, is logged naming the set and the variable, and the set is kept without it"); in the
     `config` key table add `workers` and, in the `tasks` entry table, `worker`; link the README's Workers section.
  After editing, grep the scratch file for user data (none) and for `C:\Users\` (only `C:\Users\me`).

### Task 13: Deploy and configure — Opus

This task changes the user's machine, not the repository: it commits nothing, and writes real paths only into the
user's local configuration.

**Inputs the controller must obtain from the user before dispatch** (else the task writes the placeholders shown,
in angle brackets, and reports them as still to fill in):
- `worktree_root` — `<WORKTREE_ROOT>`
- `repos` — one or more `{ path: <REPO_PATH>, forge: <github|gitlab>, issues: [...] }`; **do not invent
  repositories**
- `models` — default proposal `[opus, sonnet, haiku]` (the documented default); confirm
- `tasks` — each with `id`, `key`, `label`, `applies_to`, `model`, `prompt` and `worker: claude-work-<n>`;
  `<TASKS>` otherwise

- [ ] **Step 1: Build everything.** `cmake --build --preset clangcl-release` with a 10-minute timeout; on timeout run
  the same command again until it finishes; it must end without an error.
- [ ] **Step 2: Install.** In PowerShell:

```powershell
$prefix = Join-Path $env:LOCALAPPDATA 'Programs\contour-dev'
cmake --install out/build/clangcl-release --prefix $prefix
$bin = 'out/build/clangcl-release/bin'
# cmake --install deploys no Qt: copy every DLL and every directory (Qt plugins, QML) the install lacks.
robocopy $bin (Join-Path $prefix 'bin') *.dll /XC /XN /XO /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) { throw "robocopy (DLLs) failed: $LASTEXITCODE" }
foreach ($dir in Get-ChildItem $bin -Directory) {
    robocopy $dir.FullName (Join-Path $prefix "bin\$($dir.Name)") /E /XC /XN /XO /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) { throw "robocopy ($($dir.Name)) failed: $LASTEXITCODE" }
}
```

  (`/XC /XN /XO`: copy only what the destination lacks. A robocopy exit code below 8 is success.) Check that
  `$prefix\bin\contour.exe` and `$prefix\share\contour\extensions\orchestrator.wasm` exist, and that
  `& "$prefix\bin\contour.exe" --version` prints a version.
- [ ] **Step 3: Find the real paths at run time.** `$userHome = $env:USERPROFILE` (not `$home`, which PowerShell
  reserves); Claude Code's native binary: `Join-Path $userHome '.local\bin\claude.exe'` if it exists, else `(Get-Command claude.exe -ErrorAction Stop).Source`;
  refuse a `.cmd`. Do not create the `.claude-work-<n>` directories (Claude Code creates its config dir on first
  run).
- [ ] **Step 4: Back up and edit `%LOCALAPPDATA%\contour\contour.yml`.** Copy it to
  `contour.yml.bak-<yyyyMMdd-HHmmss>` beside it first. Edit only the `daemon:` section; leave every other line
  byte-identical (keep its line ends). If there is no `daemon:` key, append the block below at the end. If
  `daemon:` exists without `extensions:`, add `extensions:` under it at the section's indentation. If
  `daemon.extensions.orchestrator` already exists, **stop and report** — do not merge into it. The block, paths in
  single quotes:

```yaml
daemon:
    extensions:
        orchestrator:
            autostart: true
            programs:
                claude: '<home>\.local\bin\claude.exe'
            env_sets:
                claude-work-1: { CLAUDE_CONFIG_DIR: '<home>\.claude-work-1' }
                claude-work-2: { CLAUDE_CONFIG_DIR: '<home>\.claude-work-2' }
                claude-work-3: { CLAUDE_CONFIG_DIR: '<home>\.claude-work-3' }
                claude-work-4: { CLAUDE_CONFIG_DIR: '<home>\.claude-work-4' }
            config:
                worktree_root: '<WORKTREE_ROOT>'
                models: [opus, sonnet, haiku]
                workers:
                    - { id: claude-work-1, agent: claude, env_set: claude-work-1 }
                    - { id: claude-work-2, agent: claude, env_set: claude-work-2 }
                    - { id: claude-work-3, agent: claude, env_set: claude-work-3 }
                    - { id: claude-work-4, agent: claude, env_set: claude-work-4 }
                repos:
                    - { path: '<REPO_PATH>', forge: <FORGE> }
                tasks: <TASKS>
```

  `<home>` and the `claude.exe` path are the real ones from Step 3; everything in angle brackets the user's
  answers, or left as written when not given.
- [ ] **Step 5: Verify.** `Compare-Object` (or `git diff --no-index`) of the backup and the new file shows only
  lines inside the `daemon:` section. If Python has PyYAML (`python -c "import yaml"`), `yaml.safe_load` the file
  and check `daemon.extensions.orchestrator.env_sets` has four sets; otherwise say the YAML check was skipped.
  Do not restart the user's daemon: report that the configuration takes effect at its next start (`contour daemon
  stop`, then start it from `$prefix\bin\contour.exe`), that the first start asks for consent showing the four
  sets, and that each `CLAUDE_CONFIG_DIR` needs a `claude` login and `contour agent setup` run with it set.
- [ ] **Step 6: Report** the install prefix, the backup's path, the placeholders still unfilled, and
  `git status --short` showing the repository unchanged by this task.

## Self-review notes

- Spec coverage: settable list and shapes (T1); `env_sets` load/write (T2, shapes at compose per Decision 1);
  manifest lines, corpus, errors (T3); unknown set/omitted set/unlisted variable/re-check at call (T4); consent
  text and re-ask (T5); manifest `arg env-set`, bindings, `copilot` pane (T6); `workers`, `tasks[].worker`, implicit
  worker, unknown worker, permissions on Copilot (T7); run records worker/set, every call passes it, removed set
  (T8); hooks per set with worker-naming line, plugins per set (T9); Copilot runs, `Closed`, `t`, `.exe` warning
  (T10); dashboard worker (T11); README and docs/extensions.md via the index-only procedure (T12).
- Not covered by design: Copilot status and transcripts (spec non-goal); per-repository workers (non-goal).

## Amendment (2026-11-24): the user's tasks, `{key}`, and initialising a worker's Claude

The user asked for four tasks that each start a skill, and for each worker's Claude to be set up with the same
plugins as the main one.

### Task 7b: `{key}` in a task's prompt — Haiku

A skill such as `/example-plugin:ticket` takes a Jira key (`DEV-1234`, `[A-Z]+-\d+`), not a URL, and a task's
prompt knows only `{url}` (and `{topic}`). Add `{key}`: an issue's key (Jira: the key; GitHub/GitLab: the number), a pull or merge
request's number. Files: `extensions/orchestrator/core/Prompt.hpp` (`KeyPlaceholder = "{key}"`, beside
`UrlPlaceholder`), where the prompt is filled, and `Config` (a prompt may use `{key}` wherever it may use `{url}`;
a topic task may not). Tests: a Jira issue fills `DEV-1234`; a GitLab MR fills its number; `{key}` in a topic task
is a configuration problem; a prompt without `{key}` is byte-identical to today's. README: one line beside `{url}`.

### Task 13 additions

- The user's tasks, one row each. No task builds a worker picker in the launcher, so a task runs on the worker it
  names; a user who wants a task on several workers writes one task per worker. The defaults shown:

  | id | key | applies_to | prompt | worker |
  | --- | --- | --- | --- | --- |
  | `work-on-issue` | `w` | `issue` | `/example-plugin:ticket {key}` | `worker-a` |
  | `fix-ci` | `f` | `pr` | `/example-plugin:fix-ci {url}` | `worker-a` |
  | `resolve-conflicts` | `r` | `pr` | `/contour-workflows:rebase` (no skill of the private plugin resolves conflicts) | `worker-a` |
  | `address-review` | `a` | `pr` | `/example-plugin:address-review {url}` | `worker-a` |

  Check each key against the dashboard's reserved keys (`DashboardKeys`) and pick the next free letter if taken.
- `plugins:` lists exactly the main setup's enabled plugins, with their marketplaces:
  `example-plugin@example-tools` (a private marketplace at
  `https://gitlab.example.com/group/example-tools.git`), `contour-workflows@contour-terminal`
  (`contour-terminal/claude-marketplace`), `superpowers@claude-plugins-official` (known already). The
  orchestrator then checks and offers to install them for every Claude worker (Task 9).
- **Initialise each new worker's Claude directory** (`worker-a`, `worker-b`, `worker-c`): with
  `CLAUDE_CONFIG_DIR` set to that directory, run `claude plugin marketplace add` for each non-official marketplace
  above and `claude plugin install` for each plugin, then `contour agent setup` (the status hooks). Read the main
  setup's `enabledPlugins` and `extraKnownMarketplaces` from `<home>\.claude\settings.json` at run time and
  install exactly those that are enabled, so a later change to the main setup is picked up by running the step
  again. Copy no credentials, history or session files. Report what each directory still needs: a `claude`
  login, run once by the user in that directory.
