# Phase 6 — Finish notifications

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When a long command finishes in a pane the user is not looking at, tell them — a desktop notification (`✓ make finished`, `✗ make failed (exit 2)`, `✗ make killed (SIGSEGV)`), the profile's bell, or both — per a profile policy `notify_on_command_finish`, with one replaceable notification per session that is withdrawn on focus or when the next command starts.

**Architecture:** `TerminalSession` overrides `Terminal::Events::commandBlockFinished` (C1), copies the summary and posts to the GUI thread (the `progressChanged` pattern). There it asks `TerminalSessionManager` where the session is (`sessionVisibility`, decided by the pure `platform::sessionVisibilityFor`) and what its tab is called, and hands everything to the pure, header-only `platform::finishNotificationFor` (C7), which sanitises every string (C6). Delivery: Linux through the session's `platform::Notifier` (one identifier per session, so a newer finish replaces the older); elsewhere, and for the bell, through the hosting `WindowController`'s new QML-facing signals — a session in a background tab has no `SessionChrome`, so its own `showNotification`/`onBell` signals reach nobody. A daemon client gets all of this for free: phase 2 raises `commandBlockFinished` only for live transitions, so a re-attach never replays.

**Tech Stack:** C++23, Qt 6 (QtCore/QtGui/QtQuick, `Qt6::Test` for `QSignalSpy`), QML, yaml-cpp, Catch2.

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §8 (and §10.4 for the sanitiser, §12 for daemon parity, §11.1 for the keys). Global constraints: [README](README.md#global-constraints).

---

## Contract deviations and additions (C7)

Report these to the coordinating session before starting; they are deliberate.

1. **`finishNotificationFor` takes a fifth parameter** — the home directory, injected so the pure
   function never reads the environment:
   ```cpp
   [[nodiscard]] std::optional<FinishNotificationRequest> finishNotificationFor(
       vtbackend::CommandBlockSummary const& summary,
       config::FinishNotificationConfig const& policy,
       SessionVisibility visibility,
       std::string_view tabName,
       std::string_view homeDirectory);
   ```
   Only this phase calls it.
2. **The four enums and `FinishNotificationConfig` live in `src/contour/config/FinishNotificationConfig.hpp`**
   (with their `ConfigEnum` token tables), which `Config.hpp` includes — the `WindowShadow.hpp` /
   `TabBarMode.hpp` pattern. Names, namespace (`contour::config`) and shapes are exactly C7's;
   `#include <contour/config/Config.hpp>` still provides them (phase 10's settings rows). The split
   keeps the Qt-free `FinishNotification.hpp` from dragging yaml-cpp and the whole config model into
   `contour_test`.
3. **Additions:** `FinishNotificationConfig` and `FinishNotificationRequest` gain a defaulted
   `operator==`; `FinishNotificationRequest::action` gains the default `NotifyAction::Notify`.
4. **`abbreviateHomePath` moves down** from `contour::window` (TabLabel) to
   `vtbackend::abbreviateHomePath(std::string_view path, std::string_view home) -> std::string`
   in `src/vtbackend/core/WorkingDirectory.hpp`, so the tab tooltip, this phase and phase 9's picker
   (which also shows `~/src/contour`) share one rule. **Phase 9 should consume it rather than write
   its own.**
5. `contour_platform` already links `contour_config` PUBLIC (`src/contour/platform/CMakeLists.txt`),
   so `contour/platform` may include `contour/config` headers — C7's placement stands.
6. **No helper of its own for durations or truncation.** The body's duration is phase 4's
   `vtbackend::formatCommandDuration(std::chrono::steady_clock::duration)` (Task 4.13a: `850ms`, `12s`,
   `3m 12s`, `1h 02m`), and the command name is cut by phase 1's
   `vtbackend::truncateUtf8(std::string_view, size_t maxBytes)` — the ellipsis counts against the budget,
   so `MaxCommandNameBytes` (48) bounds the whole name. Both live in `vtbackend/shell/CommandBlock.hpp`.

New API this phase defines (exact signatures in each task's Interfaces block): the placement
vocabulary and `sessionVisibilityFor` (6.4); `WindowController::isMinimized`, `raiseAndActivate`,
`requestNotification`/`requestBell`/`requestAlert` and their signals (6.5);
`TerminalSessionManager::sessionVisibility`, `tabTitleForSession`, `revealSession`,
`requestWindowNotification`/`requestWindowBell`/`requestWindowAlert` (6.6).

Line numbers below are from `c74b93ad`; phases 1–5 shift them, so every hunk is anchored on its text.

---

### Task 6.1: Move `abbreviateHomePath` into vtbackend's working-directory vocabulary

**Files:**
- Modify: `src/vtbackend/core/WorkingDirectory.hpp` (after `resolveWorkingDirectory`, ~72-75)
- Modify: `src/vtbackend/core/WorkingDirectory.cpp` (end of namespace)
- Modify: `src/vtbackend/core/WorkingDirectory_test.cpp` (append)
- Modify: `src/contour/window/TabLabel.hpp` (remove ~42-53), `src/contour/window/TabLabel.cpp` (remove ~40-55),
  `src/contour/window/TabLabel_test.cpp` (remove line 12 and ~111-149)
- Modify: `src/contour/window/WindowController.cpp` (`tabWorkingDirectory`, ~407-421; includes)

**Interfaces:**
- Consumes: nothing.
- Produces: `[[nodiscard]] std::string vtbackend::abbreviateHomePath(std::string_view path, std::string_view home);`
  (removes `contour::window::abbreviateHomePath` with identical behaviour).

- [ ] **Step 0: Record the phase base**

Run: `git rev-parse HEAD`
Expected: a SHA. Report it to the coordinator as `PHASE6_BASE`; the phase gate diffs against it.

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/core/WorkingDirectory_test.cpp` (the file already has `using namespace vtbackend;`):

```cpp

// {{{ abbreviateHomePath -- how a working directory is shown (tab tooltip, finish notification)

TEST_CASE("abbreviateHomePath.a path under home is abbreviated", "[cwd]")
{
    CHECK(abbreviateHomePath("/home/bob/projects/contour", "/home/bob") == "~/projects/contour");
}

TEST_CASE("abbreviateHomePath.the home directory itself is a bare tilde", "[cwd]")
{
    CHECK(abbreviateHomePath("/home/bob", "/home/bob") == "~");
}

TEST_CASE("abbreviateHomePath.a sibling that merely shares the prefix is untouched", "[cwd]")
{
    // Without a component-boundary check this becomes "~by": a path that reads as real and points
    // somewhere else entirely.
    CHECK(abbreviateHomePath("/home/bobby", "/home/bob") == "/home/bobby");
    CHECK(abbreviateHomePath("/home/bobby/src", "/home/bob") == "/home/bobby/src");
}

TEST_CASE("abbreviateHomePath.a path outside home is untouched", "[cwd]")
{
    CHECK(abbreviateHomePath("/etc/contour", "/home/bob") == "/etc/contour");
    CHECK(abbreviateHomePath("/", "/home/bob") == "/");
}

TEST_CASE("abbreviateHomePath.an unknown home abbreviates nothing", "[cwd]")
{
    // QDir::homePath() can come back empty; abbreviating against "" would turn every path into "~".
    CHECK(abbreviateHomePath("/home/bob/src", "") == "/home/bob/src");
}

TEST_CASE("abbreviateHomePath.an empty path stays empty", "[cwd]")
{
    // Empty means "no working directory known", and every caller drops the line entirely.
    CHECK(abbreviateHomePath("", "/home/bob").empty());
}

// }}}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL to compile — `'abbreviateHomePath': identifier not found` in `WorkingDirectory_test.cpp`.

- [ ] **Step 3: Implement it in vtbackend**

In `src/vtbackend/core/WorkingDirectory.hpp`, add `#include <string_view>` after `#include <string>`, and
after the `resolveWorkingDirectory` declaration (before `} // namespace vtbackend`) add:

```cpp

/// Replaces a leading home directory with `~`, the way a shell prompt writes a path.
///
/// Purely presentational -- the tab tooltip, a finish notification, the recent-commands picker: a caller
/// that wants a path to ACT on wants the real one, which is why this is never done at the source.
///
/// Only a whole path component matches, so a sibling directory whose name merely starts with the home
/// path (`/home/bobby` against a home of `/home/bob`) is left alone rather than mangled into `~by`.
///
/// @param path The absolute path to abbreviate.
/// @param home The user's home directory. An empty value abbreviates nothing.
/// @return @p path with the home prefix replaced by `~`, or @p path unchanged.
[[nodiscard]] std::string abbreviateHomePath(std::string_view path, std::string_view home);
```

In `src/vtbackend/core/WorkingDirectory.cpp`, add `#include <string_view>` below the
`<vtbackend/core/FileUrl.hpp>` include, and before the closing `} // namespace vtbackend` add
(moved verbatim from `TabLabel.cpp`):

```cpp

string abbreviateHomePath(std::string_view path, std::string_view home)
{
    if (home.empty() || !path.starts_with(home))
        return string { path };

    // The home directory itself.
    if (path.size() == home.size())
        return "~";

    // Only a whole component matches: without this, a home of "/home/bob" would turn "/home/bobby" into
    // "~by" -- a path that reads as real and points somewhere else entirely.
    if (path[home.size()] != '/')
        return string { path };

    return "~" + string { path.substr(home.size()) };
}
```

- [ ] **Step 4: Remove the window-layer copy and repoint its caller**

1. `src/contour/window/TabLabel.hpp`: delete the declaration block that starts with
   `/// Replaces a leading home directory with \`~\`, the way a shell prompt writes a path.` and ends with
   `[[nodiscard]] std::string abbreviateHomePath(std::string_view path, std::string_view home);` (and the
   blank line before it).
2. `src/contour/window/TabLabel.cpp`: delete the whole definition
   `std::string abbreviateHomePath(std::string_view path, std::string_view home) { ... }` (and the blank
   line before it).
3. `src/contour/window/TabLabel_test.cpp`: delete the line `using contour::window::abbreviateHomePath;`
   and everything from `// {{{ abbreviateHomePath — the tab hover tooltip's working-directory line`
   through the closing `// }}}` at the end of the file (the six cases now live in vtbackend).
4. `src/contour/window/WindowController.cpp`: add a vtbackend include block after
   `#include <contour/window/WindowController.hpp>`:

   ```cpp

   #include <vtbackend/core/WorkingDirectory.hpp>
   ```

   and in `WindowController::tabWorkingDirectory` replace

   ```cpp
       return QString::fromStdString(
           abbreviateHomePath(session->displayWorkingDirectory(), QDir::homePath().toStdString()));
   ```

   with

   ```cpp
       return QString::fromStdString(
           vtbackend::abbreviateHomePath(session->displayWorkingDirectory(), QDir::homePath().toStdString()));
   ```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test contour_gui_test`
then `out/build/clangcl-debug/bin/vtbackend_test.exe "[cwd]"`
and `out/build/clangcl-debug/bin/contour_gui_test.exe "[tablabel]"`
Expected: both `All tests passed`. (If `contour_gui_test` cannot be built on this machine — README
note on `yaml-cpp/emitter.h` — compile `WindowController.cpp` and `TabLabel_test.cpp` singly via
`ninja -C out/build/clangcl-debug -t commands <obj>` + `cmd /c`, and leave the run to CI.)

- [ ] **Step 6: Format**

Run: `clang-format -i src/vtbackend/core/WorkingDirectory.hpp src/vtbackend/core/WorkingDirectory.cpp src/vtbackend/core/WorkingDirectory_test.cpp src/contour/window/TabLabel.hpp src/contour/window/TabLabel.cpp src/contour/window/TabLabel_test.cpp src/contour/window/WindowController.cpp`
Expected: no output. `git diff --stat` shows only these seven files.

- [ ] **Step 7: Commit**

```bash
git add src/vtbackend/core/WorkingDirectory.hpp src/vtbackend/core/WorkingDirectory.cpp \
        src/vtbackend/core/WorkingDirectory_test.cpp src/contour/window/TabLabel.hpp \
        src/contour/window/TabLabel.cpp src/contour/window/TabLabel_test.cpp \
        src/contour/window/WindowController.cpp
git commit -F - <<'EOF'
vtbackend: move abbreviateHomePath into the working-directory vocabulary

The tab tooltip, finish notifications and the recent-commands picker all
show a working directory as a shell prompt would. One rule, below all
three, instead of a window-layer helper the lower layers cannot reach.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.2: The `notify_on_command_finish` profile key

**Files:**
- Create: `src/contour/config/FinishNotificationConfig.hpp`
- Modify: `src/contour/config/Config.hpp` (includes ~4-9; `TerminalProfile::bell` ~724; reader declarations ~1598;
  `Writer::format(..., Bell const&)` ~1906-1909; formatters after `std::formatter<contour::config::ShadowSize>` ~2584-2591)
- Modify: `src/contour/config/Config.cpp` (`loadProfileBody` ~1143; after the `ShadowSize` loader ~2619-2622)
- Modify: `src/contour/config/ConfigDocumentation.hpp` (after `ScrollbarConfig` ~425-433, after `ScrollbarWeb`
  ~2143-2158, after `using Scrollbar = ...` ~2660)
- Modify: `src/contour/config/CMakeLists.txt` (source list)
- Test: `src/contour/config/Config_test.cpp` (append; built into `contour_gui_test`)

**Interfaces:**
- Consumes: `contour::config::ConfigEnumInfo`, `configEnumValues`, `configEnumFromToken`, `configEnumToken`
  (`ConfigEnum.hpp`); `loadConfigEnum` (`Config.cpp`, anonymous namespace).
- Produces (C7 + C9 row):
  ```cpp
  namespace contour::config {
  enum class NotifyWhen : uint8_t { Never = 0, Unfocused, Hidden, Always };
  enum class NotifyOutcome : uint8_t { Any = 0, Failure };
  enum class NotifyAction : uint8_t { Notify = 0, Bell, NotifyBell };
  enum class NotifyClearOn : uint8_t { Focus = 0, Next, Never };
  struct FinishNotificationConfig {
      NotifyWhen when = NotifyWhen::Unfocused;
      std::chrono::seconds minDuration { 10 };
      NotifyOutcome outcome = NotifyOutcome::Any;
      NotifyAction action = NotifyAction::Notify;
      NotifyClearOn clearOn = NotifyClearOn::Focus;
      bool operator==(FinishNotificationConfig const&) const = default;
  };
  // TerminalProfile:
  ConfigEntry<FinishNotificationConfig, documentation::NotifyOnCommandFinish> notifyOnCommandFinish {};
  }
  ```
  plus `configEnumValues<>` specialisations and `std::formatter<>` specialisations for all four enums.

- [ ] **Step 1: Write the failing tests**

In `src/contour/config/Config_test.cpp` add `<chrono>` after `<algorithm>` if absent (ConfigEnum.hpp is
already included since Task 4.11), then append at the end of the file:

```cpp

// {{{ notify_on_command_finish (semantic blocks, spec §8)

namespace
{

/// Every value of @p Enum has exactly one row, a label, and a token that reads back as that value.
template <typename Enum>
void checkConfigEnumTable(size_t expectedRows)
{
    auto const rows = contour::config::configEnumValues<Enum>();
    CHECK(rows.size() == expectedRows);
    for (auto const& row: rows)
    {
        INFO("token: " << row.token);
        CHECK(contour::config::configEnumFromToken<Enum>(row.token) == row.value);
        CHECK(contour::config::configEnumToken(row.value) == row.token);
        CHECK_FALSE(row.label.empty());
    }
}

} // namespace

TEST_CASE("Config: notify_on_command_finish is on by default for unfocused commands of ten seconds",
          "[config][notification]")
{
    auto const policy = contour::config::TerminalProfile {}.notifyOnCommandFinish.value();
    CHECK(policy.when == contour::config::NotifyWhen::Unfocused);
    CHECK(policy.minDuration == std::chrono::seconds { 10 });
    CHECK(policy.outcome == contour::config::NotifyOutcome::Any);
    CHECK(policy.action == contour::config::NotifyAction::Notify);
    CHECK(policy.clearOn == contour::config::NotifyClearOn::Focus);
}

TEST_CASE("Config: notify_on_command_finish loads every key from a profile", "[config][notification]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        notify_on_command_finish:
            when: hidden
            min_duration: 42
            outcome: failure
            action: notify_bell
            clear_on: next
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const expected = contour::config::FinishNotificationConfig {
        .when = contour::config::NotifyWhen::Hidden,
        .minDuration = std::chrono::seconds { 42 },
        .outcome = contour::config::NotifyOutcome::Failure,
        .action = contour::config::NotifyAction::NotifyBell,
        .clearOn = contour::config::NotifyClearOn::Next,
    };
    CHECK(profile->notifyOnCommandFinish.value() == expected);
}

TEST_CASE("Config: an unknown notify_on_command_finish value keeps the default and is reported",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        notify_on_command_finish:
            when: sometimes
            min_duration: -5
            action: shout
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& policy = profile->notifyOnCommandFinish.value();
    CHECK(policy.when == contour::config::NotifyWhen::Unfocused);
    CHECK(policy.action == contour::config::NotifyAction::Notify);
    // A negative floor reads as "every command", which is what zero already says.
    CHECK(policy.minDuration == std::chrono::seconds { 0 });
    CHECK(capture.text().contains("sometimes"));
    CHECK(capture.text().contains("shout"));
}

TEST_CASE("Config: notify_on_command_finish round-trips through a profile side file", "[config][notification]")
{
    QTemporaryDir dir;
    auto const source = loadFromYaml(dir, "default_profile: main\n");
    auto const* base = source.findProfile("main");
    REQUIRE(base != nullptr);
    auto profile = *base;
    auto const policy = contour::config::FinishNotificationConfig {
        .when = contour::config::NotifyWhen::Always,
        .minDuration = std::chrono::seconds { 3 },
        .outcome = contour::config::NotifyOutcome::Failure,
        .action = contour::config::NotifyAction::Bell,
        .clearOn = contour::config::NotifyClearOn::Never,
    };
    profile.notifyOnCommandFinish = policy;

    writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

    auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
    auto const* roundTripped = reloaded.findProfile("roundtrip");
    REQUIRE(roundTripped != nullptr);
    CHECK(roundTripped->notifyOnCommandFinish.value() == policy);
}

TEST_CASE("Config: the generated config spells out notify_on_command_finish and reads it back",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto const rendered = contour::config::defaultConfigString();
    CHECK(rendered.contains("notify_on_command_finish:"));
    CHECK(rendered.contains("when: unfocused"));
    CHECK(rendered.contains("min_duration: 10"));
    CHECK(rendered.contains("clear_on: focus"));

    auto const defaults = contour::config::Config {};
    auto const reloaded = loadFromYaml(dir, rendered);
    auto const* profile = reloaded.profile(defaults.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    CHECK(profile->notifyOnCommandFinish.value() == contour::config::FinishNotificationConfig {});
}

TEST_CASE("Config: every notify_on_command_finish value has one token that reads back", "[config][notification]")
{
    checkConfigEnumTable<contour::config::NotifyWhen>(4);
    checkConfigEnumTable<contour::config::NotifyOutcome>(2);
    checkConfigEnumTable<contour::config::NotifyAction>(3);
    checkConfigEnumTable<contour::config::NotifyClearOn>(3);
}

// }}}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL to compile — `'FinishNotificationConfig': is not a member of 'contour::config'` and
`'notifyOnCommandFinish': is not a member of 'contour::config::TerminalProfile'`.

- [ ] **Step 3: Create the enums, their tables and the struct**

Create `src/contour/config/FinishNotificationConfig.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnum.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>

namespace contour::config
{

/// Which sessions a finished command is worth telling the user about, by where the session is.
enum class NotifyWhen : uint8_t
{
    Never = 0, ///< Never.
    Unfocused, ///< Unless the session is the focused one of the active window (the default).
    Hidden,    ///< Only while the session is off screen: another tab, a zoomed sibling, a minimised window.
    Always,    ///< Every time.
};

/// Which finished commands are worth telling the user about, by how they ended.
enum class NotifyOutcome : uint8_t
{
    Any = 0, ///< Every command.
    Failure, ///< Only failures: a non-zero exit, a signal, a crash or an interrupt.
};

/// How the user is told.
enum class NotifyAction : uint8_t
{
    Notify = 0, ///< A desktop notification.
    Bell,       ///< The profile's bell sound and window alert.
    NotifyBell, ///< Both.
};

/// When a finish notification still on the desktop is withdrawn.
enum class NotifyClearOn : uint8_t
{
    Focus = 0, ///< When its session gains terminal focus.
    Next,      ///< When its session's next command starts.
    Never,     ///< Never; the desktop retires it.
};

/// The profile's `notify_on_command_finish` policy. @see platform::finishNotificationFor.
///
/// On by default (`unfocused`, ten seconds): the case it serves -- a build in a background tab -- is the
/// one nobody thinks to turn it on for, and the floor keeps ordinary commands quiet. A YAML-facing
/// struct whose members are already enums, so nothing here needs the AGENT.md `bool` carve-out.
struct FinishNotificationConfig
{
    NotifyWhen when = NotifyWhen::Unfocused;      ///< Which sessions notify, by visibility.
    std::chrono::seconds minDuration { 10 };      ///< Commands that ran shorter never notify.
    NotifyOutcome outcome = NotifyOutcome::Any;   ///< Which outcomes notify.
    NotifyAction action = NotifyAction::Notify;   ///< How the user is told.
    NotifyClearOn clearOn = NotifyClearOn::Focus; ///< When a notification is withdrawn.

    bool operator==(FinishNotificationConfig const&) const = default;
};

namespace detail
{
    // inline, so every translation unit shares one table rather than a private copy the returned span
    // would point into. The reader, the writer and (phase 10) the settings page all read these rows.
    inline constexpr auto NotifyWhenTable = std::array {
        ConfigEnumInfo<NotifyWhen> { NotifyWhen::Never, "never", "Never" },
        ConfigEnumInfo<NotifyWhen> { NotifyWhen::Unfocused, "unfocused", "When the pane is not focused" },
        ConfigEnumInfo<NotifyWhen> { NotifyWhen::Hidden, "hidden", "When the pane is not on screen" },
        ConfigEnumInfo<NotifyWhen> { NotifyWhen::Always, "always", "Always" },
    };

    inline constexpr auto NotifyOutcomeTable = std::array {
        ConfigEnumInfo<NotifyOutcome> { NotifyOutcome::Any, "any", "Every command" },
        ConfigEnumInfo<NotifyOutcome> { NotifyOutcome::Failure, "failure", "Only failed commands" },
    };

    inline constexpr auto NotifyActionTable = std::array {
        ConfigEnumInfo<NotifyAction> { NotifyAction::Notify, "notify", "Desktop notification" },
        ConfigEnumInfo<NotifyAction> { NotifyAction::Bell, "bell", "Bell" },
        ConfigEnumInfo<NotifyAction> { NotifyAction::NotifyBell, "notify_bell", "Notification and bell" },
    };

    inline constexpr auto NotifyClearOnTable = std::array {
        ConfigEnumInfo<NotifyClearOn> { NotifyClearOn::Focus, "focus", "When the pane is focused" },
        ConfigEnumInfo<NotifyClearOn> { NotifyClearOn::Next, "next", "When the next command starts" },
        ConfigEnumInfo<NotifyClearOn> { NotifyClearOn::Never, "never", "Never" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<NotifyWhen> const> configEnumValues() noexcept
{
    return detail::NotifyWhenTable;
}

template <>
constexpr std::span<ConfigEnumInfo<NotifyOutcome> const> configEnumValues() noexcept
{
    return detail::NotifyOutcomeTable;
}

template <>
constexpr std::span<ConfigEnumInfo<NotifyAction> const> configEnumValues() noexcept
{
    return detail::NotifyActionTable;
}

template <>
constexpr std::span<ConfigEnumInfo<NotifyClearOn> const> configEnumValues() noexcept
{
    return detail::NotifyClearOnTable;
}

} // namespace contour::config
```

- [ ] **Step 4: Wire it into `Config.hpp`**

1. Includes: after `#include <contour/config/ConfigDocumentation.hpp>` add
   `#include <contour/config/FinishNotificationConfig.hpp>`.
2. `TerminalProfile`: after
   `ConfigEntry<Bell, documentation::Bell> bell { { .sound = "default", .alert = true, .volume = 1.0f } };` add
   ```cpp
       ConfigEntry<FinishNotificationConfig, documentation::NotifyOnCommandFinish> notifyOnCommandFinish {};
   ```
3. `YAMLConfigReader`: after `void loadFromEntry(YAML::Node const& node, std::string const& entry, Bell& where);` add
   ```cpp
       void loadFromEntry(YAML::Node const& node, std::string const& entry, FinishNotificationConfig& where);
       void loadFromEntry(YAML::Node const& node, std::string const& entry, NotifyWhen& where);
       void loadFromEntry(YAML::Node const& node, std::string const& entry, NotifyOutcome& where);
       void loadFromEntry(YAML::Node const& node, std::string const& entry, NotifyAction& where);
       void loadFromEntry(YAML::Node const& node, std::string const& entry, NotifyClearOn& where);
   ```
4. `Writer`: after the `format(std::string_view doc, Bell const& v)` overload add
   ```cpp

       [[nodiscard]] std::string format(std::string_view doc, FinishNotificationConfig const& v)
       {
           return format(doc, v.when, v.minDuration.count(), v.outcome, v.action, v.clearOn);
       }
   ```
5. After the `std::formatter<contour::config::ShadowSize>` specialisation add
   ```cpp

   // The notify_on_command_finish enums render as the token their table row carries, so what the writer
   // emits is by construction what the reader accepts -- as ShadowSize above.
   template <>
   struct std::formatter<contour::config::NotifyWhen>: formatter<std::string_view>
   {
       auto format(contour::config::NotifyWhen value, auto& ctx) const
       {
           return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
       }
   };

   template <>
   struct std::formatter<contour::config::NotifyOutcome>: formatter<std::string_view>
   {
       auto format(contour::config::NotifyOutcome value, auto& ctx) const
       {
           return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
       }
   };

   template <>
   struct std::formatter<contour::config::NotifyAction>: formatter<std::string_view>
   {
       auto format(contour::config::NotifyAction value, auto& ctx) const
       {
           return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
       }
   };

   template <>
   struct std::formatter<contour::config::NotifyClearOn>: formatter<std::string_view>
   {
       auto format(contour::config::NotifyClearOn value, auto& ctx) const
       {
           return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
       }
   };
   ```

- [ ] **Step 5: Read it in `Config.cpp`**

1. `YAMLConfigReader::loadProfileBody`: after `loadFromEntry(child, "bell", where.bell);` add
   `loadFromEntry(child, "notify_on_command_finish", where.notifyOnCommandFinish);`
2. After the `loadFromEntry(..., ShadowSize& where)` definition (it must come after `loadConfigEnum`,
   which is defined in an anonymous namespace further up) add:

```cpp

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     FinishNotificationConfig& where)
{
    auto const child = node[entry];
    if (!child)
        return;

    loadFromEntry(child, "when", where.when);
    // Whole seconds, and never negative: a negative floor would read as "notify for everything", which
    // is what zero already says.
    if (auto const minDuration = child["min_duration"])
        where.minDuration = std::chrono::seconds { std::max(0, minDuration.as<int>()) };
    loadFromEntry(child, "outcome", where.outcome);
    loadFromEntry(child, "action", where.action);
    loadFromEntry(child, "clear_on", where.clearOn);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, NotifyWhen& where)
{
    (void) loadConfigEnum(node, entry, where, logger);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, NotifyOutcome& where)
{
    (void) loadConfigEnum(node, entry, where, logger);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, NotifyAction& where)
{
    (void) loadConfigEnum(node, entry, where, logger);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, NotifyClearOn& where)
{
    (void) loadConfigEnum(node, entry, where, logger);
}
```

- [ ] **Step 6: Document it**

In `src/contour/config/ConfigDocumentation.hpp`, after the `ScrollbarConfig` literal add:

```cpp

constexpr StringLiteral NotifyOnCommandFinishConfig {

    "notify_on_command_finish:\n"
    "    {comment} Tells you when a command finishes. Needs shell integration (OSC 133): a shell\n"
    "    {comment} without it reports no commands, so nothing is ever notified.\n"
    "    {comment} When to notify, by where the pane is:\n"
    "    {comment}   never     - never\n"
    "    {comment}   unfocused - unless it is the focused pane of the active window\n"
    "    {comment}   hidden    - only while it is off screen (another tab, a zoomed sibling,\n"
    "    {comment}               a minimised window)\n"
    "    {comment}   always    - every time\n"
    "    when: {}\n"
    "    {comment} Commands that ran for fewer seconds than this never notify.\n"
    "    min_duration: {}\n"
    "    {comment} Which commands notify: any, or failure (a non-zero exit or a signal).\n"
    "    outcome: {}\n"
    "    {comment} How: notify (a desktop notification), bell (the profile's bell sound and\n"
    "    {comment} window alert), or notify_bell (both).\n"
    "    action: {}\n"
    "    {comment} When a notification still on screen is withdrawn: focus (the pane is\n"
    "    {comment} focused), next (the pane's next command starts), or never.\n"
    "    clear_on: {}\n"
    "\n"

};
```

after the `ScrollbarWeb` literal add:

```cpp

constexpr StringLiteral NotifyOnCommandFinishWeb {
    "configuration tells you when a command you are not watching finishes: a desktop notification, the "
    "bell, or both.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    notify_on_command_finish:\n"
    "      when: unfocused\n"
    "      min_duration: 10\n"
    "      outcome: any\n"
    "      action: notify\n"
    "      clear_on: focus\n"
    "```\n"
    "Commands are known from the OSC 133 marks shell integration emits; a shell without it reports none, "
    "so nothing is notified. A notification reads `✓ make finished`, `✗ make failed (exit 2)` or "
    "`✗ make killed (SIGSEGV)`, with how long it took, where it ran and the tab's name below. A newer "
    "finish in the same pane replaces the older notification, and clicking it brings the pane forward "
    "where the desktop reports clicks.\n"
    ":octicons-horizontal-rule-16: ==when== `never`, `unfocused` (unless it is the focused pane of the "
    "active window), `hidden` (only while the pane is off screen: another tab is active, a sibling pane "
    "is zoomed, or the window is minimised) or `always`. <br/>\n"
    ":octicons-horizontal-rule-16: ==min_duration== Commands that ran for fewer seconds than this never "
    "notify. <br/>\n"
    ":octicons-horizontal-rule-16: ==outcome== `any`, or `failure` for commands that exited non-zero or "
    "died of a signal. <br/>\n"
    ":octicons-horizontal-rule-16: ==action== `notify` (a desktop notification), `bell` (the profile's "
    "bell sound and window alert) or `notify_bell` (both). <br/>\n"
    ":octicons-horizontal-rule-16: ==clear_on== When a notification still on screen is withdrawn: "
    "`focus` (the pane is focused), `next` (the pane's next command starts) or `never`. <br/>\n"
    "\n"
};
```

and after `using Scrollbar = DocumentationEntry<ScrollbarConfig, ScrollbarWeb>;` add:

```cpp
using NotifyOnCommandFinish = DocumentationEntry<NotifyOnCommandFinishConfig, NotifyOnCommandFinishWeb>;
```

(Neither literal may contain a `{` or `}` other than `{comment}` and `{}`: both go through `std::vformat`.)

- [ ] **Step 7: List the header in the target**

In `src/contour/config/CMakeLists.txt`, after `    ConfigEnum.hpp` add `    FinishNotificationConfig.hpp`.

- [ ] **Step 8: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
Expected: `All tests passed` — the six new cases, plus the existing
"generated default config loads back into the defaults" (empty error log) and
"generated default config round-trips through the loader", which now also carry the new key.
(Qt DLL / build-break fallbacks: README "Build and test commands".)

- [ ] **Step 9: Format and audit**

Run: `clang-format -i src/contour/config/FinishNotificationConfig.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp`
Hand-audit (no local clang-tidy for `src/contour/**`): every new local is `const` where it is never
reassigned; no new free function in `Config_test.cpp` outside the anonymous namespace
(`misc-use-internal-linkage`).

- [ ] **Step 10: Commit**

```bash
git add src/contour/config/FinishNotificationConfig.hpp src/contour/config/Config.hpp \
        src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp \
        src/contour/config/CMakeLists.txt src/contour/config/Config_test.cpp
git commit -F - <<'EOF'
config: add the notify_on_command_finish profile key

when / min_duration / outcome / action / clear_on, each enum described
once in a ConfigEnum table the reader, the writer and the settings page
share. On by default: unfocused, ten seconds, a desktop notification,
withdrawn on focus.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.3: The finish-notification decision

**Files:**
- Create: `src/contour/platform/FinishNotification.hpp` (header-only and Qt-free, like `session/SearchStatus.hpp`)
- Create: `src/contour/platform/FinishNotification_test.cpp`
- Modify: `src/contour/platform/CMakeLists.txt` (source list)
- Modify: `src/contour/CMakeLists.txt` (`contour_test` sources ~805-821 and its link comment ~822-823)

**Interfaces:**
- Consumes: C1 `vtbackend::CommandBlockSummary`, `CommandBlockId`, `WorkingDirectorySnapshot`
  (`vtbackend/shell/CommandBlock.hpp`); phase 1 `[[nodiscard]] std::string vtbackend::truncateUtf8(std::string_view text, size_t maxBytes);`
  and phase 4 (Task 4.13a) `[[nodiscard]] std::string vtbackend::formatCommandDuration(std::chrono::steady_clock::duration duration);`
  (both `vtbackend/shell/CommandBlock.hpp`); C6 `vtbackend::sanitizeCommandLine(std::string_view, SanitizePurpose)`
  (`vtbackend/shell/CommandLineSanitizer.hpp`); `vtbackend::ContextOutcome`, `ContextExit`, `ContextSignal`,
  `contextSignalName`, `isKnownContextSignal`, `ContextLocality` (`vtbackend/core/TerminalContext.hpp`); `vtbackend::abbreviateHomePath`
  (Task 6.1); `config::FinishNotificationConfig` and enums (Task 6.2); `core::Flags`, `core::views::joinWith`.
- Produces (namespace `contour::platform`):
  ```cpp
  enum class SessionVisibility : uint8_t { Focused = 0, VisibleUnfocused, Hidden };
  struct FinishNotificationRequest { std::string title; std::string body;
      config::NotifyAction action = config::NotifyAction::Notify;
      bool operator==(FinishNotificationRequest const&) const = default; };
  enum class FinishKind : uint8_t { Unknown = 0, Succeeded, Failed, Killed, Crashed, Interrupted };
  enum class FinishSeverity : uint8_t { Benign = 0, Failure };
  struct FinishWording { FinishKind kind; std::string_view mark; std::string_view verb; FinishSeverity severity; };
  inline constexpr std::array<FinishWording, 6> FinishWordingTable;
  [[nodiscard]] constexpr FinishWording const& wordingOf(FinishKind kind) noexcept;
  inline constexpr auto SignalExitBase = 128;
  inline constexpr auto MaxExitCode = 255;
  [[nodiscard]] constexpr vtbackend::ContextSignal signalOfExitCode(std::optional<int> exitCode) noexcept;
  [[nodiscard]] constexpr FinishKind classifyFinish(std::optional<int> exitCode, vtbackend::ContextOutcome outcome) noexcept;
  [[nodiscard]] inline std::string finishDetail(FinishKind kind, std::optional<int> exitCode, vtbackend::ContextOutcome outcome);
  inline constexpr size_t MaxCommandNameBytes = 48;
  inline constexpr std::string_view FallbackCommandName = "command";
  [[nodiscard]] inline std::string commandShortName(std::string_view commandLine);
  [[nodiscard]] inline std::string finishNotificationTitle(vtbackend::CommandBlockSummary const& summary);
  [[nodiscard]] inline std::string finishNotificationBody(vtbackend::CommandBlockSummary const& summary,
                                                          std::string_view tabName, std::string_view homeDirectory);
  [[nodiscard]] constexpr bool isNoticeWanted(config::NotifyWhen when, SessionVisibility visibility) noexcept;
  [[nodiscard]] constexpr bool isOutcomeWanted(config::NotifyOutcome wanted, FinishSeverity severity) noexcept;
  [[nodiscard]] inline std::optional<FinishNotificationRequest> finishNotificationFor(
      vtbackend::CommandBlockSummary const& summary, config::FinishNotificationConfig const& policy,
      SessionVisibility visibility, std::string_view tabName, std::string_view homeDirectory);
  enum class FinishEffect : uint8_t { Notification = 0b01, Bell = 0b10 };
  using FinishEffects = core::Flags<FinishEffect>;
  [[nodiscard]] constexpr FinishEffects effectsOf(config::NotifyAction action) noexcept;
  inline constexpr std::string_view FinishNotificationIdPrefix = "contour.command-finished.";
  [[nodiscard]] inline std::string finishNotificationIdentifier(int sessionId);
  ```

- [ ] **Step 1: Write the failing tests**

Create `src/contour/platform/FinishNotification_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The finish-notification policy, table by table: when x where x how long x how it ended x how to
// deliver, and every word the desktop is shown. Qt-free on purpose -- the decision lives in a header
// with no Qt in it, which is what lets contour_test (built in both frontend configurations) cover it.

#include <contour/platform/FinishNotification.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

using contour::config::FinishNotificationConfig;
using contour::config::NotifyAction;
using contour::config::NotifyClearOn;
using contour::config::NotifyOutcome;
using contour::config::NotifyWhen;
using contour::platform::commandShortName;
using contour::platform::effectsOf;
using contour::platform::FinishEffect;
using contour::platform::FinishKind;
using contour::platform::finishNotificationBody;
using contour::platform::finishNotificationFor;
using contour::platform::finishNotificationIdentifier;
using contour::platform::finishNotificationTitle;
using contour::platform::SessionVisibility;
using contour::platform::wordingOf;

using namespace std::chrono_literals;

namespace
{

constexpr auto Home = std::string_view { "/home/bob" };

/// A `make -j8` that ran for @p duration in ~/src/contour and ended with @p exitCode and @p outcome.
[[nodiscard]] vtbackend::CommandBlockSummary aFinish(std::optional<int> exitCode,
                                                     vtbackend::ContextOutcome outcome = {},
                                                     std::chrono::steady_clock::duration duration = 192s)
{
    auto summary = vtbackend::CommandBlockSummary {};
    summary.id = vtbackend::CommandBlockId { 7U };
    summary.commandLine = "make -j8";
    summary.exitCode = exitCode;
    summary.outcome = outcome;
    summary.duration = duration;
    summary.workingDirectory = vtbackend::WorkingDirectorySnapshot {
        .path = "/home/bob/src/contour",
        .locality = vtbackend::ContextLocality::Local,
    };
    return summary;
}

/// @p piece, @p count times over.
[[nodiscard]] std::string repeated(std::string_view piece, size_t count)
{
    auto result = std::string {};
    result.reserve(piece.size() * count);
    for ([[maybe_unused]] auto const i: std::views::iota(size_t { 0 }, count))
        result += piece;
    return result;
}

} // namespace

TEST_CASE("FinishNotification.every finish kind has one wording row", "[notification]")
{
    CHECK(contour::platform::FinishWordingTable.size() == 6);
    for (auto const kind: { FinishKind::Unknown,
                            FinishKind::Succeeded,
                            FinishKind::Failed,
                            FinishKind::Killed,
                            FinishKind::Crashed,
                            FinishKind::Interrupted })
        CHECK(wordingOf(kind).kind == kind);
}

TEST_CASE("FinishNotification.titles name the command and how it ended", "[notification]")
{
    using vtbackend::ContextExit;
    using vtbackend::ContextOutcome;
    using vtbackend::ContextSignal;

    struct Row
    {
        std::optional<int> exitCode;
        ContextOutcome outcome;
        std::string_view expected;
    };
    auto const rows = std::array {
        Row { 0, {}, "✓ make finished" },
        Row { 2, {}, "✗ make failed (exit 2)" },
        Row { 139,
              ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv },
              "✗ make killed (SIGSEGV)" },
        Row { 139, ContextOutcome { .exit = ContextExit::Crash }, "✗ make crashed" },
        Row { 139, {}, "✗ make killed (SIGSEGV)" }, // a shell's 128 + n, as the gutter's tooltip reads it
        Row { 130, ContextOutcome { .exit = ContextExit::Interrupt }, "✗ make interrupted" },
        Row { std::nullopt, ContextOutcome { .exit = ContextExit::Failure, .status = 3 }, "✗ make failed (exit 3)" },
        Row { std::nullopt, ContextOutcome { .exit = ContextExit::Success }, "✓ make finished" },
        Row { std::nullopt, {}, "make finished" },
    };
    for (auto const& row: rows)
    {
        INFO(row.expected);
        CHECK(finishNotificationTitle(aFinish(row.exitCode, row.outcome)) == row.expected);
    }
}

TEST_CASE("FinishNotification.the short name is the first word of the command line", "[notification]")
{
    CHECK(commandShortName("make -j8 all") == "make");
    CHECK(commandShortName("   ls -la") == "ls");
    CHECK(commandShortName("/usr/bin/cmake --build .") == "/usr/bin/cmake");
    CHECK(commandShortName("") == "command");
    CHECK(commandShortName("    ") == "command");
}

TEST_CASE("FinishNotification.the body says how long, where and in which tab", "[notification]")
{
    auto summary = aFinish(0);
    CHECK(finishNotificationBody(summary, "build", Home) == "took 3m 12s · ~/src/contour · build");

    SECTION("an unnamed tab leaves its segment out")
    {
        CHECK(finishNotificationBody(summary, "", Home) == "took 3m 12s · ~/src/contour");
    }

    SECTION("an unknown directory leaves its segment out")
    {
        summary.workingDirectory = {};
        CHECK(finishNotificationBody(summary, "build", Home) == "took 3m 12s · build");
    }

    SECTION("a directory on another machine is not shortened against this one's home")
    {
        summary.workingDirectory.locality = vtbackend::ContextLocality::Foreign;
        CHECK(finishNotificationBody(summary, "build", Home) == "took 3m 12s · /home/bob/src/contour · build");
    }

    SECTION("an unknown home shortens nothing")
    {
        CHECK(finishNotificationBody(summary, "build", "") == "took 3m 12s · /home/bob/src/contour · build");
    }
}

TEST_CASE("FinishNotification.the decision table", "[notification]")
{
    // when x visibility, spelled out rather than re-derived, so this does not restate the rule it
    // checks. Rows: never, unfocused, hidden, always. Columns: Focused, VisibleUnfocused, Hidden.
    constexpr auto WantedByVisibility = std::array {
        std::array { false, false, false },
        std::array { false, true, true },
        std::array { false, false, true },
        std::array { true, true, true },
    };

    enum class Ending : uint8_t
    {
        Success = 0,
        Failure,
        UnknownExit,
        Signal,
    };

    auto const when = GENERATE(NotifyWhen::Never, NotifyWhen::Unfocused, NotifyWhen::Hidden, NotifyWhen::Always);
    auto const visibility =
        GENERATE(SessionVisibility::Focused, SessionVisibility::VisibleUnfocused, SessionVisibility::Hidden);
    auto const duration = GENERATE(9s, 10s, 11s); // below, at and above the 10 s floor
    auto const ending = GENERATE(Ending::Success, Ending::Failure, Ending::UnknownExit, Ending::Signal);
    auto const wanted = GENERATE(NotifyOutcome::Any, NotifyOutcome::Failure);
    auto const action = GENERATE(NotifyAction::Notify, NotifyAction::Bell, NotifyAction::NotifyBell);

    auto summary = aFinish(std::nullopt, {}, duration);
    switch (ending)
    {
        case Ending::Success: summary.exitCode = 0; break;
        case Ending::Failure: summary.exitCode = 2; break;
        case Ending::UnknownExit: break;
        case Ending::Signal:
            summary.exitCode = 137;
            summary.outcome = vtbackend::ContextOutcome { .exit = vtbackend::ContextExit::Crash,
                                                          .signal = vtbackend::ContextSignal::Kill };
            break;
    }

    auto const policy = FinishNotificationConfig {
        .when = when, .minDuration = 10s, .outcome = wanted, .action = action, .clearOn = NotifyClearOn::Focus
    };
    auto const result = finishNotificationFor(summary, policy, visibility, "build", Home);

    auto const isFailure = ending == Ending::Failure || ending == Ending::Signal;
    auto const expected = WantedByVisibility.at(static_cast<size_t>(when)).at(static_cast<size_t>(visibility))
                          && duration >= 10s && (wanted == NotifyOutcome::Any || isFailure);

    INFO(std::format("when={} visibility={} duration={}s ending={} outcome={} action={}",
                     static_cast<int>(when),
                     static_cast<int>(visibility),
                     duration.count(),
                     static_cast<int>(ending),
                     static_cast<int>(wanted),
                     static_cast<int>(action)));
    REQUIRE(result.has_value() == expected);
    if (result.has_value())
        CHECK(result->action == action);
}

TEST_CASE("FinishNotification.a hostile command line reaches the desktop only as text", "[notification]")
{
    // Review focus 3: the command line, the directory and the tab name all come from whatever wrote to
    // the terminal.
    auto summary = aFinish(0);

    SECTION("an embedded bracketed-paste end and a newline")
    {
        summary.commandLine = "\x1b[201~rm -rf /\nmake";
        auto const title = finishNotificationTitle(summary);
        CHECK_FALSE(title.contains('\x1b'));
        CHECK_FALSE(title.contains('\n'));
    }

    SECTION("a bidi override")
    {
        summary.commandLine = "\xE2\x80\xAE" "ekam"; // U+202E RIGHT-TO-LEFT OVERRIDE, then text
        CHECK_FALSE(finishNotificationTitle(summary).contains("\xE2\x80\xAE"));
    }

    SECTION("a 64 KiB word is cut to the name budget")
    {
        // The three-byte "…" counts against MaxCommandNameBytes (vtbackend::truncateUtf8): 45 + 3 = 48.
        summary.commandLine = std::string(65536, 'a');
        CHECK(finishNotificationTitle(summary) == "✓ " + std::string(45, 'a') + "… finished");
    }

    SECTION("a cut never splits a character")
    {
        // "x" then 30 three-byte euro signs: the budget leaves 45 bytes before the ellipsis, and byte 45 is
        // inside the 15th euro, so the cut backs off to 43 -- "x" and 14 whole euros.
        summary.commandLine = "x" + repeated("€", 30);
        CHECK(finishNotificationTitle(summary) == "✓ x" + repeated("€", 14) + "… finished");
    }

    SECTION("a hostile directory and tab name are sanitised too")
    {
        summary.workingDirectory.path = "/tmp/\x1b]0;pwned\x07";
        auto const body = finishNotificationBody(summary, "tab\x1b[31m", Home);
        CHECK_FALSE(body.contains('\x1b'));
        CHECK_FALSE(body.contains('\x07'));
    }
}

TEST_CASE("FinishNotification.each action names its effects", "[notification]")
{
    CHECK(effectsOf(NotifyAction::Notify).test(FinishEffect::Notification));
    CHECK_FALSE(effectsOf(NotifyAction::Notify).test(FinishEffect::Bell));
    CHECK(effectsOf(NotifyAction::Bell).test(FinishEffect::Bell));
    CHECK_FALSE(effectsOf(NotifyAction::Bell).test(FinishEffect::Notification));
    CHECK(effectsOf(NotifyAction::NotifyBell).test(FinishEffect::Notification));
    CHECK(effectsOf(NotifyAction::NotifyBell).test(FinishEffect::Bell));
}

TEST_CASE("FinishNotification.one identifier per session", "[notification]")
{
    CHECK(finishNotificationIdentifier(3) == "contour.command-finished.3");
    CHECK(finishNotificationIdentifier(3) != finishNotificationIdentifier(4));
}
```

Then register it in `src/contour/CMakeLists.txt`: in `add_executable(contour_test ...)`, after
`        session/SearchStatus_test.cpp` add `        platform/FinishNotification_test.cpp`, and replace the two
comment lines beginning `# vtbackend, deliberately: SearchStatus` (keep phase 3's `# vthost for resolveExecutablePath()`
line directly above target_link_libraries(contour_test ...)) with:

```cmake
    # vtbackend, deliberately: SearchStatus and FinishNotification are pure decisions over vtbackend
    # types, and testing them HERE rather than in contour_gui_test is what proves they need no Qt.
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL to compile — `Cannot open include file: 'contour/platform/FinishNotification.hpp'`.

- [ ] **Step 3: Implement the decision**

Create `src/contour/platform/FinishNotification.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/FinishNotificationConfig.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <core/Flags.hpp>
#include <core/Utils.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The finish-notification decision (spec §8): whether a finished command is worth telling the user
/// about, and in which words. Header-only and Qt-free, so it is table-tested in contour_test; the
/// session supplies the facts and does the delivery.
namespace contour::platform
{

/// Where a session sits relative to what the user is looking at.
enum class SessionVisibility : uint8_t
{
    Focused = 0,      ///< The session holding terminal focus in the active window.
    VisibleUnfocused, ///< On screen -- its tab is shown, nothing covers its pane -- but focus is elsewhere.
    Hidden,           ///< Off screen: another tab is active, a sibling is zoomed, or the window is minimised.
};

/// What a finish notification says, and how it is delivered.
struct FinishNotificationRequest
{
    std::string title;                                          ///< e.g. `✗ make failed (exit 2)`.
    std::string body;                                           ///< e.g. `took 3m 12s · ~/src/contour · build`.
    config::NotifyAction action = config::NotifyAction::Notify; ///< How to deliver it.

    bool operator==(FinishNotificationRequest const&) const = default;
};

// {{{ How a command ended

/// How a command ended, as far as its record can tell.
enum class FinishKind : uint8_t
{
    Unknown = 0, ///< Neither an exit code nor an outcome was reported.
    Succeeded,   ///< Exit code 0, or OSC 3008 `exit=success`.
    Failed,      ///< A non-zero exit code, or OSC 3008 `exit=failure`.
    Killed,      ///< OSC 3008 named the signal it died of, or a shell's 128 + n exit code names one.
    Crashed,     ///< OSC 3008 `exit=crash` without a signal name.
    Interrupted, ///< OSC 3008 `exit=interrupt`.
};

/// Whether a finish counts for `outcome: failure`.
enum class FinishSeverity : uint8_t
{
    Benign = 0, ///< Not a failure: `outcome: failure` stays silent about it.
    Failure,    ///< A failure: `outcome: failure` announces it.
};

/// How one FinishKind is worded in a title, and whether it is a failure.
struct FinishWording
{
    FinishKind kind;         ///< The finish this row words.
    std::string_view mark;   ///< Leading glyph and space, or empty when nothing is known.
    std::string_view verb;   ///< What the title says the command did: `finished`, `failed`, ...
    FinishSeverity severity; ///< Whether `outcome: failure` counts this finish.
};

/// One row per FinishKind: a seventh kind is a row here, not a branch in three functions.
inline constexpr auto FinishWordingTable = std::array {
    FinishWording { FinishKind::Unknown, "", "finished", FinishSeverity::Benign },
    FinishWording { FinishKind::Succeeded, "✓ ", "finished", FinishSeverity::Benign },
    FinishWording { FinishKind::Failed, "✗ ", "failed", FinishSeverity::Failure },
    FinishWording { FinishKind::Killed, "✗ ", "killed", FinishSeverity::Failure },
    FinishWording { FinishKind::Crashed, "✗ ", "crashed", FinishSeverity::Failure },
    FinishWording { FinishKind::Interrupted, "✗ ", "interrupted", FinishSeverity::Failure },
};

/// The wording row of @p kind.
/// @param kind The finish to word.
/// @return Its row of FinishWordingTable (the Unknown row for a kind the table lacks).
[[nodiscard]] constexpr FinishWording const& wordingOf(FinishKind kind) noexcept
{
    for (auto const& row: FinishWordingTable)
        if (row.kind == kind)
            return row;
    return FinishWordingTable.front();
}

/// What a POSIX shell adds to a signal's number to report the command it ended: 128 + n.
inline constexpr auto SignalExitBase = 128;

/// The largest exit code a shell reports.
inline constexpr auto MaxExitCode = 255;

/// The signal a shell's 128 + n exit code names -- how every POSIX shell spells a signal death, though a
/// program may exit with such a code itself. The gutter's tooltip reads exit codes the same way (Task 4.13),
/// so the two never disagree about one record.
/// @param exitCode The reported exit code, if any.
/// @return The signal it names, or ContextSignal::None when it names no signal this terminal knows.
[[nodiscard]] constexpr vtbackend::ContextSignal signalOfExitCode(std::optional<int> exitCode) noexcept
{
    if (!exitCode || *exitCode <= SignalExitBase || *exitCode > MaxExitCode)
        return vtbackend::ContextSignal::None;
    auto const number = static_cast<uint8_t>(*exitCode - SignalExitBase);
    if (!vtbackend::isKnownContextSignal(number))
        return vtbackend::ContextSignal::None;
    return static_cast<vtbackend::ContextSignal>(number);
}

/// Classifies a finish. A signal name wins, then a crash or an interrupt (which only OSC 3008 can tell
/// apart from an ordinary non-zero exit), then a shell's 128 + n exit code naming a known signal, then
/// the exit code, then OSC 3008's own verdict.
/// @param exitCode The reported exit code, if any.
/// @param outcome  What OSC 3008 said about the end, if anything.
/// @return The kind of finish.
[[nodiscard]] constexpr FinishKind classifyFinish(std::optional<int> exitCode,
                                                  vtbackend::ContextOutcome outcome) noexcept
{
    if (outcome.signal != vtbackend::ContextSignal::None)
        return FinishKind::Killed;
    if (outcome.exit == vtbackend::ContextExit::Crash)
        return FinishKind::Crashed;
    if (outcome.exit == vtbackend::ContextExit::Interrupt)
        return FinishKind::Interrupted;
    if (signalOfExitCode(exitCode) != vtbackend::ContextSignal::None)
        return FinishKind::Killed;
    if (exitCode.has_value())
        return *exitCode == 0 ? FinishKind::Succeeded : FinishKind::Failed;
    if (outcome.exit == vtbackend::ContextExit::Success)
        return FinishKind::Succeeded;
    if (outcome.exit == vtbackend::ContextExit::Failure)
        return FinishKind::Failed;
    return FinishKind::Unknown;
}

/// The parenthesised detail of a title: `exit 2`, `SIGSEGV`, or nothing.
/// @param kind     The finish, as classifyFinish() decided it.
/// @param exitCode The reported exit code, if any.
/// @param outcome  What OSC 3008 said about the end, if anything.
/// @return The detail, or an empty string when the title carries none.
[[nodiscard]] inline std::string finishDetail(FinishKind kind,
                                              std::optional<int> exitCode,
                                              vtbackend::ContextOutcome outcome)
{
    switch (kind)
    {
        case FinishKind::Failed:
            return std::format("exit {}", exitCode.value_or(static_cast<int>(outcome.status & 0b1111'1111U)));
        case FinishKind::Killed:
            // OSC 3008's own signal name when it gave one, else the one a shell's 128 + n exit code names.
            return std::string { vtbackend::contextSignalName(outcome.signal != vtbackend::ContextSignal::None
                                                                  ? outcome.signal
                                                                  : signalOfExitCode(exitCode)) };
        case FinishKind::Unknown:
        case FinishKind::Succeeded:
        case FinishKind::Crashed:
        case FinishKind::Interrupted: break;
    }
    return {};
}

// }}}

// {{{ The words

/// The longest command name a title carries, in bytes, the `…` of a cut name included; longer ones are
/// cut at a character boundary (@see vtbackend::truncateUtf8).
inline constexpr size_t MaxCommandNameBytes = 48;

/// What a title calls a command whose line is unknown.
inline constexpr std::string_view FallbackCommandName = "command";

/// The first word of the command line, sanitised for display -- what a title calls the command.
/// @param commandLine The block's command line; untrusted (it came from whatever wrote to the terminal).
/// @return Its first word, cut to MaxCommandNameBytes, or FallbackCommandName when the line is blank.
[[nodiscard]] inline std::string commandShortName(std::string_view commandLine)
{
    auto const display = vtbackend::sanitizeCommandLine(commandLine, vtbackend::SanitizePurpose::Display);
    auto const text = std::string_view { display };
    auto const begin = text.find_first_not_of(' ');
    if (begin == std::string_view::npos)
        return std::string { FallbackCommandName };
    auto const end = text.find(' ', begin);
    return vtbackend::truncateUtf8(
        text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin),
        MaxCommandNameBytes);
}

/// `✓ make finished`, `✗ make failed (exit 2)`, `✗ make killed (SIGSEGV)`, ...
/// @param summary The finished block.
/// @return The title: mark, command name, verb and, when there is one, the parenthesised detail.
[[nodiscard]] inline std::string finishNotificationTitle(vtbackend::CommandBlockSummary const& summary)
{
    auto const kind = classifyFinish(summary.exitCode, summary.outcome);
    auto const& wording = wordingOf(kind);
    auto const detail = finishDetail(kind, summary.exitCode, summary.outcome);
    auto title = std::format("{}{} {}", wording.mark, commandShortName(summary.commandLine), wording.verb);
    if (!detail.empty())
        title += std::format(" ({})", detail);
    return title;
}

/// `took 3m 12s · ~/src/contour · build`; an unknown directory or an unnamed tab leaves its segment out.
/// The duration reads as everywhere else a command's duration is shown (@see vtbackend::formatCommandDuration).
/// @param summary       The finished block.
/// @param tabName       The tab strip's label for the session's tab; untrusted (it may be a window title).
/// @param homeDirectory The local home, for `~`; a directory on another machine is never shortened.
[[nodiscard]] inline std::string finishNotificationBody(vtbackend::CommandBlockSummary const& summary,
                                                        std::string_view tabName,
                                                        std::string_view homeDirectory)
{
    auto segments = std::vector<std::string> { "took " + vtbackend::formatCommandDuration(summary.duration) };

    if (auto const& directory = summary.workingDirectory; !directory.path.empty())
    {
        auto const shown = directory.locality == vtbackend::ContextLocality::Foreign
                               ? directory.path
                               : vtbackend::abbreviateHomePath(directory.path, homeDirectory);
        segments.push_back(vtbackend::sanitizeCommandLine(shown, vtbackend::SanitizePurpose::Display));
    }

    if (!tabName.empty())
        segments.push_back(vtbackend::sanitizeCommandLine(tabName, vtbackend::SanitizePurpose::Display));

    return segments | core::views::joinWith(std::string_view { " · " });
}

// }}}

// {{{ The decision

/// Whether @p when asks for a notice from a session that is @p visibility.
/// @param when       The profile's `notify_on_command_finish.when`.
/// @param visibility Where the session is right now.
/// @return True when a notice is wanted.
[[nodiscard]] constexpr bool isNoticeWanted(config::NotifyWhen when, SessionVisibility visibility) noexcept
{
    switch (when)
    {
        case config::NotifyWhen::Never: return false;
        case config::NotifyWhen::Unfocused: return visibility != SessionVisibility::Focused;
        case config::NotifyWhen::Hidden: return visibility == SessionVisibility::Hidden;
        case config::NotifyWhen::Always: return true;
    }
    return false;
}

/// Whether @p wanted asks for a notice about a finish of @p severity.
/// @param wanted   The profile's `notify_on_command_finish.outcome`.
/// @param severity How bad the finish was (@see wordingOf).
/// @return True when a notice is wanted.
[[nodiscard]] constexpr bool isOutcomeWanted(config::NotifyOutcome wanted, FinishSeverity severity) noexcept
{
    switch (wanted)
    {
        case config::NotifyOutcome::Any: return true;
        case config::NotifyOutcome::Failure: return severity == FinishSeverity::Failure;
    }
    return true;
}

/// Decides whether a finished command is worth telling the user about, and what to say.
///
/// Pure: everything it reads is a parameter. The command line, the directory and the tab name all
/// arrive from whatever wrote to the terminal, so each is sanitised for display (C6) before it can
/// reach the desktop.
///
/// @param summary       The finished block (a value copy made on the parser thread).
/// @param policy        The profile's `notify_on_command_finish`.
/// @param visibility    Where the block's session is right now. @see sessionVisibilityFor
/// @param tabName       The tab strip's label for the session's tab; empty leaves it out.
/// @param homeDirectory The user's home, for writing the directory as `~/...`; empty shortens nothing.
/// @return The notification to deliver, or nullopt when the policy says nothing should be.
[[nodiscard]] inline std::optional<FinishNotificationRequest> finishNotificationFor(
    vtbackend::CommandBlockSummary const& summary,
    config::FinishNotificationConfig const& policy,
    SessionVisibility visibility,
    std::string_view tabName,
    std::string_view homeDirectory)
{
    if (!isNoticeWanted(policy.when, visibility))
        return std::nullopt;
    if (summary.duration < policy.minDuration)
        return std::nullopt;
    if (!isOutcomeWanted(policy.outcome, wordingOf(classifyFinish(summary.exitCode, summary.outcome)).severity))
        return std::nullopt;

    return FinishNotificationRequest { .title = finishNotificationTitle(summary),
                                       .body = finishNotificationBody(summary, tabName, homeDirectory),
                                       .action = policy.action };
}

// }}}

// {{{ Delivery

/// What delivering a finish notice does.
enum class FinishEffect : uint8_t
{
    Notification = 0b01, ///< A desktop notification (Linux) or tray message (elsewhere).
    Bell = 0b10,         ///< The profile's bell sound, and its window alert when configured.
};

/// A set of FinishEffect.
using FinishEffects = core::Flags<FinishEffect>;

/// The effects @p action asks for. A switch, so a fourth action is a compile error here.
/// @param action The profile's `notify_on_command_finish.action`.
/// @return The effects delivering the notice has.
[[nodiscard]] constexpr FinishEffects effectsOf(config::NotifyAction action) noexcept
{
    switch (action)
    {
        case config::NotifyAction::Notify: return FinishEffects { FinishEffect::Notification };
        case config::NotifyAction::Bell: return FinishEffects { FinishEffect::Bell };
        case config::NotifyAction::NotifyBell:
            return FinishEffects { FinishEffect::Notification, FinishEffect::Bell };
    }
    return FinishEffects { FinishEffect::Notification };
}

/// The prefix of every finish notification's identifier: unlike anything an OSC 99 application is
/// likely to choose, since the identifier space is the session notifier's.
inline constexpr std::string_view FinishNotificationIdPrefix = "contour.command-finished.";

/// The one identifier session @p sessionId's finish notifications carry, so a newer one replaces the
/// older on the desktop instead of stacking under it.
/// @param sessionId The session's id.
/// @return FinishNotificationIdPrefix followed by @p sessionId.
[[nodiscard]] inline std::string finishNotificationIdentifier(int sessionId)
{
    return std::format("{}{}", FinishNotificationIdPrefix, sessionId);
}

// }}}

} // namespace contour::platform
```

In `src/contour/platform/CMakeLists.txt`, after `    ExternalLauncher.cpp ExternalLauncher.hpp` add
`    FinishNotification.hpp`.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target contour_test`
then `out/build/clangcl-debug/bin/contour_test.exe "[notification]"`
Expected: `All tests passed` (the decision table alone expands to 864 generated runs).

- [ ] **Step 5: Format and audit**

Run: `clang-format -i src/contour/platform/FinishNotification.hpp src/contour/platform/FinishNotification_test.cpp`
Hand-audit: every header function is `inline` or `constexpr`; test helpers sit in the anonymous
namespace (`misc-use-internal-linkage`); no C-style loop; no `min`/`max`/`small` identifiers; no
truncation or duration helper of this header's own — `git grep -n "truncateUtf8\|formatCommandDuration" src/contour/platform/FinishNotification.hpp`
shows only `vtbackend::`-qualified uses (two calls, two `@see`).

- [ ] **Step 6: Commit**

```bash
git add src/contour/platform/FinishNotification.hpp src/contour/platform/FinishNotification_test.cpp \
        src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git commit -F - <<'EOF'
platform: decide whether and how a finished command is announced

finishNotificationFor() turns a CommandBlockSummary, the profile policy,
the session's visibility, its tab name and the home directory into a
title ("✗ make failed (exit 2)"), a body ("took 3m 12s · ~/src/contour ·
build") and an action. Pure and Qt-free, table-tested in contour_test;
every string from the terminal is sanitised before it can reach the
desktop.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.4: The session-visibility decision

**Files:**
- Modify: `src/contour/platform/FinishNotification.hpp` (after `SessionVisibility`)
- Test: `src/contour/platform/FinishNotification_test.cpp` (append)

**Interfaces:**
- Consumes: `SessionVisibility` (Task 6.3).
- Produces (namespace `contour::platform`):
  ```cpp
  enum class SessionFocus : uint8_t { Elsewhere = 0, Here };
  enum class TabSelection : uint8_t { Background = 0, Active };
  enum class PaneExposure : uint8_t { Shown = 0, Covered };
  enum class WindowPresence : uint8_t { Shown = 0, Minimized };
  struct SessionPlacement {
      SessionFocus focus = SessionFocus::Elsewhere;
      TabSelection tab = TabSelection::Background;
      PaneExposure pane = PaneExposure::Shown;
      WindowPresence window = WindowPresence::Shown;
      bool operator==(SessionPlacement const&) const = default;
  };
  [[nodiscard]] constexpr SessionVisibility sessionVisibilityFor(SessionPlacement placement) noexcept;
  ```

- [ ] **Step 1: Write the failing test**

Append to `src/contour/platform/FinishNotification_test.cpp`:

```cpp

TEST_CASE("SessionVisibility.the decision table", "[notification]")
{
    using contour::platform::PaneExposure;
    using contour::platform::SessionFocus;
    using contour::platform::SessionPlacement;
    using contour::platform::sessionVisibilityFor;
    using contour::platform::TabSelection;
    using contour::platform::WindowPresence;

    constexpr auto Here = SessionFocus::Here;
    constexpr auto Elsewhere = SessionFocus::Elsewhere;
    constexpr auto Active = TabSelection::Active;
    constexpr auto Background = TabSelection::Background;
    constexpr auto PaneShown = PaneExposure::Shown;
    constexpr auto Covered = PaneExposure::Covered;
    constexpr auto WindowShown = WindowPresence::Shown;
    constexpr auto Minimized = WindowPresence::Minimized;
    constexpr auto Focused = SessionVisibility::Focused;
    constexpr auto Visible = SessionVisibility::VisibleUnfocused;
    constexpr auto Hidden = SessionVisibility::Hidden;

    struct Row
    {
        SessionPlacement placement;
        SessionVisibility expected;
    };

    // All sixteen placements, spelled out. A minimised window hides everything in it; otherwise focus
    // decides; without focus, only the shown tab's uncovered panes are on screen.
    auto const rows = std::array {
        Row { { Here, Active, PaneShown, WindowShown }, Focused },
        Row { { Here, Active, PaneShown, Minimized }, Hidden },
        Row { { Here, Active, Covered, WindowShown }, Focused },
        Row { { Here, Active, Covered, Minimized }, Hidden },
        Row { { Here, Background, PaneShown, WindowShown }, Focused },
        Row { { Here, Background, PaneShown, Minimized }, Hidden },
        Row { { Here, Background, Covered, WindowShown }, Focused },
        Row { { Here, Background, Covered, Minimized }, Hidden },
        Row { { Elsewhere, Active, PaneShown, WindowShown }, Visible },
        Row { { Elsewhere, Active, PaneShown, Minimized }, Hidden },
        Row { { Elsewhere, Active, Covered, WindowShown }, Hidden },
        Row { { Elsewhere, Active, Covered, Minimized }, Hidden },
        Row { { Elsewhere, Background, PaneShown, WindowShown }, Hidden },
        Row { { Elsewhere, Background, PaneShown, Minimized }, Hidden },
        Row { { Elsewhere, Background, Covered, WindowShown }, Hidden },
        Row { { Elsewhere, Background, Covered, Minimized }, Hidden },
    };

    for (auto const& row: rows)
    {
        INFO(std::format("focus={} tab={} pane={} window={}",
                         static_cast<int>(row.placement.focus),
                         static_cast<int>(row.placement.tab),
                         static_cast<int>(row.placement.pane),
                         static_cast<int>(row.placement.window)));
        CHECK(sessionVisibilityFor(row.placement) == row.expected);
    }

    // The default placement -- a session nothing places -- is off screen, and decided at compile time.
    static_assert(sessionVisibilityFor(SessionPlacement {}) == SessionVisibility::Hidden);
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL to compile — `'PaneExposure': is not a member of 'contour::platform'`.

- [ ] **Step 3: Implement it**

In `src/contour/platform/FinishNotification.hpp`, directly after the `SessionVisibility` enum add:

```cpp

/// Whether a session holds the application's single terminal focus.
enum class SessionFocus : uint8_t
{
    Elsewhere = 0, ///< Another session, or no session at all, holds the focus.
    Here,          ///< This session holds the focus.
};

/// Whether the tab hosting a session is the one its window shows.
enum class TabSelection : uint8_t
{
    Background = 0, ///< Another tab of the window is shown.
    Active,         ///< The session's tab is the window's shown tab.
};

/// Whether a session's pane is drawn within its tab. A zoomed sibling covers every other pane.
enum class PaneExposure : uint8_t
{
    Shown = 0, ///< The pane is drawn.
    Covered,   ///< A zoomed sibling fills the tab instead.
};

/// Whether the window hosting a session is on screen at all.
enum class WindowPresence : uint8_t
{
    Shown = 0, ///< The window is not minimised.
    Minimized, ///< The window is minimised (as far as the platform reports it).
};

/// The four facts SessionVisibility is decided from, as the session manager gathers them.
struct SessionPlacement
{
    SessionFocus focus = SessionFocus::Elsewhere;  ///< Whether the session holds terminal focus.
    TabSelection tab = TabSelection::Background;   ///< Whether its tab is the window's shown one.
    PaneExposure pane = PaneExposure::Shown;       ///< Whether a zoomed sibling covers its pane.
    WindowPresence window = WindowPresence::Shown; ///< Whether its window is minimised.

    bool operator==(SessionPlacement const&) const = default;
};

/// Decides where a session is from how it is placed.
///
/// A minimised window hides everything in it, even the pane holding focus (a window manager that
/// minimises without deactivating must not make a build in it count as watched). Otherwise focus
/// decides; and an unfocused pane is on screen only while its tab is the shown one and no zoomed
/// sibling covers it.
/// @param placement The facts, gathered by TerminalSessionManager::sessionVisibility.
/// @return Where the session is.
[[nodiscard]] constexpr SessionVisibility sessionVisibilityFor(SessionPlacement placement) noexcept
{
    if (placement.window == WindowPresence::Minimized)
        return SessionVisibility::Hidden;
    if (placement.focus == SessionFocus::Here)
        return SessionVisibility::Focused;
    if (placement.tab == TabSelection::Active && placement.pane == PaneExposure::Shown)
        return SessionVisibility::VisibleUnfocused;
    return SessionVisibility::Hidden;
}
```

- [ ] **Step 4: Run it to see it pass**

Run: `cmake --build --preset clangcl-debug --target contour_test`
then `out/build/clangcl-debug/bin/contour_test.exe "[notification]"`
Expected: `All tests passed`.

- [ ] **Step 5: Format**

Run: `clang-format -i src/contour/platform/FinishNotification.hpp src/contour/platform/FinishNotification_test.cpp`
Expected: no output.

- [ ] **Step 6: Commit**

```bash
git add src/contour/platform/FinishNotification.hpp src/contour/platform/FinishNotification_test.cpp
git commit -F - <<'EOF'
platform: decide a session's visibility from its placement

Focused / VisibleUnfocused / Hidden from four facts -- terminal focus,
the hosting tab's selection, a zoomed sibling, a minimised window --
each a named enum rather than a bool, the rule a sixteen-row table.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.5: Window-level attention: minimised state, raising, and requests QML acts on

A session in a background tab has no `SessionChrome` (`PaneProxy`s exist only for the active tab), so
the `showNotification` / `onBell` / `onAlert` signals it emits today reach nothing
(`SessionChrome.qml` ~271-281 is the only listener). The window, not the pane, has to carry them.

**Files:**
- Modify: `src/contour/window/WindowController.hpp` (public, after `minimizeWindow()` ~471-472; `signals:` ~576-609)
- Modify: `src/contour/window/WindowController.cpp` (after `WindowController::minimizeWindow()` ~1255-1259)
- Modify: `src/contour/qml/Main.qml` (`Connections` ~262-268; `showNotification` ~305-318; `trayIcon`'s
  `onMessageClicked` ~335)
- Test: `src/contour/window/MultiWindow_test.cpp` (append), `src/contour/test/MainWindowQml_test.cpp`
  (mock signals ~262-279; a case before `#include <MainWindowQml_test.moc>`)
- Modify: `src/contour/session/KeyboardRouting_test.cpp` (`RoutingMockController`'s `signals:` ~357-374: it
  also loads Main.qml, so it must carry the new handlers' signals too)

**Interfaces:**
- Consumes: `QWindow::windowStates()/setWindowStates()/raise()/requestActivate()`; `BellSound.qml` (`source`, `play(volume)`).
- Produces (`contour::window::WindowController`):
  ```cpp
  [[nodiscard]] bool isMinimized() const noexcept;
  Q_INVOKABLE void raiseAndActivate();
  void requestNotification(QString const& title, QString const& body);
  void requestBell(QString const& source, double volume);
  void requestAlert();
  signals:
  void notificationRequested(QString title, QString body);
  void bellRequested(QString source, double volume);
  void alertRequested();
  ```
  Main.qml: `function showTrayMessage(title, content)`, `function playWindowBell(source, volume)`.

- [ ] **Step 1: Write the failing C++ test**

Append to `src/contour/window/MultiWindow_test.cpp`:

```cpp

TEST_CASE("WindowController: minimised state, restoring it, and the attention requests Main.qml acts on",
          "[contour][window][notification]")
{
    // Declared before the controller so it outlives it: the controller keeps a raw pointer to it.
    auto const osWindow = std::make_unique<QQuickWindow>();
    TestApp app;
    ScopedController const window { app.manager() };

    SECTION("without an OS window nothing is minimised, and raising is a no-op")
    {
        CHECK_FALSE(window->isMinimized());
        window->raiseAndActivate();
        CHECK_FALSE(window->isMinimized());
    }

    SECTION("a minimised window is reported, and raising restores it without losing maximised")
    {
        window->bindWindow(osWindow.get());
        osWindow->setWindowStates(Qt::WindowMinimized | Qt::WindowMaximized);
        CHECK(window->isMinimized());

        window->raiseAndActivate();
        CHECK_FALSE(window->isMinimized());
        CHECK(osWindow->windowStates().testFlag(Qt::WindowMaximized));
    }

    SECTION("attention requests are re-emitted as signals for the window's QML")
    {
        QSignalSpy notifications(window.controller, &contour::window::WindowController::notificationRequested);
        QSignalSpy bells(window.controller, &contour::window::WindowController::bellRequested);
        QSignalSpy alerts(window.controller, &contour::window::WindowController::alertRequested);

        window->requestNotification(QString::fromUtf8("✓ make finished"), QStringLiteral("took 12s"));
        window->requestBell(QStringLiteral("qrc:/contour/bell.wav"), 0.5);
        window->requestAlert();

        REQUIRE(notifications.count() == 1);
        CHECK(notifications.at(0).at(0).toString() == QString::fromUtf8("✓ make finished"));
        CHECK(notifications.at(0).at(1).toString() == QStringLiteral("took 12s"));
        REQUIRE(bells.count() == 1);
        CHECK(bells.at(0).at(0).toString() == QStringLiteral("qrc:/contour/bell.wav"));
        CHECK(bells.at(0).at(1).toDouble() == 0.5);
        CHECK(alerts.count() == 1);
    }
}
```

(`MultiWindow_test.cpp` already includes `<QtQuick/QQuickWindow>`, `<QtTest/QSignalSpy>`,
`WindowController.hpp` and has `using contour::test::ScopedController; using contour::test::TestApp;`;
add `#include <memory>` after `#include <algorithm>`.)

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL to compile — `'isMinimized': is not a member of 'contour::window::WindowController'`.

- [ ] **Step 3: Implement the controller half**

In `src/contour/window/WindowController.hpp`, after the `minimizeWindow()` declaration add:

```cpp

    /// Whether this window's OS window is minimised. False before a window is bound.
    ///
    /// Read from QWindow::windowStates(), which the platform keeps current. (Wayland compositors do not
    /// report minimising, so there a minimised window reads as shown.)
    [[nodiscard]] bool isMinimized() const noexcept;

    /// Brings this window to the front: un-minimises it, keeping a maximised or full-screen state,
    /// raises it, and asks the window manager to activate it. What clicking a finish notification
    /// does after its tab was made active. A no-op before a window is bound.
    ///
    /// Q_INVOKABLE so Main.qml's tray icon can call it when its message is clicked (off Linux).
    Q_INVOKABLE void raiseAndActivate();

    /// Asks this window's QML to show a tray message on behalf of one of its sessions.
    ///
    /// Routed through the WINDOW because the session may sit in a background tab, which has no
    /// SessionChrome to relay a signal of its own. The delivery for platforms without a desktop Notifier.
    /// @param title The message title.
    /// @param body  The message body.
    void requestNotification(QString const& title, QString const& body) { emit notificationRequested(title, body); }

    /// Asks this window's QML to play a bell sound, for the same reason.
    /// @param source The sound's URL; empty plays nothing.
    /// @param volume 0.0 (silent) .. 1.0 (loudest).
    void requestBell(QString const& source, double volume) { emit bellRequested(source, volume); }

    /// Asks this window's QML to flash its taskbar entry, for the same reason.
    void requestAlert() { emit alertRequested(); }
```

In its `signals:` section, after `void settingsActiveChanged();` add:

```cpp
    /// @see requestNotification.
    void notificationRequested(QString title, QString body);
    /// @see requestBell.
    void bellRequested(QString source, double volume);
    /// @see requestAlert.
    void alertRequested();
```

In `src/contour/window/WindowController.cpp`, after `WindowController::minimizeWindow()` add:

```cpp

bool WindowController::isMinimized() const noexcept
{
    return _osWindow != nullptr && _osWindow->windowStates().testFlag(Qt::WindowMinimized);
}

void WindowController::raiseAndActivate()
{
    if (_osWindow == nullptr)
        return;

    // Cleared from the state set rather than via showNormal(): a window minimised from maximised must
    // come back maximised.
    auto states = _osWindow->windowStates();
    states.setFlag(Qt::WindowMinimized, false);
    _osWindow->setWindowStates(states);
    _osWindow->raise();
    _osWindow->requestActivate();
}
```

- [ ] **Step 4: Run it to see it pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[window][notification]"`
Expected: `All tests passed`.

- [ ] **Step 5: Write the failing QML test**

In `src/contour/test/MainWindowQml_test.cpp`, in `MockMainController`'s `signals:` block after
`void saveLayoutRequested();` add:

```cpp
    // Match Main.qml's window-attention handlers (finish notifications for background tabs); a missing
    // signal here is a QML warning, which the run-wide gate turns into a failure.
    void notificationRequested(QString title, QString body);
    void bellRequested(QString source, double volume);
    void alertRequested();
```

In `src/contour/session/KeyboardRouting_test.cpp`, in `RoutingMockController`'s `signals:` block after
`void saveLayoutRequested();` add the same three (that mock loads Main.qml too, and its keyboard-routing case
fails on any QML diagnostic):

```cpp
    // Match Main.qml's window-attention handlers (finish notifications for background tabs); a missing
    // signal here is a QML warning, which the run-wide gate turns into a failure.
    void notificationRequested(QString title, QString body);
    void bellRequested(QString source, double volume);
    void alertRequested();
```

and before the final `#include <MainWindowQml_test.moc>` (in `MainWindowQml_test.cpp`) add:

```cpp
TEST_CASE("Main.qml window attention: bell and alert requests are handled without diagnostics (offscreen)",
          "[contour][gui][qml][mainwindow][notification]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    // The window's own entry points exist: the Connections handlers call them, and a QML function with
    // untyped parameters is registered with QVariant ones.
    CHECK(root->metaObject()->indexOfMethod("showTrayMessage(QVariant,QVariant)") >= 0);
    CHECK(root->metaObject()->indexOfMethod("playWindowBell(QVariant,QVariant)") >= 0);

    // The tray message is left out on purpose: offscreen there is no tray, and Main.qml says so with a
    // console.warn by design. The bell (multimedia not ready, so nothing is loaded) and the alert run.
    emit controller.bellRequested(QStringLiteral("qrc:/contour/bell.wav"), 0.5);
    emit controller.alertRequested();
    QCoreApplication::processEvents();

    for (auto const& w: warnings.messages())
        UNSCOPED_INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

```

- [ ] **Step 6: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[mainwindow]"`
Expected: FAIL — the new case's two `indexOfMethod(...) >= 0` checks (`-1 >= 0`): Main.qml has neither
function yet. The other `[mainwindow]` cases still pass (the mock's extra signals are harmless).

- [ ] **Step 7: Wire Main.qml**

In `src/contour/qml/Main.qml`, replace

```qml
    Connections {
        target: appWindow.win
        function onCommandPaletteRequested() { commandPalette.open(); }
        function onSaveLayoutRequested() { saveLayoutDialog.open(); }
        function onContextMenuRequested() { terminalContextMenu.popup(); }
        function onTitleBarContextMenuRequested() { titleBarContextMenu.popup(); }
    }
```

with

```qml
    Connections {
        target: appWindow.win
        function onCommandPaletteRequested() { commandPalette.open(); }
        function onSaveLayoutRequested() { saveLayoutDialog.open(); }
        function onContextMenuRequested() { terminalContextMenu.popup(); }
        function onTitleBarContextMenuRequested() { titleBarContextMenu.popup(); }
        // Attention on behalf of a session that may have no pane on screen -- a command finishing in a
        // background tab. Such a session has no SessionChrome, so the window carries the request.
        function onNotificationRequested(title, body) { appWindow.showTrayMessage(title, body); }
        function onBellRequested(source, volume) { appWindow.playWindowBell(source, volume); }
        function onAlertRequested() { appWindow.alert(0); }
    }

    // The bell the window plays for a session without a SessionChrome of its own. Loaded once
    // multimedia is ready, exactly as SessionChrome's bell is (QtMultimedia's probing costs seconds).
    Loader {
        id: windowBellLoader
        active: terminalSessions.multimediaReady
        source: "BellSound.qml"
    }

    function playWindowBell(source, volume) {
        if (windowBellLoader.status !== Loader.Ready)
            return;
        windowBellLoader.item.source = source;
        windowBellLoader.item.play(volume);
    }
```

and replace

```qml
    function showNotification(title, content) {
        // "OSC 777 ; notify ; <TITLE> ; <CONTENT> ST"
        // Example: printf "\033]777;notify;Hello Title;Hello Content\033\\"
        console.debug(windowLog, "main: notification [%1] %2".arg(title).arg(content));
        if (trayIcon.supportsMessages)
        {
            trayIcon.show();
            trayIcon.showMessage("Application Message: %1".arg(title),
                                 "%1".arg(content),
                                 60 * 1000);
        }
        else
            console.warn("main: Notification system not supported!");
    }
```

with

```qml
    function showNotification(title, content) {
        // "OSC 777 ; notify ; <TITLE> ; <CONTENT> ST"
        // Example: printf "\033]777;notify;Hello Title;Hello Content\033\\"
        showTrayMessage("Application Message: %1".arg(title), content);
    }

    // One tray message. Contour's own notices (a command finishing in a background tab) come here
    // directly, without the "Application Message" prefix an application's OSC 777 gets.
    function showTrayMessage(title, content) {
        console.debug(windowLog, "main: notification [%1] %2".arg(title).arg(content));
        if (trayIcon.supportsMessages)
        {
            trayIcon.show();
            trayIcon.showMessage(title, "%1".arg(content), 60 * 1000);
        }
        else
            console.warn("main: Notification system not supported!");
    }
```

and, in the `SystemTrayIcon { id: trayIcon ... }` at the end of the file, replace

```qml
        onMessageClicked: hide()
```

with

```qml
        // Spec §8: clicking a tray message raises the window (the click carries no identity of the message,
        // so the pane cannot be focused -- that part stays Linux-only).
        onMessageClicked: { hide(); if (appWindow.win) appWindow.win.raiseAndActivate(); }
```

- [ ] **Step 8: Run the QML and controller tests**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[mainwindow],[window][notification],[keyboard]"`
Expected: `All tests passed`, and the run-wide QML gate reports nothing.

- [ ] **Step 9: Format and audit**

Run: `clang-format -i src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/window/MultiWindow_test.cpp src/contour/test/MainWindowQml_test.cpp src/contour/session/KeyboardRouting_test.cpp`
Hand-audit: `isMinimized` is a predicate (the `bool` return the guidelines allow); `states` is the one
non-`const` local and is mutated; no `bool` parameter was added.

- [ ] **Step 10: Commit**

```bash
git add src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp \
        src/contour/qml/Main.qml src/contour/window/MultiWindow_test.cpp \
        src/contour/test/MainWindowQml_test.cpp src/contour/session/KeyboardRouting_test.cpp
git commit -F - <<'EOF'
window: carry tray, bell and alert requests for sessions without a pane

A session in a background tab has no SessionChrome, so the signals it
raises for the tray and the bell reach nothing. The window controller
now re-emits them for Main.qml, which shows the tray message, plays a
window-level bell and flashes the taskbar entry. It also reports
whether its OS window is minimised and can restore and raise it, and
clicking a tray message now raises the window.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.6: `TerminalSessionManager` places sessions and routes window attention

**Files:**
- Modify: `src/contour/session/TerminalSessionManager.hpp` (includes; public API after `clearFocusedWindow` ~425-429;
  the private helpers `tabHostingSession` / `windowHostingSession` / `controllerHostingSession` ~771-799)
- Modify: `src/contour/session/TerminalSessionManager.cpp` (after `syncFocusForWindow` ~339-348)
- Test: `src/contour/session/TerminalSessionManager_test.cpp` (append; includes)

**Interfaces:**
- Consumes: `platform::SessionPlacement`, `sessionVisibilityFor` (Task 6.4); `WindowController::isMinimized`,
  `raiseAndActivate`, `requestNotification`, `requestBell`, `requestAlert`, `tabTitles()` (Task 6.5 / existing);
  `vtworkspace::Tab::isZoomed()`, `activePane()`, `rootPane()->findLeaf(SessionId)`, `Window::activeTab()`,
  `indexOf(TabId)`, `SessionModel::activateTab(WindowId, TabId)`; existing `_focusedSession`, `activatePane`.
- Produces (`contour::session::TerminalSessionManager`, public):
  ```cpp
  [[nodiscard]] platform::SessionVisibility sessionVisibility(TerminalSession const& session) const;
  [[nodiscard]] std::string tabTitleForSession(TerminalSession const& session) const;
  void revealSession(TerminalSession const& session);
  void requestWindowNotification(TerminalSession const& session, QString const& title, QString const& body);
  void requestWindowBell(TerminalSession const& session, QString const& source, double volume);
  void requestWindowAlert(TerminalSession const& session);
  ```
  and the three private helpers now take `TerminalSession const*` (all existing callers pass a
  non-const pointer, which converts).

- [ ] **Step 1: Write the failing tests**

In `src/contour/session/TerminalSessionManager_test.cpp` add these includes to the existing blocks:
`#include <contour/session/TerminalSession.hpp>` and `#include <contour/window/WindowController.hpp>`
(contour block), `#include <vtpty/MockPty.hpp>` (after the vtbackend include), `#include <QtQuick/QQuickWindow>`
and `#include <QtTest/QSignalSpy>` (Qt block), `#include <ranges>` and `#include <string>` (std block). Then append:

```cpp

// {{{ Finish notifications: where a session is, what its tab is called, and bringing it forward

namespace
{

/// Closes every tab of @p window, last first, so the sessions wind down as a user closing them would.
void closeAllTabsOf(contour::window::WindowController& window)
{
    for (auto const row: std::views::iota(0, window.count()) | std::views::reverse)
        window.closeTabAtIndex(row);
}

} // namespace

TEST_CASE("TerminalSessionManager: a session's visibility follows focus, tabs, zoom and minimising",
          "[manager][notification]")
{
    using contour::platform::SessionVisibility;

    // Declared before the controller so it outlives it: the controller keeps a raw pointer to it.
    auto const osWindow = std::make_unique<QQuickWindow>();
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const first = window->activeSession();
    REQUIRE(first != nullptr);
    manager.splitActivePane(/*vertical*/ true, first);
    auto* const second = window->activeSession();
    REQUIRE(second != nullptr);
    REQUIRE(first != second);

    SECTION("the focused pane is Focused, and its on-screen sibling VisibleUnfocused")
    {
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::Focused);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::VisibleUnfocused);
    }

    SECTION("a window that loses focus leaves its panes on screen")
    {
        manager.clearFocusedWindow(window.id);
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::VisibleUnfocused);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::VisibleUnfocused);
    }

    SECTION("a zoomed pane covers its sibling")
    {
        manager.toggleActivePaneZoom(second);
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::Focused);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::Hidden);
    }

    SECTION("another tab hides both panes")
    {
        window->createNewTab();
        auto* const third = window->activeSession();
        REQUIRE(third != nullptr);
        CHECK(manager.sessionVisibility(*third) == SessionVisibility::Focused);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::Hidden);
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::Hidden);
    }

    SECTION("a minimised window hides even its focused pane")
    {
        window->bindWindow(osWindow.get());
        osWindow->setWindowStates(Qt::WindowMinimized);
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::Hidden);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::Hidden);
    }

    SECTION("a session no tab hosts is hidden unless it holds focus")
    {
        auto const standalone = std::make_unique<contour::session::TerminalSession>(
            &manager,
            std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }),
            app.app(),
            std::string {});
        CHECK(manager.sessionVisibility(*standalone) == SessionVisibility::Hidden);
        manager.setFocusedSession(standalone.get());
        CHECK(manager.sessionVisibility(*standalone) == SessionVisibility::Focused);
        manager.setFocusedSession(nullptr);
    }

    closeAllTabsOf(*window);
}

TEST_CASE("TerminalSessionManager: revealing a session brings its tab and pane forward", "[manager][notification]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const first = window->activeSession();
    REQUIRE(first != nullptr);
    manager.splitActivePane(/*vertical*/ true, first); // the new pane takes focus
    window->createNewTab();                            // and a second tab hides both
    REQUIRE(window->activeTabIndex() == 1);

    manager.revealSession(*first);

    CHECK(window->activeTabIndex() == 0);
    CHECK(window->activeSession() == first);
    CHECK(manager.sessionVisibility(*first) == contour::platform::SessionVisibility::Focused);

    closeAllTabsOf(*window);
}

TEST_CASE("TerminalSessionManager: a session's tab title and attention requests reach its window",
          "[manager][notification]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    // The label the strip shows, read back through it rather than re-derived.
    manager.setTabTitleForSession(session->modelSessionId(), "build");
    CHECK(manager.tabTitleForSession(*session) == "build");

    QSignalSpy notifications(window.controller, &contour::window::WindowController::notificationRequested);
    QSignalSpy bells(window.controller, &contour::window::WindowController::bellRequested);
    QSignalSpy alerts(window.controller, &contour::window::WindowController::alertRequested);

    manager.requestWindowNotification(*session, QStringLiteral("make finished"), QStringLiteral("took 12s"));
    manager.requestWindowBell(*session, QStringLiteral("qrc:/contour/bell.wav"), 0.5);
    manager.requestWindowAlert(*session);

    REQUIRE(notifications.count() == 1);
    CHECK(notifications.at(0).at(0).toString() == QStringLiteral("make finished"));
    CHECK(notifications.at(0).at(1).toString() == QStringLiteral("took 12s"));
    REQUIRE(bells.count() == 1);
    CHECK(bells.at(0).at(0).toString() == QStringLiteral("qrc:/contour/bell.wav"));
    CHECK(bells.at(0).at(1).toDouble() == 0.5);
    CHECK(alerts.count() == 1);

    closeAllTabsOf(*window);
}

// }}}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL to compile — `'sessionVisibility': is not a member of 'contour::session::TerminalSessionManager'`.

- [ ] **Step 3: Declare the API**

In `src/contour/session/TerminalSessionManager.hpp`, add `#include <contour/platform/FinishNotification.hpp>`
after `#include <contour/config/LayoutStore.hpp>` (keep the block sorted: before `display/`), and add
`#include <string>` to the std block. After the `clearFocusedWindow` declaration add:

```cpp

    /// Where @p session sits relative to what the user is looking at, for finish notifications.
    ///
    /// Gathers the four facts -- terminal focus, the hosting tab's selection, a zoomed sibling, the
    /// window's minimised state -- and leaves the rule to platform::sessionVisibilityFor(), where it is
    /// table-tested. GUI thread.
    /// @param session The session to place; one no tab hosts is Hidden unless it holds focus.
    /// @return Its visibility.
    [[nodiscard]] platform::SessionVisibility sessionVisibility(TerminalSession const& session) const;

    /// The label the tab strip shows for the tab hosting @p session -- read back through the strip's
    /// own TitleRole, so a notification names the tab exactly as the user sees it.
    /// @param session The session whose tab to name.
    /// @return The label, or empty when no tab hosts @p session.
    [[nodiscard]] std::string tabTitleForSession(TerminalSession const& session) const;

    /// Brings @p session in front of the user: makes its tab and its pane active, then restores, raises
    /// and activates its window. What clicking a finish notification does. A no-op for a session no
    /// tab hosts.
    /// @param session The session to reveal.
    void revealSession(TerminalSession const& session);

    /// Shows a tray message through the window hosting @p session (platforms without a desktop
    /// Notifier). Routed through the window because a session in a background tab has no SessionChrome
    /// of its own. A no-op for a session no window hosts.
    /// @param session The session the message is about.
    /// @param title   The message title.
    /// @param body    The message body.
    void requestWindowNotification(TerminalSession const& session, QString const& title, QString const& body);

    /// Plays a bell sound in the window hosting @p session, routed as requestWindowNotification is.
    /// @param session The session ringing.
    /// @param source  The sound's URL (TerminalSession::getBellSource()); empty plays nothing.
    /// @param volume  0.0 (silent) .. 1.0 (loudest).
    void requestWindowBell(TerminalSession const& session, QString const& source, double volume);

    /// Flashes the taskbar entry of the window hosting @p session, routed as requestWindowNotification is.
    /// @param session The session asking for attention.
    void requestWindowAlert(TerminalSession const& session);
```

In the private section, change the parameter of the three helpers from `TerminalSession* session` to
`TerminalSession const* session` — they only read `modelSessionId()`:

```cpp
    [[nodiscard]] vtworkspace::Tab* tabHostingSession(TerminalSession const* session) const noexcept
```
```cpp
    [[nodiscard]] vtworkspace::Window* windowHostingSession(TerminalSession const* session) const noexcept
```
```cpp
    [[nodiscard]] window::WindowController* controllerHostingSession(TerminalSession const* session) const noexcept
```

(bodies unchanged).

- [ ] **Step 4: Implement it**

In `src/contour/session/TerminalSessionManager.cpp`, after `TerminalSessionManager::syncFocusForWindow` add:

```cpp

platform::SessionVisibility TerminalSessionManager::sessionVisibility(TerminalSession const& session) const
{
    auto const* tab = tabHostingSession(&session);
    auto const* win = tab != nullptr ? _model->window(_model->windowOfTab(tab->id())) : nullptr;
    auto const* controller = win != nullptr ? controllerFor(win->id()) : nullptr;

    // A zoomed tab draws its active pane alone, so every other pane of it is off screen.
    auto const paneShown =
        tab != nullptr && (!tab->isZoomed() || tab->activePane()->session() == session.modelSessionId());

    return platform::sessionVisibilityFor(platform::SessionPlacement {
        .focus = &session == _focusedSession ? platform::SessionFocus::Here : platform::SessionFocus::Elsewhere,
        .tab = win != nullptr && win->activeTab() == tab ? platform::TabSelection::Active
                                                         : platform::TabSelection::Background,
        .pane = paneShown ? platform::PaneExposure::Shown : platform::PaneExposure::Covered,
        .window = controller != nullptr && controller->isMinimized() ? platform::WindowPresence::Minimized
                                                                     : platform::WindowPresence::Shown,
    });
}

std::string TerminalSessionManager::tabTitleForSession(TerminalSession const& session) const
{
    auto const* tab = tabHostingSession(&session);
    if (tab == nullptr)
        return {};

    auto const windowId = _model->windowOfTab(tab->id());
    auto const* win = _model->window(windowId);
    auto const* controller = controllerFor(windowId);
    if (win == nullptr || controller == nullptr)
        return {};

    auto const titles = controller->tabTitles();
    auto const row = win->indexOf(tab->id());
    return row >= 0 && static_cast<size_t>(row) < titles.size() ? titles[static_cast<size_t>(row)]
                                                                : std::string {};
}

void TerminalSessionManager::revealSession(TerminalSession const& session)
{
    auto* tab = tabHostingSession(&session);
    if (tab == nullptr)
        return;

    auto const windowId = _model->windowOfTab(tab->id());
    _model->activateTab(windowId, tab->id());
    if (auto const* leaf = tab->rootPane()->findLeaf(session.modelSessionId()); leaf != nullptr)
        activatePane(tab->id(), leaf->id());

    // Terminal focus follows through syncFocusForWindow once the window is active, which in turn
    // withdraws a finish notification set to clear on focus.
    if (auto* controller = controllerFor(windowId))
        controller->raiseAndActivate();
}

void TerminalSessionManager::requestWindowNotification(TerminalSession const& session,
                                                       QString const& title,
                                                       QString const& body)
{
    if (auto* controller = controllerHostingSession(&session))
        controller->requestNotification(title, body);
}

void TerminalSessionManager::requestWindowBell(TerminalSession const& session, QString const& source, double volume)
{
    if (auto* controller = controllerHostingSession(&session))
        controller->requestBell(source, volume);
}

void TerminalSessionManager::requestWindowAlert(TerminalSession const& session)
{
    if (auto* controller = controllerHostingSession(&session))
        controller->requestAlert();
}
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[manager]"` and `... "[focus]"`
Expected: `All tests passed` for both — the new `[notification]` cases, and the existing focus-routing
cases unchanged by the `const*` helper signatures.

- [ ] **Step 6: Format and audit**

Run: `clang-format -i src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp`
Hand-audit: no local named `window` (it would shadow the `window::` namespace — hence `win`); the
`static_cast<size_t>(row)` follows the `row >= 0` check; `closeAllTabsOf` is in the anonymous
namespace and uses no C-style loop.

- [ ] **Step 7: Commit**

```bash
git add src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp \
        src/contour/session/TerminalSessionManager_test.cpp
git commit -F - <<'EOF'
session: place sessions on screen and route attention to their window

sessionVisibility() gathers focus, tab selection, zoom and minimised
state for the pure sessionVisibilityFor(); tabTitleForSession() reads
the strip's own label; revealSession() activates a session's tab and
pane and raises its window; requestWindow{Notification,Bell,Alert}()
reach the hosting window for sessions that have no pane on screen.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.7: `TerminalSession` raises finish notifications

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp` (includes ~10-13; Events overrides after `progressChanged` ~666;
  private methods after `refreshGuiTabInfoForStatusLine()` ~1049; members before `_desktopNotifier` ~1240-1243)
- Modify: `src/contour/session/TerminalSession.cpp` (includes ~32-46; constructor body end ~282-284; after
  `progressChanged` ~1529-1539; `sendFocusInEvent` ~2282-2294)
- Test: `src/contour/session/TerminalSession_test.cpp` (append; includes)

**Interfaces:**
- Consumes: C1 `Terminal::Events::commandBlockFinished(CommandBlockSummary const&)` (parser thread, state mutex
  held); `platform::finishNotificationFor`, `effectsOf`, `FinishEffect`, `finishNotificationIdentifier` (6.3);
  `TerminalSessionManager::sessionVisibility`, `tabTitleForSession`, `revealSession`, `requestWindowNotification`,
  `requestWindowBell`, `requestWindowAlert` (6.6); `platform::Notifier::notify/close` and its
  `actionInvoked` / `notificationClosed` signals (`Notifier.hpp` 42-58); `platform::postToObject`.
- Produces (`TerminalSession`):
  ```cpp
  void commandBlockFinished(vtbackend::CommandBlockSummary const& summary) override;   // public, Events
  // private:
  void handleCommandBlockFinished(vtbackend::CommandBlockSummary const& summary);
  void deliverFinishNotice(vtbackend::CommandBlockId block, platform::FinishNotificationRequest const& request,
                           config::NotifyClearOn clearOn);
  void withdrawFinishNotice();
  void forgetFinishNotice() noexcept;
  void connectFinishNoticeEvents();
  struct FinishNotice { vtbackend::CommandBlockId block {}; config::NotifyClearOn clearOn = config::NotifyClearOn::Focus; };
  std::optional<FinishNotice> _finishNotice;
  ```

- [ ] **Step 1: Write the failing tests**

In `src/contour/session/TerminalSession_test.cpp` add `#include <contour/platform/FinishNotification.hpp>` to the
contour block and `#include <QtCore/QEvent>` to the Qt block. Append at the end of the file:

```cpp

// ============================================================================================
// Finish notifications (semantic blocks, spec §8)
// ============================================================================================

namespace
{

using contour::config::FinishNotificationConfig;
using contour::config::NotifyAction;
using contour::config::NotifyClearOn;
using contour::config::NotifyOutcome;
using contour::config::NotifyWhen;
using contour::platform::finishNotificationIdentifier;

using namespace std::chrono_literals;

/// A display-less session whose profile notifies under @p policy, raising through @p notifier.
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeFinishNotifyingSession(
    contour::ContourGuiApp& app, FinishNotificationConfig policy, std::unique_ptr<contour::platform::Notifier> notifier)
{
    auto name = registerProfile(app, "finish-notify", [&](contour::config::TerminalProfile& profile) {
        profile.notifyOnCommandFinish = policy;
    });
    return std::make_unique<contour::session::TerminalSession>(&app.sessionsManager(),
                                                               std::make_unique<vtpty::MockPty>(TestPageSize),
                                                               app,
                                                               std::move(name),
                                                               std::nullopt,
                                                               std::nullopt,
                                                               std::move(notifier));
}

/// Runs one shell-integrated command through @p session's parser -- prompt, typed input, output and exit
/// status, as an OSC 133 shell reports them -- and delivers what that posted to the GUI thread.
void runAndDeliverShellCommand(contour::session::TerminalSession& session,
                               std::string_view commandLine,
                               int exitCode)
{
    session.terminal().writeToScreen(std::format("\033]133;A\033\\$ \033]133;B\033\\{}\r\n", commandLine));
    session.terminal().writeToScreen(std::format("\033]133;C;cmdline_url={}\033\\output\r\n", commandLine));
    session.terminal().writeToScreen(std::format("\033]133;D;{}\033\\", exitCode));
    QCoreApplication::sendPostedEvents(&session, QEvent::MetaCall);
}

/// Focuses @p session and takes focus away again, returning how many notifications that withdrew.
/// Platform-independent evidence: withdrawing goes through the notifier everywhere.
[[nodiscard]] size_t withdrawnByFocus(contour::session::TerminalSessionManager& manager,
                                      contour::session::TerminalSession& session,
                                      RecordingNotifier const& recorder)
{
    auto const before = recorder.closed.size();
    manager.setFocusedSession(&session);
    manager.setFocusedSession(nullptr);
    return recorder.closed.size() - before;
}

/// Takes the manager's focus away before the standalone session it may point at is destroyed.
class FocusReset
{
  public:
    explicit FocusReset(contour::session::TerminalSessionManager& manager): _manager { manager } {}
    ~FocusReset() { _manager.setFocusedSession(nullptr); }
    FocusReset(FocusReset const&) = delete;
    FocusReset& operator=(FocusReset const&) = delete;
    FocusReset(FocusReset&&) = delete;
    FocusReset& operator=(FocusReset&&) = delete;

  private:
    contour::session::TerminalSessionManager& _manager;
};

} // namespace

TEST_CASE("TerminalSession: a finished command raises one finish notification", "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    auto const session = makeFinishNotifyingSession(testApp.app(), { .minDuration = 0s }, std::move(notifier));
    auto const identifier = finishNotificationIdentifier(session->id());

    runAndDeliverShellCommand(*session, "make", 2);

#ifdef __linux__
    REQUIRE(recorder->raised.size() == 1);
    CHECK(recorder->raised[0].identifier == identifier);
    CHECK(recorder->raised[0].title == "✗ make failed (exit 2)");
    CHECK(recorder->raised[0].body.starts_with("took "));
#endif

    // On every platform the notice is outstanding -- off Linux it went to the window's tray -- so
    // focusing the pane withdraws exactly it.
    REQUIRE(withdrawnByFocus(testApp.manager(), *session, *recorder) == 1);
    CHECK(recorder->closed.back() == identifier);
}

TEST_CASE("TerminalSession: a command shorter than min_duration raises nothing", "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    // The default policy: ten seconds, which a command run straight through the parser never reaches.
    auto const session = makeFinishNotifyingSession(testApp.app(), {}, std::move(notifier));

    runAndDeliverShellCommand(*session, "make", 0);

    CHECK(recorder->raised.empty());
    CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 0);
}

TEST_CASE("TerminalSession: without shell integration nothing is ever notified", "[contour][session][notification]")
{
    // Review focus 5: a shell without OSC 133 reports no command, so nothing finishes. tcsh's
    // unconditional `D`, arriving with no command behind it, is no finish either.
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    auto const session = makeFinishNotifyingSession(
        testApp.app(), { .when = NotifyWhen::Always, .minDuration = 0s }, std::move(notifier));

    session->terminal().writeToScreen("$ make\r\nbuilding\r\n");
    session->terminal().writeToScreen("\033]133;D;1\033\\");
    QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);

    CHECK(recorder->raised.empty());
    CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 0);
}

TEST_CASE("TerminalSession: a newer finish replaces the older notification", "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    auto const session = makeFinishNotifyingSession(testApp.app(), { .minDuration = 0s }, std::move(notifier));

    runAndDeliverShellCommand(*session, "make", 0);
    runAndDeliverShellCommand(*session, "make", 0);

#ifdef __linux__
    REQUIRE(recorder->raised.size() == 2);
    CHECK(recorder->raised[0].identifier == recorder->raised[1].identifier);
#endif

    // One notice outstanding, not two: the second took the first one's place.
    CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 1);
}

TEST_CASE("TerminalSession: the focused pane is not notified under `unfocused`", "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    auto const session = makeFinishNotifyingSession(testApp.app(), { .minDuration = 0s }, std::move(notifier));
    FocusReset const focusReset { testApp.manager() };

    testApp.manager().setFocusedSession(session.get());
    runAndDeliverShellCommand(*session, "make", 0);
    CHECK(recorder->raised.empty());

    testApp.manager().setFocusedSession(nullptr);
    CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 0);
}

TEST_CASE("TerminalSession: `outcome: failure` notifies only for a command that failed",
          "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    auto const session = makeFinishNotifyingSession(
        testApp.app(), { .minDuration = 0s, .outcome = NotifyOutcome::Failure }, std::move(notifier));

    runAndDeliverShellCommand(*session, "make", 0);
    CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 0);

    runAndDeliverShellCommand(*session, "make", 2);
    CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 1);
}

TEST_CASE("TerminalSession: `clear_on: never` survives a focus-in", "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    auto const session = makeFinishNotifyingSession(
        testApp.app(), { .minDuration = 0s, .clearOn = NotifyClearOn::Never }, std::move(notifier));

    runAndDeliverShellCommand(*session, "make", 0);

    CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 0);
}

TEST_CASE("TerminalSession: the desktop retiring a finish notification forgets it", "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    auto const session = makeFinishNotifyingSession(testApp.app(), { .minDuration = 0s }, std::move(notifier));
    auto const identifier = finishNotificationIdentifier(session->id());

    runAndDeliverShellCommand(*session, "make", 0);

    SECTION("a click retires it (and would bring the pane forward, had it a tab)")
    {
        recorder->fireActivated(identifier);
        CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 0);
    }

    SECTION("a close retires it")
    {
        recorder->fireClosed(identifier, /*reason*/ 2);
        CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 0);
    }

    SECTION("another notification's events leave it alone")
    {
        recorder->fireActivated("someone-else");
        recorder->fireClosed("someone-else", /*reason*/ 2);
        CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 1);
    }
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[session][notification]"`
Expected: builds (the tests use only existing API plus Tasks 6.2–6.6); FAIL at runtime —
"a finished command raises one finish notification", "a newer finish replaces...", "`outcome: failure`..."
and the third section of "the desktop retiring..." report `withdrawnByFocus(...) == 0` where 1 was
expected, because nothing overrides `commandBlockFinished` yet. The no-notice cases already pass.

- [ ] **Step 3: Declare the override, helpers and state**

In `src/contour/session/TerminalSession.hpp`:

1. After `#include <contour/platform/ColorConversion.hpp>` add `#include <contour/platform/FinishNotification.hpp>`;
   add `#include <optional>` to the std block.
2. After `void progressChanged(vtbackend::Progress progress) override;` add
   ```cpp
       /// A shell command block finished (OSC 133;D, or an OSC 3008 end while 3008 owns the marks).
       ///
       /// Raised on the parser thread with Terminal::_stateMutex held, exactly like progressChanged():
       /// this only copies the summary and posts the decision to the GUI thread, reading no terminal
       /// state. @see handleCommandBlockFinished.
       void commandBlockFinished(vtbackend::CommandBlockSummary const& summary) override;
   ```
3. After the `void refreshGuiTabInfoForStatusLine();` declaration add
   ```cpp

       /// Decides on, and delivers, the finish notification for @p summary. GUI thread.
       void handleCommandBlockFinished(vtbackend::CommandBlockSummary const& summary);

       /// Delivers @p request about @p block, remembering a raised notification for withdrawal on
       /// @p clearOn. Linux raises it through the desktop notifier; elsewhere it goes to the hosting
       /// window's tray, as the bell always does (a background tab has no SessionChrome to relay it).
       void deliverFinishNotice(vtbackend::CommandBlockId block,
                                platform::FinishNotificationRequest const& request,
                                config::NotifyClearOn clearOn);

       /// Withdraws this session's finish notification from the desktop, if it has one.
       void withdrawFinishNotice();

       /// Forgets this session's finish notification without withdrawing it: the desktop retired it.
       void forgetFinishNotice() noexcept;

       /// Connects the notifier's click and close events for this session's finish notification once:
       /// every finish notification of a session carries the same identifier.
       void connectFinishNoticeEvents();
   ```
4. Directly before the `/// How desktop notifications are raised. Never null: ...` comment of `_desktopNotifier` add
   ```cpp
       /// The finish notification this session has on the desktop: which block it reports and what
       /// withdraws it. At most one -- a newer finish replaces the older under the same identifier.
       /// Recorded on every platform, so withdrawal is uniform (off Linux it is a no-op on a NullNotifier).
       struct FinishNotice
       {
           vtbackend::CommandBlockId block {};
           config::NotifyClearOn clearOn = config::NotifyClearOn::Focus;
       };
       std::optional<FinishNotice> _finishNotice;

   ```

- [ ] **Step 4: Implement them**

In `src/contour/session/TerminalSession.cpp`:

1. Add `#include <QtCore/QDir>` after `#include <QtCore/QDebug>`.
2. Constructor, replace
   ```cpp
       _profile = *_config.profile(_profileName); // XXX do it again. but we've to be more efficient here
       configureTerminal();
   }
   ```
   with
   ```cpp
       _profile = *_config.profile(_profileName); // XXX do it again. but we've to be more efficient here
       configureTerminal();
       connectFinishNoticeEvents();
   }
   ```
3. After `TerminalSession::progressChanged` add:

```cpp

void TerminalSession::commandBlockFinished(vtbackend::CommandBlockSummary const& summary)
{
    // Same threading constraint as progressChanged() above: the parser thread, with the state mutex
    // held. The summary is a value copy made for exactly this hop; nothing here reads the terminal.
    // postToObject targets `this`, so Qt cancels the queued call if the session dies first.
    platform::postToObject(this, [this, summary]() { handleCommandBlockFinished(summary); });
}

void TerminalSession::handleCommandBlockFinished(vtbackend::CommandBlockSummary const& summary)
{
    auto const& policy = _profile.notifyOnCommandFinish.value();

    // The home is read where the tab tooltip reads it (WindowController::tabWorkingDirectory), so the
    // two never disagree about what `~` is.
    auto const request = platform::finishNotificationFor(summary,
                                                         policy,
                                                         _manager->sessionVisibility(*this),
                                                         _manager->tabTitleForSession(*this),
                                                         QDir::homePath().toStdString());
    if (request)
        deliverFinishNotice(summary.id, *request, policy.clearOn);
}

void TerminalSession::deliverFinishNotice(vtbackend::CommandBlockId block,
                                          platform::FinishNotificationRequest const& request,
                                          config::NotifyClearOn clearOn)
{
    auto const effects = platform::effectsOf(request.action);

    if (effects.test(platform::FinishEffect::Notification))
    {
#ifdef __linux__
        // The same identifier every time, so the desktop replaces this session's previous finish
        // notification rather than stacking a second one under it.
        auto notification = vtbackend::DesktopNotification {};
        notification.identifier = platform::finishNotificationIdentifier(_id);
        notification.title = request.title;
        notification.body = request.body;
        _desktopNotifier->notify(notification);
#else
        _manager->requestWindowNotification(
            *this, QString::fromStdString(request.title), QString::fromStdString(request.body));
#endif
        _finishNotice = FinishNotice { .block = block, .clearOn = clearOn };
    }

    if (effects.test(platform::FinishEffect::Bell))
    {
        auto const& bell = _profile.bell.value();
        _manager->requestWindowBell(*this, getBellSource(), static_cast<double>(bell.volume));
        if (bell.alert)
            _manager->requestWindowAlert(*this);
    }
}

void TerminalSession::withdrawFinishNotice()
{
    if (!_finishNotice)
        return;

    forgetFinishNotice();
    // Through the notifier on every platform: where there is none, it is a NullNotifier, and a tray
    // message cannot be withdrawn anyway.
    _desktopNotifier->close(platform::finishNotificationIdentifier(_id));
}

void TerminalSession::forgetFinishNotice() noexcept
{
    _finishNotice.reset();
}

void TerminalSession::connectFinishNoticeEvents()
{
    // Persistent rather than once per notification (connectOnceMatching): every finish notification of
    // this session carries the same identifier, so one pair of connections serves all of them, and the
    // OSC 99 handlers sharing these signals filter by their own identifiers.
    auto const isOurs = [this](QString const& identifier) {
        return _finishNotice.has_value() && identifier.toStdString() == platform::finishNotificationIdentifier(_id);
    };

    connect(_desktopNotifier.get(),
            &platform::Notifier::actionInvoked,
            this,
            [this, isOurs](QString const& identifier) {
                if (!isOurs(identifier))
                    return;
                forgetFinishNotice();
                _manager->revealSession(*this);
            });

    connect(_desktopNotifier.get(),
            &platform::Notifier::notificationClosed,
            this,
            [this, isOurs](QString const& identifier, uint /*reason*/, vtbackend::CloseReport /*report*/) {
                if (isOurs(identifier))
                    forgetFinishNotice();
            });
}
```

4. `TerminalSession::sendFocusInEvent`, replace
   ```cpp
       if (_display)
           _display->setBlurBehind(_profile.background.value().blur);

       scheduleRedraw();
   }
   ```
   (the one inside `sendFocusInEvent`) with
   ```cpp
       if (_display)
           _display->setBlurBehind(_profile.background.value().blur);

       // clear_on: focus -- the user is looking at the pane now, so the notification has done its job.
       if (_finishNotice && _finishNotice->clearOn == config::NotifyClearOn::Focus)
           withdrawFinishNotice();

       scheduleRedraw();
   }
   ```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[session][notification]"`
and `out/build/clangcl-debug/bin/contour_gui_test.exe "[session]"`
Expected: `All tests passed` for both (the existing OSC 99 notification wiring test included).

- [ ] **Step 6: Format and audit**

Run: `clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp`
Hand-audit: `commandBlockFinished` reads nothing but its argument (the C1 threading rule); the lambda
captures `summary` by value; `notification` is the only non-`const` local and only exists on Linux
(no unused variable elsewhere); `FocusReset` and the helpers are in the anonymous namespace.

- [ ] **Step 7: Commit**

```bash
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp \
        src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
session: notify when a command finishes in a pane nobody is watching

commandBlockFinished() copies the summary off the parser thread; on the
GUI thread the profile's notify_on_command_finish policy, the session's
visibility and its tab's label decide whether and what to tell. One
notification identifier per session, so a newer finish replaces the
older; focusing the pane withdraws it, clicking it brings the pane
forward, and the bell rings through the hosting window.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.8: `clear_on: next` — withdraw when the next command starts

There is deliberately no "command started" event (spec §4.5: no new event beyond `commandBlockFinished`).
A start is noticed two ways, both level-triggered against the record store, so neither can miss one:
(1) while a `next` notice is outstanding, `screenUpdated()` — raised after every output batch, the
`;C` of the next command included — posts one coalesced check that looks for a `Running` block under
the terminal lock on the GUI thread; (2) the next command's own `commandBlockFinished` withdraws the
notice outright, covering a command that starts and ends between two checks.

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp` (private method; a private enum and two members next to
  `_finishNotice`)
- Modify: `src/contour/session/TerminalSession.cpp` (`screenUpdated` ~624; `handleCommandBlockFinished`,
  `deliverFinishNotice`, `forgetFinishNotice` from Task 6.7)
- Test: `src/contour/session/TerminalSession_test.cpp` (append)

**Interfaces:**
- Consumes: C1 `Terminal::commandBlocks() const`, `CommandBlockStore::current() const noexcept`,
  `CommandBlockState::Running`; `core::locked`.
- Produces (`TerminalSession`, private):
  ```cpp
  void withdrawFinishNoticeIfNextCommandStarted();
  enum class FinishNoticeWait : uint8_t { Nothing = 0, NextCommand };
  std::atomic<FinishNoticeWait> _finishNoticeWait { FinishNoticeWait::Nothing };
  std::atomic_flag _finishNoticeCheckPending = ATOMIC_FLAG_INIT;
  ```

- [ ] **Step 1: Write the failing test**

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp

TEST_CASE("TerminalSession: `clear_on: next` withdraws the notification when the next command starts",
          "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const recorder = notifier.get();
    auto const session = makeFinishNotifyingSession(
        testApp.app(), { .minDuration = 0s, .clearOn = NotifyClearOn::Next }, std::move(notifier));
    auto const identifier = finishNotificationIdentifier(session->id());

    runAndDeliverShellCommand(*session, "make", 0);
    REQUIRE(recorder->closed.empty());

    SECTION("a new prompt alone is not a start")
    {
        session->terminal().writeToScreen("\033]133;A\033\\$ ");
        QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);
        CHECK(recorder->closed.empty());
    }

    SECTION("the next command's output start withdraws it")
    {
        session->terminal().writeToScreen("\033]133;A\033\\$ \033]133;B\033\\make test\r\n");
        session->terminal().writeToScreen("\033]133;C;cmdline_url=make%20test\033\\");
        QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);
        REQUIRE(recorder->closed.size() == 1);
        CHECK(recorder->closed[0] == identifier);
    }

    SECTION("a next command that comes and goes within one read withdraws it once")
    {
        session->terminal().writeToScreen(
            "\033]133;A\033\\$ \033]133;B\033\\true\r\n\033]133;C;cmdline_url=true\033\\\033]133;D;0\033\\");
        QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);
        // Withdrawn once; `true` then raised its own notice in the old one's place.
        CHECK(recorder->closed.size() == 1);
    }

    SECTION("focus does not withdraw it")
    {
        CHECK(withdrawnByFocus(testApp.manager(), *session, *recorder) == 0);
    }
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[session][notification]"`
Expected: FAIL in the new case's sections "the next command's output start withdraws it" and
"a next command that comes and goes within one read withdraws it once" — `recorder->closed.size() == 1`
is `0 == 1`. Its other two sections and every Task 6.7 case pass.

- [ ] **Step 3: Declare the check and its cross-thread state**

In `src/contour/session/TerminalSession.hpp`, after the `void connectFinishNoticeEvents();` declaration add

```cpp

    /// Withdraws a `clear_on: next` finish notice once a later command is running. GUI thread; takes
    /// the terminal lock briefly to read the record store.
    void withdrawFinishNoticeIfNextCommandStarted();
```

and after `std::optional<FinishNotice> _finishNotice;` add

```cpp
    /// What an outstanding finish notice waits for before it is withdrawn, as far as the parser thread
    /// needs to know -- an enum rather than a bool, so the store and the load read as what they mean.
    enum class FinishNoticeWait : uint8_t
    {
        Nothing = 0, ///< No notice, or one that focus (not a command) withdraws.
        NextCommand, ///< A `clear_on: next` notice: the next command's start withdraws it.
    };
    /// Mirrors `_finishNotice->clearOn == Next` for the parser thread, which may not read
    /// _finishNotice: screenUpdated() asks it whether output is worth a look at the record store.
    std::atomic<FinishNoticeWait> _finishNoticeWait { FinishNoticeWait::Nothing };
    /// Collapses the checks screenUpdated() posts to one in flight, as _searchTallyPostPending does.
    std::atomic_flag _finishNoticeCheckPending = ATOMIC_FLAG_INIT;
```

- [ ] **Step 4: Implement it**

In `src/contour/session/TerminalSession.cpp`:

1. At the top of `TerminalSession::screenUpdated()`, right after `ZoneScoped;` and BEFORE `if (!_display) return;`
   (a background tab has no display, and it is the main case), add:
   ```cpp

       // A finish notice waiting for the next command to start. Whether one has is a question for the
       // record store, which needs the state mutex this callback may already be raised under -- so it
       // is asked on the GUI thread, once per burst of output rather than once per batch.
       if (_finishNoticeWait.load(std::memory_order_acquire) == FinishNoticeWait::NextCommand
           && !_finishNoticeCheckPending.test_and_set(std::memory_order_acq_rel))
           platform::postToObject(this, [this]() {
               _finishNoticeCheckPending.clear(std::memory_order_release);
               withdrawFinishNoticeIfNextCommandStarted();
           });
   ```
2. In `handleCommandBlockFinished`, before `auto const& policy = ...` add:
   ```cpp
       // A later command has finished, so it certainly started -- even one that came and went between
       // two start checks, which the screenUpdated() path could miss.
       if (_finishNotice && _finishNotice->clearOn == config::NotifyClearOn::Next && _finishNotice->block != summary.id)
           withdrawFinishNotice();

   ```
3. In `deliverFinishNotice`, replace
   ```cpp
           _finishNotice = FinishNotice { .block = block, .clearOn = clearOn };
       }
   ```
   with
   ```cpp
           _finishNotice = FinishNotice { .block = block, .clearOn = clearOn };
           _finishNoticeWait.store(clearOn == config::NotifyClearOn::Next ? FinishNoticeWait::NextCommand
                                                                          : FinishNoticeWait::Nothing,
                                   std::memory_order_release);
           // The next command may already be running if this post arrived late.
           withdrawFinishNoticeIfNextCommandStarted();
       }
   ```
4. Replace the body of `forgetFinishNotice`:
   ```cpp
   void TerminalSession::forgetFinishNotice() noexcept
   {
       _finishNotice.reset();
       _finishNoticeWait.store(FinishNoticeWait::Nothing, std::memory_order_release);
   }
   ```
5. After `forgetFinishNotice` add:
   ```cpp

   void TerminalSession::withdrawFinishNoticeIfNextCommandStarted()
   {
       if (!_finishNotice || _finishNotice->clearOn != config::NotifyClearOn::Next)
           return;

       // The notice's own block has finished, so an unfinished block that is RUNNING can only be a later
       // one. A new prompt alone (Prompting) is no start: the user may yet walk away from it.
       auto const nextStarted = core::locked(_terminal, [&]() {
           auto const* current = _terminal.commandBlocks().current();
           return current != nullptr && current->state == vtbackend::CommandBlockState::Running;
       });
       if (nextStarted)
           withdrawFinishNotice();
   }
   ```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[session][notification]"`
Expected: `All tests passed` (Task 6.7's cases included).

- [ ] **Step 6: Format and audit**

Run: `clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp`
Hand-audit: `screenUpdated` takes no lock and reads only atomics on the parser thread; the posted
lambda captures only `this` (cancelled with the session); acquire/release pairs match; the cross-thread
state is the `FinishNoticeWait` enum, not a `bool` (README "Global constraints": enum class over bool).
For the `clang-tsan` tree (README "Building"), this path is `[threading]`-relevant: note it for the gate.

- [ ] **Step 7: Commit**

```bash
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp \
        src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
session: withdraw a clear_on: next finish notice when a command starts

No new event: while such a notice is out, screenUpdated() posts one
coalesced check that looks for a Running block on the GUI thread, and
the next command's own finish withdraws it too, so a command that
comes and goes between two checks is not missed.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.9: End to end — split panes, background tabs and the window bell

The chain through real manager-created sessions: `Hidden` vs `VisibleUnfocused` for split panes, a
background tab and a zoomed sibling, observed through the bell route (platform-independent, and never
touches a real D-Bus session the way a Linux `notify` from a manager-created session would).

**Files:**
- Test: `src/contour/session/TerminalSession_test.cpp` (append; includes)

**Interfaces:**
- Consumes: everything from Tasks 6.2–6.8; `contour::test::MockPtySessionFactory`, `ScopedController`
  (`GuiTestFixtures.hpp`); `WindowController::bellRequested`, `alertRequested`.
- Produces: tests only.

- [ ] **Step 1: Write the test**

In `src/contour/session/TerminalSession_test.cpp` add `#include <contour/window/WindowController.hpp>` to the
contour block and `#include <QtTest/QSignalSpy>` to the Qt block, then append:

```cpp

TEST_CASE("TerminalSession: a finish rings the window bell only where the policy's visibility allows",
          "[contour][session][notification][manager]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = testApp.manager();

    // Every session below is created under the app's default profile, which a session copies at birth.
    auto* const profile = testApp.app().config().profile(testApp.app().profileName());
    REQUIRE(profile != nullptr);
    profile->notifyOnCommandFinish =
        FinishNotificationConfig { .when = NotifyWhen::Hidden, .minDuration = 0s, .action = NotifyAction::Bell };

    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const background = window->activeSession();
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(background != nullptr);
    REQUIRE(left != nullptr);
    manager.splitActivePane(/*vertical*/ true, left);
    auto* const right = window->activeSession();
    REQUIRE(right != nullptr);
    REQUIRE(right != left);

    QSignalSpy bells(window.controller, &contour::window::WindowController::bellRequested);
    QSignalSpy alerts(window.controller, &contour::window::WindowController::alertRequested);

    SECTION("an unfocused split pane is on screen, so `hidden` stays quiet")
    {
        runAndDeliverShellCommand(*left, "make", 0);
        CHECK(bells.isEmpty());
    }

    SECTION("a pane in a background tab is hidden, so it rings through its window")
    {
        runAndDeliverShellCommand(*background, "make", 0);
        REQUIRE(bells.count() == 1);
        CHECK(bells.at(0).at(0).toString() == QStringLiteral("qrc:/contour/bell.wav"));
        CHECK(bells.at(0).at(1).toDouble() == 1.0);
        CHECK(alerts.count() == 1); // the default profile's bell alerts
    }

    SECTION("zooming the focused pane hides its sibling")
    {
        manager.toggleActivePaneZoom(right);
        runAndDeliverShellCommand(*left, "make", 0);
        CHECK(bells.count() == 1);
    }

    for (auto const row: std::views::iota(0, window->count()) | std::views::reverse)
        window->closeTabAtIndex(row);
}
```

- [ ] **Step 2: Run it**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
then `out/build/clangcl-debug/bin/contour_gui_test.exe "[notification][manager]"`
Expected: `All tests passed`. This task adds coverage over code already landed; if it fails, the
defect is in Tasks 6.5–6.8 — fix it there (with a test that pins the cause) before committing.
To prove the test bites, temporarily change `.when = NotifyWhen::Hidden` to `NotifyWhen::Unfocused`
and confirm the first section fails (`bells.isEmpty()`), then revert.

- [ ] **Step 3: Format**

Run: `clang-format -i src/contour/session/TerminalSession_test.cpp`
Expected: no output.

- [ ] **Step 4: Commit**

```bash
git add src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
session: cover finish notices across split panes, tabs and zoom

Manager-created sessions under a `hidden` / `bell` policy: an unfocused
split pane stays quiet, a background tab and a zoom-covered sibling ring
through the hosting window.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 6.10: Phase gate

**Files:** whatever steps 4–6 change.

- [ ] **Step 1: Build every touched target with zero warnings**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test contour_test contour_gui_test contour`
Expected: success, zero warnings (`/WX`). If `contour`/`contour_gui_test` stop on the pre-existing
`yaml-cpp/emitter.h` break (README), compile each changed `src/contour/**` TU singly
(`ninja -C out/build/clangcl-debug -t commands <obj>` + `cmd /c`), record which, and leave their run to CI.

- [ ] **Step 2: Run the full suite and the spelling check**

Run: `ctest --test-dir out/build/clangcl-debug --output-on-failure`
then `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`
Expected: only the phase-0 baseline failures; spelling PASS (or SKIP as at baseline). Note the
`N tests passed` count for Step 6.

- [ ] **Step 3: Format and hand-audit the phase diff**

Run (Git Bash): `git diff --name-only <PHASE6_BASE>..HEAD -- '*.hpp' '*.cpp' | xargs clang-format --dry-run --Werror`
Expected: no output.
Hand-audit every added line under `src/contour/**` (CI's clang-tidy is the oracle; README notes):
`misc-use-internal-linkage` (test helpers in anonymous namespaces), `misc-const-correctness`,
`bugprone-implicit-widening-of-multiplication-result` (`repeated()`'s `reserve`), no `NOLINT`, no new
`bool` parameter, no Windows macro names. On a Linux box with the TSan tree, also run
`ctest --preset=clang-tsan -R contour_gui_test` for the parser-thread/GUI-thread hop of Tasks 6.7–6.8.

- [ ] **Step 4: `/simplify` over the phase**

Run `/simplify` on `git diff <PHASE6_BASE>..HEAD`. Known candidates to check: the three one-line
`requestWindow*` forwarders; phase 4's `signalNameOf` (Task 4.13, `TerminalSession.cpp`, with its own
`SignalExitBase`/`MaxExitCode`) may become `platform::signalOfExitCode` (Task 6.3), so the tooltip and the
notification share one 128 + n rule. (Truncation and duration text are already shared — `vtbackend::truncateUtf8`
from phase 1, `vtbackend::formatCommandDuration` from phase 4 — so neither should reappear here.)
Rebuild, re-run `contour_test "[notification]"` and `contour_gui_test "[notification]"`, then commit any
fixes:

```bash
git add -u
git commit -F - <<'EOF'
session: simplify the finish-notification path

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(Skip the commit when `/simplify` changes nothing; stage only the files this phase touched.)

- [ ] **Step 5: Code review at xhigh**

Run `/code-review xhigh` on `<PHASE6_BASE>..HEAD` (or dispatch a review subagent with `effort: "xhigh"`),
pointing it at: the C1 threading rule in `commandBlockFinished`; `screenUpdated`'s atomics; the
`const*` helper change in `TerminalSessionManager`; Main.qml's new handlers against the mock; and the
contract deviations listed at the top of this file. Fix every confirmed finding, each with a test.

- [ ] **Step 6: Commit the review fixes with the suite count**

```bash
git add -u
git commit -F - <<'EOF'
session: address review findings on finish notifications

ctest (clangcl-debug): <N> tests passed, <M> failed (phase-0 baseline: <list>).

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

If the review found nothing, make no empty commit; report the pass count to the coordinator instead.

- [ ] **Step 7: Report**

Report to the coordinating session (it records progress; a subagent never does):
the ctest summary line, any target that could only be compiled TU-by-TU, the spelling result, the
review's findings and their fixes, and the contract deviations (top of this file) so phase 9 and 10
pick them up.

---

## Known limitations (recorded, not fixed here)

- **Click-to-focus is Linux-only.** Off Linux a click raises the window; only focusing the pane is Linux-only:
  there the notice is a tray message, and `SystemTrayIcon.messageClicked` carries no identity of the message
  clicked, so there is no pane to route to.
- **Wayland does not report minimising**, so `WindowController::isMinimized()` stays false there and
  `when: hidden` under-notifies for a minimised window (`unfocused`, the default, is unaffected: a minimised
  window is inactive, so its sessions are not focused).
- **Pre-existing, out of scope:** an application's own OSC 777 / OSC 9 notification and a `BEL` from a
  background tab still go through `SessionChrome` and are lost; OSC 777 / OSC 9 are not focus-gated
  (spec §16.2). The window route added in Task 6.5 is what a follow-up can reuse.
- **A daemon client's `clear_on: next`** relies on the mirrored terminal raising `screenUpdated()` when a
  Delta lands; if phase 2's mirror does not, the next command's finish (Task 6.8 path 2) still withdraws it.
- A closing session does not withdraw its outstanding notice (its notifier is destroyed with it); clicking
  it afterwards does nothing.
