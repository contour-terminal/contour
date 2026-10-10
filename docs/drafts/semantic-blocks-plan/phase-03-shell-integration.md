# Phase 3 — Shell integration scripts and the sanitiser

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every bundled shell integration marks where its prompt ends (`OSC 133;B`) and reports the command line (`OSC 133;C;cmdline_url=`) wherever the shell can tell it; PowerShell gets an integration of its own; natively integrating shells (fish ≥ 4.0, Nushell ≥ 0.111) are neither doubled nor misread; and every command line becomes safe to show or to insert through one pure sanitiser (contract C6).

**Architecture:** The scripts stay plain files under `src/contour/cli/shell-integration/`, embedded byte for byte by `EmbedShellIntegration.cmake`. Each script change is proven by running the *embedded* bytes in the real shell on a real PTY (`vtpty::Process`), with every byte the shell writes fed through a `vtbackend::MockTerm` that also answers the shell's terminal queries — the OSC 133 byte stream and the phase-1 `CommandBlockStore` record must agree. A Windows-only probe first settles whether OSC 133 survives ConPTY, and gates the PowerShell task. `sanitizeCommandLine` is a table-driven, byte-wise UTF-8 decoder in `vtbackend/shell/`; `promptKindOf` reads OSC 133;A's `k=` so that continuation prompts (Nushell, kitty) stop opening blocks.

**Tech Stack:** C++23, Catch2, CMake; bash (vendored bash-preexec 0.5.0), zsh, fish, tcsh, PowerShell 7 + PSReadLine; ConPTY/OpenConsole 1.24 on Windows; GitHub Actions.

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §10 (all of it), §16.1 (the ConPTY, fish and Nushell items), §14 (row "Shell scripts"), §13.2, and README Review Focus item 3. Global constraints: [README](README.md#global-constraints). This phase **produces** C6 exactly as written; it **consumes** C1 (`Terminal::commandBlocks()`, `CommandBlockStore::current()/lastFinished()/size()`, `CommandBlockRecord::{id, commandLine, commandLineSource, exitCode}`, `CommandLineSource`, `Terminal::commandBlockAt(LineOffset)`).

**Depends on:** phase 1 (the record assertions and the OSC 133 handler in `Screen.cpp` as phase 1 left it). The scripts themselves depend on nothing.

---

## Verification findings (spec §16.1), established while writing this phase

These settle facts the spec states loosely; tasks below are built on them, and the commit bodies of Tasks 3.1, 3.7 and 3.10 repeat them so that phase 10 (website text, release notes) can quote them.

1. **ConPTY.** Contour does *not* use the inbox pseudoconsole when it can avoid it: `ConPty::ConptyApiImpl` (`src/vtpty/ConPty.cpp:30-101`) loads `conpty.dll` from the executable's directory first, and `src/vtpty/CMakeLists.txt:176-227` downloads and stages `conpty.dll` + `OpenConsole.exe` **1.24.3504.0** (`CONTOUR_EMBED_OPENCONSOLE`, default ON) into `bin/`. Since Windows Terminal 1.22 that ConPTY translates console API calls to VT directly and lets "applications send unmodified VT directly to the terminal" ([WT 1.22 release notes](https://devblogs.microsoft.com/commandline/windows-terminal-preview-1-22-release)); Windows Terminal's own scrollbar marks for pwsh travel through the very same ConPTY ([WT shell integration](https://learn.microsoft.com/en-us/windows/terminal/tutorials/shell-integration)). So passthrough is *expected* — but not yet *measured* with Contour's spawn path. **Task 3.1 measures it, and gates Task 3.9.** The inbox `kernel32` fallback (no `conpty.dll` beside the executable) is not what Contour ships and is not the verdict.
2. **fish marks prompts itself since 4.0.0, not 4.1** (read from fish's source at the release tags and its [release notes](https://fishshell.com/docs/current/relnotes.html)):
   - 4.0.0 — `133;A;special_key=1`, `133;C` (no command line), `133;D;<status>` (`src/reader.rs`, `src/screen.rs` at tag `4.0.0`);
   - 4.0.1 — `133;C;cmdline_url=<escape_string(…, Url)>`: alnum and `/.~-_` kept, every other UTF-8 byte `%XX` upper-case;
   - 4.0.6 — `set -Ua fish_features no-mark-prompt` turns all of it off (`status test-feature mark-prompt` → 0 on, 1 off, 2 unknown);
   - 4.1.0 — `A` carries `click_events=1`;
   - 4.3.0 — `133;B` at the prompt's end (fish `src/terminal.rs` on master: `osc_133_prompt_start/end/command_start/command_finished`);
   - 4.8 — none of it inside Konsole (irrelevant in Contour).
   fish emits `C` *before* firing `fish_preexec` and `D` before `fish_postexec`, so the script must never add a second one. fish counts an OSC sequence inside the prompt as zero-width (`is_osc_escape_seq`, `src/screen.rs`).
3. **Nushell** (verified against `nushell/nushell` `crates/nu-cli/src/prompt_update.rs` and `nushell/reedline` `src/terminal_extensions/semantic_prompt.rs`): with `$env.config.shell_integration.osc133` (default `true`) it sends `133;A;k=i;click_events=1` before the primary prompt, `133;P;k=r` before the right prompt, **`133;A;k=s;click_events=1` before every continuation line**, `133;B` at input start, a bare `133;C` (no `cmdline_url`) and `133;D;<exit>`. The `k=` markers and click events landed in **0.111.0** ([release notes](https://www.nushell.sh/blog/2026-02-28-nushell_v0_111_0.html), #17468, #17491). Contour reads every `133;A` as a new prompt today (`Screen.cpp:6782-6794`), so a multi-line Nushell command would be split into blocks — **Task 3.4 fixes that**. The command line is recovered from the grid (phase 1), because Nushell never reports it.
4. **PowerShell's `C`**: Windows Terminal's documented pwsh prompt emits `D`, `A`, `9;9`, `B` and **no `C` at all**; there is no documented Enter-key handler. An Enter handler would also run *before* PSReadLine writes the newline, putting `OutputStart` on the prompt line. This phase therefore wraps **`PSConsoleHostReadLine`** and emits `C` after it returns — after the newline — exactly as VS Code's `shellIntegration.ps1` does (`microsoft/vscode`, `src/vs/workbench/contrib/terminal/common/scripts/shellIntegration.ps1`). This is a deliberate deviation from spec §10.3's wording; the behaviour the spec asks for (command line with `C`, before the command runs) is unchanged.
5. **tcsh** drops a `%{…%}` literal that nothing follows ("we lose the last literal, if it is not followed by a normal character", `RefreshPromptpart()` in tcsh's `ed.refresh.c`), so `B` cannot simply be appended to `prompt` — Task 3.8's step-over construction.
6. **`cmdline_url` decoding**: `Screen.cpp:6820-6821` calls `core::unescapeURL` (`vendor/core-cpp/src/core/Utils.hpp:460-483`), byte-wise `%XX`, either hex case, malformed escapes kept literally; values are split on `;` first (`core::forEachKeyValue`), so every encoder must encode `;`. Confirmed by Task 3.3.

## Contract notes

- **C6 is produced exactly as written.** Unstated details, fixed here and tested: DEL (U+007F) is treated as a C0 control for both purposes; `Insert` replaces bidi controls with U+FFFD (as `Display` does) rather than dropping them, so the user sees where one was; ill-formed UTF-8 becomes one U+FFFD per maximal subpart for **both** purposes; nothing is truncated.
- **Additions** (not in the README contract; owned by this phase):
  - `src/vtbackend/vt/SemanticPrompt.hpp`: `enum class PromptKind : uint8_t { Initial = 0, Continuation, Right };` and `[[nodiscard]] PromptKind promptKindOf(std::string_view parameters) noexcept;`
  - `src/contour/cli/ShellIntegration.hpp`: `struct NativeShellIntegrationRow { std::string_view name; std::string_view displayName; std::string_view howToEnable; };`, `[[nodiscard]] std::span<NativeShellIntegrationRow const> nativeShellIntegrations() noexcept;`, `[[nodiscard]] std::string_view nativeShellsText();`, `[[nodiscard]] std::string unsupportedShellMessage(std::string_view shell);`
- The shell name is `pwsh`. The CLI verb that installs any integration is `contour generate integration shell <SHELL> to <FILE>` (here `contour generate integration shell pwsh to FILE`); there is no separate shell-integration verb, so every place that names the spec's wording uses this one.

## Before Task 3.1

Run: `git rev-parse HEAD` and note it as `PHASE3_START` (the phase gate diffs against it).

---

### Task 3.1: Does OSC 133 survive ConPTY? (verification gate)

**Files:**
- Create: `src/vtpty/test/conpty_osc133_probe.cpp`
- Modify: `src/vtpty/CMakeLists.txt` (the `if(WIN32)` branch inside `if(VTPTY_TESTING)`, lines 148-153)
- Modify: `src/vtpty/ConPty_test.cpp`

**Interfaces:**
- Consumes: `vtpty::Process(ExecInfo const&, std::unique_ptr<Pty>, bool escapeSandbox, std::shared_ptr<ProcessPlacement>)` (`Process.hpp:62-77`), `vtpty::createPty(PageSize, std::optional<ImageSize>)` (`Pty.hpp:156`), `vtpty::NoPlacement` (`ProcessPlacement.hpp:87`), `vtpty::testing::readUntil(Pty&, std::function<bool(std::string_view)> const&)` (`src/vtpty/test/PtyReading.hpp`), `core::escape(std::string_view)`.
- Produces: the test `ConPty.passesOsc133Through` (`[vtpty][conpty][osc133]`) and the executable target `conpty_osc133_probe` (Windows only). No production API.

This task has no implementation step: the component under test is ConPTY itself, and the test *is* the deliverable. Its outcome decides Task 3.9.

- [ ] **Step 1: Write the probe program**

Create `src/vtpty/test/conpty_osc133_probe.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The child half of ConPty.passesOsc133Through: a console program that writes OSC 133 marks the way
// a shell integration does -- prompt start and end, command start with its command line, command
// end -- followed by a sentinel, and keeps the pseudoconsole open long enough to be read.

#include <chrono>
#include <string_view>
#include <thread>

#include <Windows.h>

int main()
{
    auto* const output = GetStdHandle(STD_OUTPUT_HANDLE);

    // A console program sees escape sequences interpreted only once it asks for that, as every
    // shell does; without it the console would print ESC as a glyph and pass nothing through.
    auto mode = DWORD {};
    GetConsoleMode(output, &mode);
    SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    constexpr auto Probe = std::string_view { "\x1b]133;A\x1b\\prompt> \x1b]133;B\x1b\\\r\n"
                                              "\x1b]133;C;cmdline_url=echo%20hi\x1b\\hi\r\n"
                                              "\x1b]133;D;0\x1b\\contour-probe-done\r\n" };
    auto written = DWORD {};
    WriteFile(output, Probe.data(), static_cast<DWORD>(Probe.size()), &written, nullptr);

    // ConPTY renders asynchronously; a child that exits at once can take its last frame with it.
    std::this_thread::sleep_for(std::chrono::seconds { 3 });
    return 0;
}
```

- [ ] **Step 2: Register it**

In `src/vtpty/CMakeLists.txt` replace

```cmake
    if(WIN32)
        # ConPty is the Windows-only PTY backend.
        target_sources(vtpty_test PRIVATE ConPty_test.cpp)
    else()
```

with

```cmake
    if(WIN32)
        # ConPty is the Windows-only PTY backend.
        target_sources(vtpty_test PRIVATE ConPty_test.cpp)

        # The child half of ConPty.passesOsc133Through: a console program writing OSC 133 marks the way
        # a shell integration does, so the test can see whether they survive the pseudoconsole.
        add_executable(conpty_osc133_probe test/conpty_osc133_probe.cpp)
        add_dependencies(vtpty_test conpty_osc133_probe)
        target_compile_definitions(vtpty_test PRIVATE
            VTPTY_CONPTY_OSC133_PROBE="$<TARGET_FILE:conpty_osc133_probe>")
    else()
```

- [ ] **Step 3: Write the test**

In `src/vtpty/ConPty_test.cpp` replace the include block and the anonymous namespace (lines 1-19) with:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using vtpty::ColumnCount;
using vtpty::ConPty;
using vtpty::LineCount;
using vtpty::PageSize;

namespace
{

constexpr PageSize pageSize(int columns, int lines) noexcept
{
    return PageSize { .lines = LineCount(lines), .columns = ColumnCount(columns) };
}

/// What a terminal answers to the two queries a pseudoconsole may send it at startup: primary
/// device attributes and the cursor position. An unanswered one can hold ConPTY's output back.
constexpr auto TerminalAnswers = std::array {
    std::pair { std::string_view { "\x1b[c" }, std::string_view { "\x1b[?61;4c" } },
    std::pair { std::string_view { "\x1b[6n" }, std::string_view { "\x1b[1;1R" } },
};

} // namespace
```

and append at the end of the file:

```cpp
// Whether an OSC 133 mark a console program writes reaches the terminal on the far side of the
// pseudoconsole -- Contour's PowerShell integration depends on it (docs/drafts/semantic-blocks.md
// §10.3, §16.1). ConPTY does not forward the child's bytes as they are: it interprets them into its
// console buffer, and a mark survives only if ConPTY passes it on. The ConPTY under test is the one
// Contour ships beside its executable (CONTOUR_EMBED_OPENCONSOLE); without it, the system's own.
TEST_CASE("ConPty.passesOsc133Through", "[vtpty][conpty][osc133]")
{
    auto probe = std::filesystem::path { VTPTY_CONPTY_OSC133_PROBE };
    probe.make_preferred();
    INFO("bundled conpty.dll beside the test: " << std::filesystem::exists(probe.parent_path() / "conpty.dll"));

    auto process = vtpty::Process {
        vtpty::Process::ExecInfo { .program = probe.string(), .arguments = {}, .workingDirectory = {}, .env = {} },
        vtpty::createPty(pageSize(80, 24), std::nullopt),
        /*escapeSandbox=*/false,
        std::make_shared<vtpty::NoPlacement>()
    };
    REQUIRE(process.start().has_value());

    // A read on ConPTY blocks until the pseudoconsole has something to say. Ending the child is what
    // makes it return, should the probe never print its sentinel.
    auto const watchdog = std::jthread { [&process](std::stop_token const& stop) {
        auto mutex = std::mutex {};
        auto lock = std::unique_lock { mutex };
        auto wakeup = std::condition_variable_any {};
        std::ignore = wakeup.wait_for(lock, stop, std::chrono::seconds { 30 }, [] { return false; });
        if (!stop.stop_requested())
            process.terminate(vtpty::Process::TerminationHint::Normal);
    } };

    auto answered = std::vector<std::string_view> {};
    auto const output = vtpty::testing::readUntil(process, [&](std::string_view text) {
        for (auto const& [query, answer]: TerminalAnswers)
        {
            if (text.contains(query) && !std::ranges::contains(answered, query))
            {
                answered.push_back(query);
                std::ignore = process.write(answer);
            }
        }
        return text.contains("contour-probe-done");
    });
    INFO("pseudoconsole output: " << core::escape(output));

    // The probe ran, and ConPTY rendered what it printed ...
    REQUIRE(output.contains("contour-probe-done"));

    // ... and passed every mark on, parameters included. Only the introducer and payload are checked:
    // ConPTY may re-terminate a sequence with BEL instead of ST.
    CHECK(output.contains("\x1b]133;A"));
    CHECK(output.contains("\x1b]133;B"));
    CHECK(output.contains("\x1b]133;C;cmdline_url=echo%20hi"));
    CHECK(output.contains("\x1b]133;D;0"));

    process.terminate(vtpty::Process::TerminationHint::Normal);
    std::ignore = process.wait();
}
```

- [ ] **Step 4: Build and run**

Run: `cmake --build --preset clangcl-debug --target vtpty_test` then `out/build/clangcl-debug/bin/vtpty_test.exe "ConPty.passesOsc133Through"`
Expected: `All tests passed (6 assertions in 1 test case)`.

- [ ] **Step 5: Apply the decision rule (spec §16.1)**

- **PASS** → continue with Step 6; Task 3.9 (PowerShell) goes ahead.
- **The `REQUIRE` on `contour-probe-done` fails** → the probe did not run or ConPTY printed nothing; that is a harness problem, not the verdict. Check that `conpty_osc133_probe.exe` sits in `out/build/clangcl-debug/bin/` and that `INFO` reports `conpty.dll` present (if not: reconfigure with `-DCONTOUR_EMBED_OPENCONSOLE=ON`, rebuild `vtpty`), then rerun Step 4.
- **The `REQUIRE` passes and any `133` `CHECK` fails, with the bundled `conpty.dll` present** → ConPTY swallows OSC 133. **Do not commit.** Send the coordinator the test's full `INFO` output and the OpenConsole version (`src/vtpty/CMakeLists.txt`, `OPENCONSOLE_VERSION`). Task 3.9 is **held back**; Tasks 3.10 and 3.11 then skip their PowerShell parts as they say. Continue with Task 3.2.

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/vtpty/ConPty_test.cpp src/vtpty/test/conpty_osc133_probe.cpp`

```bash
git add src/vtpty/test/conpty_osc133_probe.cpp src/vtpty/CMakeLists.txt src/vtpty/ConPty_test.cpp
git commit -F - <<'EOF'
vtpty: probe that OSC 133 survives ConPTY

Spec §16.1 verification: OSC 133 A, B, C (with cmdline_url) and D, written
by a console program with VT processing enabled, reach the master side of
the ConPTY Contour bundles (OpenConsole 1.24.3504.0) intact. The PowerShell
integration can therefore ship.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.2: The command-line sanitiser (contract C6)

**Files:**
- Create: `src/vtbackend/shell/CommandLineSanitizer.hpp`
- Create: `src/vtbackend/shell/CommandLineSanitizer.cpp`
- Create: `src/vtbackend/shell/CommandLineSanitizer_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (`vtbackend_HEADERS`, `vtbackend_SOURCES`, `vtbackend_test` sources)

**Interfaces:**
- Consumes: `core::views::enumerate` (`vendor/core-cpp/src/core/Utils.hpp:142-152`), `unicode::convert_to<char>(char32_t)` (tests only).
- Produces (C6, exactly):

```cpp
namespace vtbackend {
enum class SanitizePurpose : uint8_t { Display = 0, Insert };
[[nodiscard]] std::string sanitizeCommandLine(std::string_view text, SanitizePurpose purpose);
}
```

- [ ] **Step 1: Write the failing test**

Create `src/vtbackend/shell/CommandLineSanitizer_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// A command line arrives from whatever wrote to the PTY (spec §10.4, README Review Focus 3). These
// pin what survives being shown (Display) and being typed back into a shell (Insert).

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <libunicode/convert.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

using vtbackend::SanitizePurpose;
using vtbackend::sanitizeCommandLine;

namespace
{

/// U+FFFD REPLACEMENT CHARACTER, UTF-8 encoded.
constexpr auto Fffd = std::string_view { "\xEF\xBF\xBD" };

/// Every purpose, for the checks that hold for all of them.
constexpr auto Purposes = std::array { SanitizePurpose::Display, SanitizePurpose::Insert };

/// @return @p codePoint, UTF-8 encoded.
[[nodiscard]] std::string utf8(char32_t codePoint)
{
    return unicode::convert_to<char>(codePoint);
}

/// @return @p pattern with every '#' standing for U+FFFD.
[[nodiscard]] std::string withReplacements(std::string_view pattern)
{
    auto result = std::string {};
    for (auto const ch: pattern)
    {
        if (ch == '#')
            result += Fffd;
        else
            result += ch;
    }
    return result;
}

} // namespace

TEST_CASE("sanitizeCommandLine keeps ordinary text as it is", "[sanitize]")
{
    auto const text = std::string_view { "git commit -m \"f\xc3\xbc\xc3\xbc\" && echo \xe2\x82\xac \xf0\x9f\x98\x80 ~/src" };
    for (auto const purpose: Purposes)
    {
        CHECK(sanitizeCommandLine(text, purpose) == text);
        CHECK(sanitizeCommandLine("", purpose).empty());
    }
}

TEST_CASE("sanitizeCommandLine replaces C0 controls for display and drops them for insertion", "[sanitize]")
{
    for (auto const code: std::views::iota(0x00, 0x20))
    {
        auto const control = static_cast<char>(code);
        if (control == '\t' || control == '\n')
            continue;
        INFO(std::format("C0 control 0x{:02X}", code));
        auto const text = std::string { "a" } + control + "b";
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Display) == std::format("a{}b", Fffd));
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Insert) == "ab");
    }
}

TEST_CASE("sanitizeCommandLine keeps TAB and LF for insertion only", "[sanitize]")
{
    auto const text = std::string_view { "for x in a b\n\tdo echo $x\ndone" };
    CHECK(sanitizeCommandLine(text, SanitizePurpose::Insert) == text);
    CHECK(sanitizeCommandLine(text, SanitizePurpose::Display)
          == std::format("for x in a b{0}{0}do echo $x{0}done", Fffd));
}

TEST_CASE("sanitizeCommandLine treats DEL as a control", "[sanitize]")
{
    CHECK(sanitizeCommandLine("a\x7f" "b", SanitizePurpose::Display) == std::format("a{}b", Fffd));
    CHECK(sanitizeCommandLine("a\x7f" "b", SanitizePurpose::Insert) == "ab");
}

TEST_CASE("sanitizeCommandLine replaces C1 controls for display and drops them for insertion", "[sanitize]")
{
    for (auto const code: std::views::iota(0x80U, 0xA0U))
    {
        INFO(std::format("C1 control U+{:04X}", code));
        auto const text = "a" + utf8(static_cast<char32_t>(code)) + "b";
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Display) == std::format("a{}b", Fffd));
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Insert) == "ab");
    }

    // Their neighbours are ordinary text: '~' below, NO-BREAK SPACE above.
    for (auto const codePoint: std::array<char32_t, 2> { 0x7E, 0xA0 })
    {
        auto const text = "a" + utf8(codePoint) + "b";
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(text, purpose) == text);
    }
}

TEST_CASE("sanitizeCommandLine replaces bidi embeddings, overrides and isolates for either purpose",
          "[sanitize]")
{
    constexpr auto BidiControls =
        std::array<char32_t, 9> { 0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069 };
    for (auto const codePoint: BidiControls)
    {
        INFO(std::format("U+{:04X}", static_cast<uint32_t>(codePoint)));
        auto const text = "a" + utf8(codePoint) + "b";
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(text, purpose) == std::format("a{}b", Fffd));
    }

    // The code points either side of both ranges are ordinary text.
    for (auto const codePoint: std::array<char32_t, 4> { 0x2029, 0x202F, 0x2065, 0x206A })
    {
        INFO(std::format("U+{:04X}", static_cast<uint32_t>(codePoint)));
        auto const text = "a" + utf8(codePoint) + "b";
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(text, purpose) == text);
    }
}

TEST_CASE("sanitizeCommandLine replaces each maximal subpart of ill-formed UTF-8 once", "[sanitize]")
{
    struct Case
    {
        std::string_view input;
        std::string_view expected; ///< '#' stands for U+FFFD.
        std::string_view what;
    };
    constexpr auto Cases = std::array {
        // The Unicode Standard's own example (§3.9, Table 3-8, "U+FFFD for maximal subparts").
        Case { "\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64", "a###b#c##d", "Table 3-8" },
        Case { "\xC0\x80", "##", "overlong NUL" },
        Case { "\xE0\x80\x80", "###", "overlong three-byte form" },
        Case { "\xED\xA0\x80", "###", "a UTF-16 surrogate" },
        Case { "\xF4\x90\x80\x80", "####", "beyond U+10FFFF" },
        Case { "\xF5\x80", "##", "F5 never leads" },
        Case { "\xFF", "#", "FF never leads" },
        Case { "x\xE2\x82", "x#", "cut off by the end" },
        Case { "\xE2\x82x", "#x", "cut off by an ASCII byte" },
        Case { "\x80", "#", "a lone continuation byte" },
        Case { "\x9B" "31m", "#31m", "a raw 8-bit CSI" },
    };
    for (auto const& [input, expected, what]: Cases)
    {
        INFO(what);
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(input, purpose) == withReplacements(expected));
    }
}

TEST_CASE("sanitizeCommandLine for insertion cannot end a bracketed paste", "[sanitize]")
{
    // A crafted cmdline_url carrying the bracketed-paste end marker would end the paste early and
    // have the shell run the remainder; without its ESC the marker is inert text.
    CHECK(sanitizeCommandLine("ls\x1b[201~rm -rf ~", SanitizePurpose::Insert) == "ls[201~rm -rf ~");

    // Nor do the 8-bit forms get through: U+009B (C1 CSI) is dropped, a raw 0x9B byte is ill-formed.
    CHECK(sanitizeCommandLine("ls\xC2\x9B" "201~x", SanitizePurpose::Insert) == "ls201~x");
    CHECK(sanitizeCommandLine("ls\x9B" "201~x", SanitizePurpose::Insert) == std::format("ls{}201~x", Fffd));

    CHECK(sanitizeCommandLine("\x1b]0;pwned\x07" "ok", SanitizePurpose::Insert) == "]0;pwnedok");
}

TEST_CASE("sanitizeCommandLine for display shows where each control was", "[sanitize]")
{
    CHECK(sanitizeCommandLine("ls\x1b[201~", SanitizePurpose::Display) == std::format("ls{}[201~", Fffd));
}

TEST_CASE("sanitizeCommandLine never truncates", "[sanitize]")
{
    // Bounding a command line is the store's business (MaxRecordedCommandLineBytes), not this
    // function's: 64 KiB in, every byte accounted for out.
    constexpr auto Length = size_t { 64 } * 1024;
    auto const plain = std::string(Length, 'x');
    auto const escapes = std::string(Length, '\x1b');
    for (auto const purpose: Purposes)
        CHECK(sanitizeCommandLine(plain, purpose) == plain);
    CHECK(sanitizeCommandLine(escapes, SanitizePurpose::Display).size() == Length * Fffd.size());
    CHECK(sanitizeCommandLine(escapes, SanitizePurpose::Insert).empty());
}

TEST_CASE("sanitizeCommandLine output is final", "[sanitize]")
{
    // What comes out is well-formed and free of everything the purpose forbids, so a second pass
    // changes nothing: a caller that sanitises twice cannot double-replace.
    auto const hostile =
        std::string { "a\x1b[31m\xC2\x9B\x7f\t\n\xE2\x80\xAE" "b\xF0\x9F\x98" "c\xFF" };
    for (auto const purpose: Purposes)
    {
        auto const once = sanitizeCommandLine(hostile, purpose);
        CHECK(sanitizeCommandLine(once, purpose) == once);
    }
}
```

- [ ] **Step 2: Register the test**

In `src/vtbackend/CMakeLists.txt`, in the `add_executable(vtbackend_test …)` list, add the line `        shell/CommandLineSanitizer_test.cpp` directly after `        shell/CommandBlocks_test.cpp`.

- [ ] **Step 3: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `fatal error C1083: Cannot open include file: 'vtbackend/shell/CommandLineSanitizer.hpp'`.

- [ ] **Step 4: Declare the sanitiser**

Create `src/vtbackend/shell/CommandLineSanitizer.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vtbackend
{

/// What a sanitised command line is for, which decides how much of it may survive.
enum class SanitizePurpose : uint8_t
{
    /// Shown to the user: a notification, a picker row, a tooltip, the sticky header.
    Display = 0,
    /// Typed back into a shell prompt through the paste path (the recent-commands picker).
    Insert,
};

/// Makes a command line reported by a shell safe to show, or to type back into a shell.
///
/// A command line arrives from whatever wrote to the PTY, so it is untrusted (spec §10.4).
/// Display: C0/C1 controls and bidi overrides → U+FFFD; Insert: additionally drops ESC and every C0
/// except TAB and LF, and every C1.
///
/// In detail: DEL counts as a C0 control. The bidi controls are the embeddings, overrides and
/// isolates (U+202A..U+202E, U+2066..U+2069); Insert, too, replaces rather than drops them, so the
/// user sees where one was. Ill-formed UTF-8 becomes one U+FFFD per maximal subpart, for either
/// purpose. Nothing is truncated: bounding the length is the caller's business.
/// @param text The raw command line, as reported or recovered.
/// @param purpose What the result is for.
/// @return The sanitised command line, well-formed UTF-8.
[[nodiscard]] std::string sanitizeCommandLine(std::string_view text, SanitizePurpose purpose);

} // namespace vtbackend
```

- [ ] **Step 5: Implement it**

Create `src/vtbackend/shell/CommandLineSanitizer.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace vtbackend
{

namespace
{
    /// What becomes of one code point.
    enum class Treatment : uint8_t
    {
        Keep = 0,
        Replace, ///< Becomes U+FFFD, so the reader sees that something was there.
        Drop,
    };

    /// A run of code points not kept as they are, and what each purpose does with them.
    struct Rule
    {
        char32_t first;
        char32_t last;
        std::array<Treatment, 2> treatment; ///< Indexed by SanitizePurpose: Display, Insert.
    };

    /// Every code point not kept as it is; anything not listed is kept. A new purpose is a new column.
    constexpr auto Rules = std::array {
        Rule { .first = 0x00, .last = 0x08, .treatment = { Treatment::Replace, Treatment::Drop } }, // C0 < TAB
        Rule { .first = 0x09, .last = 0x09, .treatment = { Treatment::Replace, Treatment::Keep } }, // TAB
        Rule { .first = 0x0A, .last = 0x0A, .treatment = { Treatment::Replace, Treatment::Keep } }, // LF
        Rule { .first = 0x0B, .last = 0x1F, .treatment = { Treatment::Replace, Treatment::Drop } }, // ESC & co.
        Rule { .first = 0x7F, .last = 0x7F, .treatment = { Treatment::Replace, Treatment::Drop } }, // DEL
        Rule { .first = 0x80, .last = 0x9F, .treatment = { Treatment::Replace, Treatment::Drop } }, // C1
        Rule { .first = 0x202A, .last = 0x202E, .treatment = { Treatment::Replace, Treatment::Replace } },
        Rule { .first = 0x2066, .last = 0x2069, .treatment = { Treatment::Replace, Treatment::Replace } },
    };

    /// A well-formed UTF-8 lead byte (Unicode §3.9, Table 3-7) and what may follow it.
    struct LeadByte
    {
        uint8_t first;
        uint8_t last;
        uint8_t length;      ///< Bytes in the whole sequence.
        uint8_t payloadMask; ///< The lead byte's share of the code point.
        uint8_t secondMin;   ///< The second byte's range, narrowed against overlongs, surrogates and
        uint8_t secondMax;   ///< code points beyond U+10FFFF; every later byte is 0x80..0xBF.
    };

    constexpr auto LeadBytes = std::array {
        LeadByte { .first = 0xC2, .last = 0xDF, .length = 2, .payloadMask = 0x1F, .secondMin = 0x80, .secondMax = 0xBF },
        LeadByte { .first = 0xE0, .last = 0xE0, .length = 3, .payloadMask = 0x0F, .secondMin = 0xA0, .secondMax = 0xBF },
        LeadByte { .first = 0xE1, .last = 0xEC, .length = 3, .payloadMask = 0x0F, .secondMin = 0x80, .secondMax = 0xBF },
        LeadByte { .first = 0xED, .last = 0xED, .length = 3, .payloadMask = 0x0F, .secondMin = 0x80, .secondMax = 0x9F },
        LeadByte { .first = 0xEE, .last = 0xEF, .length = 3, .payloadMask = 0x0F, .secondMin = 0x80, .secondMax = 0xBF },
        LeadByte { .first = 0xF0, .last = 0xF0, .length = 4, .payloadMask = 0x07, .secondMin = 0x90, .secondMax = 0xBF },
        LeadByte { .first = 0xF1, .last = 0xF3, .length = 4, .payloadMask = 0x07, .secondMin = 0x80, .secondMax = 0xBF },
        LeadByte { .first = 0xF4, .last = 0xF4, .length = 4, .payloadMask = 0x07, .secondMin = 0x80, .secondMax = 0x8F },
    };

    /// A multi-byte sequence part-way through decoding.
    struct Partial
    {
        size_t start;       ///< Offset of its lead byte.
        char32_t codePoint; ///< The bits gathered so far.
        uint8_t remaining;  ///< Continuation bytes still to come.
        uint8_t nextMin;    ///< The range the next one must fall into.
        uint8_t nextMax;
    };

    constexpr auto ReplacementCharacter = std::string_view { "\xEF\xBF\xBD" };

    /// @return What @p purpose does with @p codePoint.
    [[nodiscard]] constexpr Treatment treatmentOf(char32_t codePoint, SanitizePurpose purpose) noexcept
    {
        auto const rule = std::ranges::find_if(
            Rules, [codePoint](Rule const& candidate) { return candidate.first <= codePoint && codePoint <= candidate.last; });
        return rule != Rules.end() ? rule->treatment[std::to_underlying(purpose)] : Treatment::Keep;
    }

    /// @return The rule for @p codeUnit as a lead byte, or nullopt when it can never lead.
    [[nodiscard]] constexpr std::optional<LeadByte> leadByteOf(uint8_t codeUnit) noexcept
    {
        auto const rule = std::ranges::find_if(LeadBytes, [codeUnit](LeadByte const& candidate) {
            return candidate.first <= codeUnit && codeUnit <= candidate.last;
        });
        if (rule == LeadBytes.end())
            return std::nullopt;
        return *rule;
    }
} // namespace

std::string sanitizeCommandLine(std::string_view text, SanitizePurpose purpose)
{
    auto out = std::string {};
    out.reserve(text.size());

    auto const emit = [&](char32_t codePoint, std::string_view bytes) {
        switch (treatmentOf(codePoint, purpose))
        {
            case Treatment::Keep: out += bytes; break;
            case Treatment::Replace: out += ReplacementCharacter; break;
            case Treatment::Drop: break;
        }
    };

    // Decoded one code unit at a time, so that a broken sequence costs exactly its maximal subpart
    // (Unicode §3.9, "U+FFFD Substitution of Maximal Subparts") and the unit that broke it starts afresh.
    auto partial = std::optional<Partial> {};
    for (auto const [offset, ch]: core::views::enumerate(text))
    {
        auto const codeUnit = static_cast<uint8_t>(ch);
        if (partial)
        {
            if (partial->nextMin <= codeUnit && codeUnit <= partial->nextMax)
            {
                partial->codePoint = static_cast<char32_t>((partial->codePoint << 6U) | (codeUnit & 0x3FU));
                partial->nextMin = 0x80;
                partial->nextMax = 0xBF;
                --partial->remaining;
                if (partial->remaining == 0)
                {
                    emit(partial->codePoint, text.substr(partial->start, offset + 1 - partial->start));
                    partial.reset();
                }
                continue;
            }
            out += ReplacementCharacter;
            partial.reset();
        }

        if (codeUnit < 0x80)
            emit(codeUnit, text.substr(offset, 1));
        else if (auto const lead = leadByteOf(codeUnit))
            partial = Partial { .start = offset,
                                .codePoint = static_cast<char32_t>(codeUnit & lead->payloadMask),
                                .remaining = static_cast<uint8_t>(lead->length - 1),
                                .nextMin = lead->secondMin,
                                .nextMax = lead->secondMax };
        else
            out += ReplacementCharacter;
    }
    if (partial)
        out += ReplacementCharacter;

    return out;
}

} // namespace vtbackend
```

In `src/vtbackend/CMakeLists.txt` add `    shell/CommandLineSanitizer.hpp` directly after `    shell/CommandBlocks.hpp` (in `vtbackend_HEADERS`) and `    shell/CommandLineSanitizer.cpp` directly after `    shell/CommandBlocks.cpp` (in `vtbackend_SOURCES`).

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[sanitize]"`
Expected: `All tests passed` (11 test cases), build free of warnings.

- [ ] **Step 7: Format and commit**

Run: `clang-format -i src/vtbackend/shell/CommandLineSanitizer.hpp src/vtbackend/shell/CommandLineSanitizer.cpp src/vtbackend/shell/CommandLineSanitizer_test.cpp`

```bash
git add src/vtbackend/shell/CommandLineSanitizer.hpp src/vtbackend/shell/CommandLineSanitizer.cpp \
        src/vtbackend/shell/CommandLineSanitizer_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: sanitise command lines for display and for insertion

A command line arrives from whatever wrote to the PTY. Display replaces C0,
DEL, C1 and the bidi embedding/override/isolate controls with U+FFFD; Insert
keeps TAB and LF, drops ESC and the other controls -- so a crafted ESC [201~
cannot end a bracketed paste early -- and still marks bidi controls. Ill-formed
UTF-8 becomes one U+FFFD per maximal subpart. Table-driven, never truncates.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---
### Task 3.3: `cmdline_url` round-trips a multi-byte command line (characterization)

**Files:**
- Create: `src/vtbackend/shell/Osc133Reporting_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (`vtbackend_test` sources)

**Interfaces:**
- Consumes: C1 `Terminal::commandBlocks()`, `CommandBlockStore::current()`, `CommandBlockRecord::{commandLine, commandLineSource}`, `CommandLineSource::Reported`; `MockTerm` (`src/vtbackend/testing/MockTerm.hpp`).
- Produces: tests only.

The parser already decodes `cmdline_url` (`Screen.cpp:6817-6822` → `core::unescapeURL`); what is missing is a test that a command line encoded the way this phase's scripts encode it — one `%XX` per UTF-8 byte — reaches the record intact. The test is expected to pass at once; if it does not, phase 1 changed the decoding, and that is reported, not patched here.

- [ ] **Step 1: Write the test**

Create `src/vtbackend/shell/Osc133Reporting_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// OSC 133 as the shells actually send it -- the bundled integrations of
// src/contour/cli/shell-integration/ and the native ones of fish and Nushell -- read right.

#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace vtbackend;

namespace
{

/// @return The command line the record holds once @p encoded was reported with OSC 133;C, after a prompt.
[[nodiscard]] std::string reportedCommandLine(std::string_view encoded)
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mock.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\\r\n");
    mock.writeToScreen(std::string { "\033]133;C;cmdline_url=" } + std::string { encoded } + "\033\\");

    auto const* const record = mock.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLineSource == CommandLineSource::Reported);
    return record->commandLine;
}

} // namespace

TEST_CASE("cmdline_url decodes a command line encoded byte by byte", "[osc133][cmdline_url]")
{
    // One %XX per UTF-8 byte, upper-case hex: what fish, and this phase's bash, zsh and pwsh, send.
    CHECK(reportedCommandLine("echo%20%22a%20%20b%22%20%C3%BC%E2%82%AC%F0%9F%98%80")
          == "echo \"a  b\" \xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80");
    CHECK(reportedCommandLine("%c3%bc") == "\xc3\xbc"); // lower-case hex decodes as well
}

TEST_CASE("cmdline_url carries what only percent-encoding can", "[osc133][cmdline_url]")
{
    // ';' would end the key/value pair and ESC or BEL the sequence; encoded, they arrive. The record
    // keeps them RAW -- sanitising is per use (contract C6).
    CHECK(reportedCommandLine("a%3Bb%3Dc") == "a;b=c");
    CHECK(reportedCommandLine("x%1B%5B201~") == "x\x1b[201~");
    CHECK(reportedCommandLine("50%25") == "50%");
    CHECK(reportedCommandLine("l1%0Al2") == "l1\nl2");
}

TEST_CASE("cmdline_url keeps a broken escape as it came", "[osc133][cmdline_url]")
{
    CHECK(reportedCommandLine("100%") == "100%");
    CHECK(reportedCommandLine("%zz") == "%zz");
}
```

- [ ] **Step 2: Register it**

In `src/vtbackend/CMakeLists.txt`, in the `vtbackend_test` list, add `        shell/Osc133Reporting_test.cpp` directly after `        shell/PromptRegion_test.cpp`.

- [ ] **Step 3: Run it**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[cmdline_url]"`
Expected: `All tests passed` (3 test cases). If any check fails, stop and report the failing values to the coordinator: the decoding in `Screen.cpp`'s `case 'C'` changed in phase 1.

- [ ] **Step 4: Format and commit**

Run: `clang-format -i src/vtbackend/shell/Osc133Reporting_test.cpp`

```bash
git add src/vtbackend/shell/Osc133Reporting_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: pin cmdline_url decoding of byte-wise encoded command lines

The shell integrations percent-encode every UTF-8 byte that is not
unreserved, ';' among them; the record must get back exactly what was typed.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.4: Continuation and right-hand prompts do not open a block

**Files:**
- Create: `src/vtbackend/vt/SemanticPrompt.hpp`
- Create: `src/vtbackend/vt/SemanticPrompt.cpp`
- Create: `src/vtbackend/vt/SemanticPrompt_test.cpp`
- Modify: `src/vtbackend/screen/Screen.cpp` (includes, lines 4-21; `case 'A':` of the OSC 133 handler, line 6782 at `c74b93ad`)
- Modify: `src/vtbackend/shell/Osc133Reporting_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt`

**Interfaces:**
- Consumes: `core::forEachKeyValue` / `core::ForEachKeyValueParams` (`Utils.hpp:485-512`); C1 `Terminal::commandBlockAt(LineOffset)`, `CommandBlockStore::lastFinished()`, `CommandBlockRecord::{id, exitCode}`.
- Produces (addition; lives in `vt/`, which includes nothing above it):

```cpp
namespace vtbackend {
enum class PromptKind : uint8_t { Initial = 0, Continuation, Right };
[[nodiscard]] PromptKind promptKindOf(std::string_view parameters) noexcept;
}
```

Why (finding 3): reedline — Nushell's line editor — sends `133;A;k=s;click_events=1` before every further line of a multi-line command, and kitty's integrations do the same for `PS2`. Read as a new prompt, each one marks a head row (`setMark()`), discards the block being typed and mints a new one.

- [ ] **Step 1: Write the failing tests**

Create `src/vtbackend/vt/SemanticPrompt_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SemanticPrompt.hpp>

#include <catch2/catch_test_macros.hpp>

using vtbackend::PromptKind;
using vtbackend::promptKindOf;

TEST_CASE("promptKindOf reads OSC 133;A's k= parameter", "[osc133]")
{
    // The parameters as Screen hands them over: everything after the "A", leading ';' included.
    CHECK(promptKindOf("") == PromptKind::Initial);
    CHECK(promptKindOf(";click_events=1") == PromptKind::Initial);
    CHECK(promptKindOf(";k=i;click_events=1") == PromptKind::Initial);
    CHECK(promptKindOf(";k=s;click_events=1") == PromptKind::Continuation);
    CHECK(promptKindOf(";k=c") == PromptKind::Continuation);
    CHECK(promptKindOf(";aid=7;k=r") == PromptKind::Right);
}

TEST_CASE("promptKindOf reads an unknown or empty kind as a new prompt", "[osc133]")
{
    CHECK(promptKindOf(";k=zz") == PromptKind::Initial);
    CHECK(promptKindOf(";k") == PromptKind::Initial);
    CHECK(promptKindOf(";k=") == PromptKind::Initial);
}
```

Append to `src/vtbackend/shell/Osc133Reporting_test.cpp`:

```cpp
TEST_CASE("OSC 133;A k=s continues the prompt in progress", "[osc133]")
{
    // A two-line command typed into Nushell, marked byte for byte as reedline marks it.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mock.writeToScreen("\033]133;A;k=i;click_events=1\033\\> \033]133;B\033\\for x in [1 2] {\r\n");
    mock.writeToScreen("\033]133;A;k=s;click_events=1\033\\::: \033]133;B\033\\print $x }\r\n");
    mock.writeToScreen("\033]133;C\033\\1\r\n2\r\n\033]133;D;0\033\\");

    // One block, headed by the first line only.
    auto const& screen = mock.terminal.currentScreen();
    CHECK(screen.lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    CHECK_FALSE(screen.lineFlagsAt(LineOffset(1)).contains(LineFlag::Marked));

    auto const* const record = mock.terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->exitCode == std::optional { 0 });
    auto const* const head = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(head != nullptr);
    CHECK(head->id == record->id);
}

TEST_CASE("OSC 133;A k=r beside a prompt does not start another", "[osc133]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mock.writeToScreen("\033]133;A\033\\$ ");
    auto const first = mock.terminal.commandBlocks().currentId();
    mock.writeToScreen("\033]133;A;k=r\033\\\033]133;B\033\\ls\r\n");
    mock.writeToScreen("\033]133;C\033\\a\r\n\033]133;D;0\033\\");

    auto const* const record = mock.terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->id == first); // the right prompt's A started no record of its own
    auto const* const head = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(head != nullptr);
    CHECK(head->id == record->id);
}
```

In `src/vtbackend/CMakeLists.txt`, in the `vtbackend_test` list, add `        vt/SemanticPrompt_test.cpp` directly before `        vt/Sequence_test.cpp`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `fatal error C1083: Cannot open include file: 'vtbackend/vt/SemanticPrompt.hpp'`.

- [ ] **Step 3: Declare `promptKindOf`**

Create `src/vtbackend/vt/SemanticPrompt.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace vtbackend
{

/// What an OSC 133;A starts, by its `k=` parameter (the semantic-prompts "kind").
enum class PromptKind : uint8_t
{
    Initial = 0,  ///< `k=i`, or no `k=` at all: a new prompt, and with it a new command block.
    Continuation, ///< `k=c` or `k=s`: a further line of the prompt in progress (PS2, reedline).
    Right,        ///< `k=r`: a right-hand prompt drawn beside the one in progress.
};

/// Reads the prompt kind from OSC 133;A's parameters.
/// @param parameters Everything after the "A", e.g. ";k=s;click_events=1"; empty for a bare "A".
/// @return The kind; Initial when there is no `k=`, or one this terminal does not know.
[[nodiscard]] PromptKind promptKindOf(std::string_view parameters) noexcept;

} // namespace vtbackend
```

- [ ] **Step 4: Implement it**

Create `src/vtbackend/vt/SemanticPrompt.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SemanticPrompt.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace vtbackend
{

namespace
{
    using namespace std::string_view_literals;

    /// Every `k=` value with a meaning here; any other reads as Initial.
    constexpr auto PromptKindTokens = std::array {
        std::pair { "i"sv, PromptKind::Initial },
        std::pair { "c"sv, PromptKind::Continuation },
        std::pair { "s"sv, PromptKind::Continuation },
        std::pair { "r"sv, PromptKind::Right },
    };
} // namespace

PromptKind promptKindOf(std::string_view parameters) noexcept
{
    auto kind = PromptKind::Initial;
    core::forEachKeyValue(
        core::ForEachKeyValueParams { .text = parameters, .entryDelimiter = ';', .assignmentDelimiter = '=' },
        [&kind](std::string_view key, std::string_view value) noexcept {
            if (key != "k")
                return;
            auto const token =
                std::ranges::find(PromptKindTokens, value, &std::pair<std::string_view, PromptKind>::first);
            if (token != PromptKindTokens.end())
                kind = token->second;
        });
    return kind;
}

} // namespace vtbackend
```

In `src/vtbackend/CMakeLists.txt` add `    vt/SemanticPrompt.hpp` directly after `    shell/SemanticBlockTracker.hpp` (`vtbackend_HEADERS`) and `    vt/SemanticPrompt.cpp` directly after `    shell/SemanticBlockTracker.cpp` (`vtbackend_SOURCES`).

- [ ] **Step 5: Let `Screen` skip what does not start a prompt**

In `src/vtbackend/screen/Screen.cpp` add `#include <vtbackend/vt/SemanticPrompt.hpp>` directly after `#include <vtbackend/vt/RectangularAreaChecksum.hpp>`, and make the following the first statements of the OSC 133 handler's `case 'A': {` (at `c74b93ad` they go between `case 'A': {` and `setMark();`; whatever phase 1 put after that stays exactly as it is):

```cpp
            // A further line of the prompt in progress (`k=c`/`k=s`: PS2, and reedline -- Nushell
            // 0.111+ -- before every continuation line) or a right-hand prompt beside it (`k=r`)
            // starts no new prompt: marking it a head would end the block being typed.
            if (promptKindOf(seq.intermediateCharacters().substr(1)) != PromptKind::Initial)
                break;
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[osc133]"`
Expected: `All tests passed`. Then run the whole binary, `out/build/clangcl-debug/bin/vtbackend_test.exe`: it passes too (no existing OSC 133 test relied on `k=` being ignored).

- [ ] **Step 7: Format and commit**

Run: `clang-format -i src/vtbackend/vt/SemanticPrompt.hpp src/vtbackend/vt/SemanticPrompt.cpp src/vtbackend/vt/SemanticPrompt_test.cpp src/vtbackend/shell/Osc133Reporting_test.cpp src/vtbackend/screen/Screen.cpp`

```bash
git add src/vtbackend/vt/SemanticPrompt.hpp src/vtbackend/vt/SemanticPrompt.cpp \
        src/vtbackend/vt/SemanticPrompt_test.cpp src/vtbackend/shell/Osc133Reporting_test.cpp \
        src/vtbackend/screen/Screen.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: let continuation prompts continue the block being typed

OSC 133;A with k=s or k=c (PS2; reedline, so Nushell 0.111+, sends one
before every continuation line) or k=r (a right-hand prompt) no longer marks
a head row and no longer discards the command being typed.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.5: The shell harness, and bash

**Files:**
- Create: `src/contour/cli/ShellIntegrationShells_test.cpp`
- Modify: `src/contour/cli/shell-integration/shell-integration.bash` (prepend a new Contour block before line 1; delete lines 382-409)
- Modify: `src/contour/CMakeLists.txt` (`contour_test` sources, lines 806-821; its `target_link_libraries`, line 824)

**Interfaces:**
- Consumes: `contour::cli::shellIntegrationScript(std::string_view)` (`ShellIntegration.hpp:36`); `vtpty::Process`, `vtpty::Process::ExecInfo`, `vtpty::Process::Environment`, `vtpty::Process::TerminationHint` (`Process.hpp`); `vtpty::createPty`; `vtpty::NoPlacement`; `vtpty::testing::readUntil`; `vthost::resolveExecutablePath(std::string_view, std::string_view, std::function<bool(std::filesystem::path const&)> const&)` (`src/vthost/Daemon.hpp:252-255`, pure, PATH-walking, already used by `contour client`); `core::replaceVariables` (`Utils.hpp:724`), `core::unescapeURL`, `core::escape`, `core::defaultEnvironment()`, `core::testing::ScopedTempDir` (`vendor/core-cpp/src/core/testing/ScopedTempDir.hpp`); `vtbackend::MockTerm<>` (`writeToScreen`, `replyData`, `resetReplyData`, `terminal.flushInput()`); C1 `Terminal::commandBlocks()`, `CommandBlockStore::lastFinished()`, `CommandBlockRecord::{commandLine, commandLineSource, exitCode}`.
- Produces: test-only helpers in `ShellIntegrationShells_test.cpp`'s anonymous namespace — `struct Mark { char kind; std::string params; }`, `std::vector<Mark> osc133Marks(std::string_view)`, `std::string kindsOf(std::vector<Mark> const&)`, `struct ShellLaunch`, `std::vector<ShellLaunch>& shellLaunches()`, `ShellLaunch const& launchFor(std::string_view)`, `class ShellSession` (`pumpUntil(std::function<bool(std::string_view)> const&, std::string_view what)`, `type(std::string_view)`, `transcript()`, `terminal()`), `std::unique_ptr<ShellSession> start(ShellLaunch const&)`, `void awaitPromptEnd(ShellSession&, size_t from)`, `void checkCommandCycle(ShellSession&, ShellLaunch const&)`, `std::string printedBy(ShellLaunch const&)` and `constexpr EncodingCases`; Tasks 3.6–3.9 add rows and test cases to the same file. The bash script's behaviour: `OSC 133;B` at the end of `PS1`, `OSC 133;C;cmdline_url=` from bash-preexec's `$1`.

**Where these tests run.** In `contour_test`, which already links `contour_cli` (the embedded scripts) and `vtbackend` (and through it `vtpty`). Each shell absent from `PATH` SKIPs; the POSIX shells are not run on Windows, where `bash` on `PATH` may be WSL's launcher. On this Windows box the bash, zsh, fish and tcsh cases therefore report *skipped*; their FAIL/PASS verdicts come from a Linux tree — WSL, or the `ubuntu_2404_cc_matrix` CI job (Task 3.11 makes a skip there a failure) — and from the macOS job (bash 3.2, zsh, tcsh).

**What the bash change fixes besides `B` and the command line.** Contour's hooks used to sit *after* the vendored bash-preexec, whose duplicate-inclusion guard (`shell-integration.bash:50-53`) `return`s from the whole sourced file: with a bash-preexec already loaded by something else (atuin, starship), or the file sourced twice, Contour's hooks were silently never installed. They now come first, under their own names (`__contour_preexec`, `__contour_precmd`) instead of bash-preexec's convenience names `preexec`/`precmd`, which a user's own functions of the same name replaced.

**The `PS1` guard.** `__contour_mark_prompt_end` runs as a precmd hook and rewrites `PS1` as *(PS1 with every copy of the mark removed) + mark* — so a re-sourced file, or a framework that rebuilds `PS1` around its previous value, never ends up with two. `__contour_precmd` moves that hook to the end of `precmd_functions` every prompt, so a framework that registered its own `PS1`-building hook later (starship, oh-my-bash) runs before it — from the second prompt on, since bash-preexec expands the list before running it. A framework that builds `PS1` from a `PROMPT_COMMAND` entry of its own still runs after every precmd hook and drops the mark; that prompt then lacks only its `B` column (the command line is reported with `C` regardless).

- [ ] **Step 1: Write the harness and the failing bash tests**

Create `src/contour/cli/ShellIntegrationShells_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The shell-integration scripts, run by the shells they are written for.
//
// Each shell found on PATH is started on a real PTY with its embedded script sourced -- the very
// bytes `contour generate integration` hands out -- and a command line holding two spaces, both
// kinds of quote, a ';', a '%' and characters of two, three and four UTF-8 bytes is typed at its
// prompt. Every byte the shell writes goes through a Terminal, which also answers whatever the shell
// asks of its terminal on the way (fish's DA1, .NET's cursor-position report), as Contour would. Two
// witnesses then have to agree: the OSC 133 sequences in the byte stream, and the command block the
// Terminal recorded from them.
//
// A shell that is not installed SKIPs. CI installs bash, zsh, fish and tcsh and fails when any of them
// skipped ("verify the shell-integration tests actually ran", build.yml). The POSIX shells are not
// run on Windows, where `bash` on PATH may be WSL's launcher.

#include <contour/cli/ShellIntegration.hpp>

#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <vthost/Daemon.hpp>

#include <core/Environment.hpp>
#include <core/Escape.hpp>
#include <core/Utils.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace
{

/// The terminal's size, for the PTY and for the Terminal reading it alike.
constexpr auto TerminalSize =
    vtbackend::PageSize { .lines = vtbackend::LineCount(24), .columns = vtbackend::ColumnCount(80) };

/// How long a shell may take, all told, before the test ends it: on Windows a read blocks until the
/// pseudoconsole has something to say, and only the shell's end makes it return.
constexpr auto Deadline = std::chrono::seconds { 60 };

#ifdef __APPLE__
/// A UTF-8 locale the runner has: macOS ships no C.UTF-8.
constexpr auto Utf8Locale = "en_US.UTF-8"sv;
#else
/// A UTF-8 locale the runner has.
constexpr auto Utf8Locale = "C.UTF-8"sv;
#endif

/// The command line typed at the POSIX shells' prompts: two spaces, both kinds of quote, the ';' that
/// separates OSC 133's parameters, a '%', and characters of two, three and four UTF-8 bytes.
constexpr auto PosixCommand = "echo \"a  b;c\" 'd\"e' 50% \xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80"sv;

/// One OSC 133 sequence found in a byte stream.
struct Mark
{
    char kind {};       ///< A, B, C or D.
    std::string params; ///< What follows "<kind>;", e.g. "cmdline_url=ls" or "0"; empty for none.
};

/// @return Every OSC 133 sequence in @p bytes, in order, whichever terminator (ST or BEL) ends it.
///         One the end of @p bytes cuts off is not counted.
[[nodiscard]] std::vector<Mark> osc133Marks(std::string_view bytes)
{
    constexpr auto Introducer = "\x1b]133;"sv;
    auto marks = std::vector<Mark> {};
    for (auto const piece: std::views::split(bytes, Introducer) | std::views::drop(1))
    {
        auto const body = std::string_view { piece.begin(), piece.end() };
        auto const end = body.find_first_of("\x07\x1b"sv);
        if (end == std::string_view::npos || end == 0)
            continue;
        auto const payload = body.substr(0, end);
        marks.push_back(Mark { .kind = payload.front(),
                               .params = std::string { payload.size() > 2 ? payload.substr(2) : ""sv } });
    }
    return marks;
}

/// @return The kinds of @p marks as one string, e.g. "ABCDAB".
[[nodiscard]] std::string kindsOf(std::vector<Mark> const& marks)
{
    return std::ranges::to<std::string>(marks | std::views::transform(&Mark::kind));
}

/// How one shell is started for these tests: with its integration script sourced, and nothing of
/// the user's -- a private HOME holds the start-up file that sources it.
struct ShellLaunch
{
    std::string shell;                  ///< The integration table's name for it.
    std::string program;                ///< The executable looked up on PATH.
    std::string scriptFile;             ///< Where in HOME the script goes; PowerShell runs only *.ps1.
    std::string startupFile;            ///< A start-up file written into HOME, by relative path; empty for none.
    std::string startupText;            ///< Its contents. ${SCRIPT} and ${HOME} stand for those paths.
    std::vector<std::string> arguments; ///< The command-line arguments, with the same substitutions.
    std::string command;                ///< The command line typed at the prompt.
    vtbackend::CommandLineSource commandLineSource; ///< How the terminal learns that command line.
};

/// @return Every shell the integration scripts target, and how each is started here.
[[nodiscard]] std::vector<ShellLaunch>& shellLaunches()
{
    static auto launches = std::vector<ShellLaunch> {
        ShellLaunch {
            .shell = "bash",
            .program = "bash",
            .scriptFile = "shell-integration.bash",
            .startupFile = ".bashrc",
            .startupText = "PS1='$ '\nsource '${SCRIPT}'\n",
            .arguments = { "--noprofile", "--rcfile", "${HOME}/.bashrc", "-i" },
            .command = std::string { PosixCommand },
            .commandLineSource = vtbackend::CommandLineSource::Reported,
        },
    };
    return launches;
}

/// @return The row for @p shell.
[[nodiscard]] ShellLaunch const& launchFor(std::string_view shell)
{
    auto const& launches = shellLaunches();
    auto const row = std::ranges::find(launches, shell, &ShellLaunch::shell);
    REQUIRE(row != launches.end());
    return *row;
}

/// @return Whether @p candidate is a file this process may execute.
[[nodiscard]] bool isExecutableFile(std::filesystem::path const& candidate)
{
    auto error = std::error_code {};
    auto const status = std::filesystem::status(candidate, error);
    if (error || !std::filesystem::is_regular_file(status))
        return false;
#ifdef _WIN32
    return true;
#else
    return (status.permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
#endif
}

/// @return Where @p program is on PATH, or nullopt when it is not.
[[nodiscard]] std::optional<std::filesystem::path> findOnPath(std::string_view program)
{
    auto const searchPath = core::defaultEnvironment().get("PATH").value_or(std::string {});
    auto const resolved = vthost::resolveExecutablePath(program, searchPath, isExecutableFile);
    if (resolved == program)
        return std::nullopt;
    return std::filesystem::path { resolved };
}

/// Writes @p contents to @p path, creating the directories above it.
void writeFile(std::filesystem::path const& path, std::string_view contents)
{
    std::filesystem::create_directories(path.parent_path());
    auto out = std::ofstream { path, std::ios::binary };
    REQUIRE(out.good());
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/// @return The environment every shell here runs in: its own HOME, and with it its own history and
///         start-up files; a UTF-8 locale; a TERM every shell knows.
[[nodiscard]] vtpty::Process::Environment isolatedEnvironment(std::string const& home)
{
    return vtpty::Process::Environment {
        { "HOME", home },
        { "ZDOTDIR", home },
        { "XDG_CONFIG_HOME", home + "/.config" },
        { "XDG_DATA_HOME", home + "/.local/share" },
        { "HISTFILE", home + "/.history" },
        { "BASH_SILENCE_DEPRECATION_WARNING", "1" },
        { "LANG", std::string { Utf8Locale } },
        { "LC_ALL", std::string { Utf8Locale } },
        { "TERM", "xterm-256color" },
    };
}

/// A shell started per a ShellLaunch on a real PTY, whose output a Terminal reads -- and answers -- as
/// Contour would.
class ShellSession
{
  public:
    /// Writes the script and the start-up file into a private HOME and starts the shell there.
    /// @param launch How to start it.
    /// @param program Where its executable was found.
    ShellSession(ShellLaunch const& launch, std::filesystem::path const& program):
        _home { "contour-shell-integration" },
        _process { prepare(launch, program, _home),
                   vtpty::createPty(TerminalSize, std::nullopt),
                   /*escapeSandbox=*/false,
                   std::make_shared<vtpty::NoPlacement>() },
        _terminal { TerminalSize },
        _watchdog { [this](std::stop_token const& stop) { endAfterDeadline(stop); } }
    {
        REQUIRE(_process.start().has_value());
    }

    ShellSession(ShellSession const&) = delete;
    ShellSession& operator=(ShellSession const&) = delete;
    ShellSession(ShellSession&&) = delete;
    ShellSession& operator=(ShellSession&&) = delete;

    ~ShellSession()
    {
        _watchdog.request_stop();
        _process.terminate(vtpty::Process::TerminationHint::Hangup);
        std::ignore = _process.wait();
    }

    /// Reads until @p done holds for everything the shell wrote so far, feeding each byte through the
    /// terminal and sending back whatever the terminal answers; fails the test when it never does.
    /// @param done Whether what the shell wrote so far is enough.
    /// @param what What is being waited for, for the failure message.
    void pumpUntil(std::function<bool(std::string_view)> const& done, std::string_view what)
    {
        auto seen = size_t { 0 };
        std::ignore = vtpty::testing::readUntil(_process, [&](std::string_view readSoFar) {
            auto const fresh = readSoFar.substr(seen);
            seen = readSoFar.size();
            _transcript += fresh;
            _terminal.writeToScreen(fresh);
            _terminal.terminal.flushInput();
            if (!_terminal.replyData().empty())
            {
                std::ignore = _process.write(_terminal.replyData());
                _terminal.resetReplyData();
            }
            return done(_transcript);
        });
        INFO("waiting for: " << what);
        INFO("what the shell wrote: " << core::escape(_transcript));
        REQUIRE(done(_transcript));
    }

    /// Types @p text at the shell, as a keyboard would.
    void type(std::string_view text) { REQUIRE(_process.write(text) == static_cast<int>(text.size())); }

    /// @return Everything the shell wrote so far.
    [[nodiscard]] std::string const& transcript() const noexcept { return _transcript; }

    /// @return The terminal that read it.
    [[nodiscard]] vtbackend::MockTerm<>& terminal() noexcept { return _terminal; }

  private:
    /// Puts the script and the start-up file into @p home.
    /// @return How to start the shell there.
    [[nodiscard]] static vtpty::Process::ExecInfo prepare(ShellLaunch const& launch,
                                                          [[maybe_unused]] std::filesystem::path const& program,
                                                          core::testing::ScopedTempDir const& home)
    {
        auto const homeText = home.path().generic_string();
        auto const scriptText = (home / launch.scriptFile).generic_string();
        auto const substitute = [&](std::string_view text) {
            return core::replaceVariables(
                text, [&](std::string_view name) -> std::string const& { return name == "SCRIPT" ? scriptText : homeText; });
        };

        auto const script = contour::cli::shellIntegrationScript(launch.shell);
        REQUIRE(script.has_value());
        writeFile(home / launch.scriptFile, *script);
        if (!launch.startupFile.empty())
            writeFile(home / launch.startupFile, substitute(launch.startupText));

#ifdef _WIN32
        // CreateProcess searches PATH itself, and Process_win32 does not quote a program path, which
        // "C:\Program Files\PowerShell\7\pwsh.exe" would need.
        auto const programText = launch.program;
#else
        auto const programText = program.string();
#endif
        auto exec = vtpty::Process::ExecInfo {
            .program = programText, .arguments = {}, .workingDirectory = home.path(), .env = isolatedEnvironment(homeText)
        };
        for (auto const& argument: launch.arguments)
            exec.arguments.push_back(substitute(argument));
        return exec;
    }

    /// Ends the shell once Deadline has passed, unless asked to stop first.
    void endAfterDeadline(std::stop_token const& stop)
    {
        auto mutex = std::mutex {};
        auto lock = std::unique_lock { mutex };
        auto wakeup = std::condition_variable_any {};
        std::ignore = wakeup.wait_for(lock, stop, Deadline, [] { return false; });
        if (!stop.stop_requested())
            _process.terminate(vtpty::Process::TerminationHint::Hangup);
    }

    core::testing::ScopedTempDir _home;
    vtpty::Process _process;
    vtbackend::MockTerm<> _terminal;
    std::string _transcript;
    std::jthread _watchdog; // last: stopped and joined before anything it touches goes away
};

/// @return @p launch's shell, started; SKIPs when it is not installed.
[[nodiscard]] std::unique_ptr<ShellSession> start(ShellLaunch const& launch)
{
#ifdef _WIN32
    if (launch.shell != "pwsh")
        SKIP(launch.shell << " is not run on Windows");
#endif
    auto const program = findOnPath(launch.program);
    if (!program)
        SKIP(launch.program << " is not installed");
    return std::make_unique<ShellSession>(launch, *program);
}

/// Waits until the shell has drawn a prompt to its end (OSC 133;B) after @p from bytes of transcript.
void awaitPromptEnd(ShellSession& session, size_t from = 0)
{
    session.pumpUntil([from](std::string_view text) { return kindsOf(osc133Marks(text.substr(from))).contains('B'); },
                      "the end of a prompt (OSC 133;B)");
}

/// Types @p launch's command at the prompt the shell shows, and checks what it told the terminal from
/// then on: exactly one C carrying the command line, exactly one D with exit status 0, and the next
/// prompt's A and B, in that order -- repaints of the waiting prompt (A, B) aside -- and the command
/// block the terminal recorded from them.
void checkCommandCycle(ShellSession& session, ShellLaunch const& launch)
{
    auto const typedAt = session.transcript().size();
    session.type(launch.command + "\r");
    session.pumpUntil(
        [typedAt](std::string_view text) {
            auto const kinds = kindsOf(osc133Marks(text.substr(typedAt)));
            auto const finished = kinds.find('D');
            return finished != std::string::npos && kinds.find('B', finished) != std::string::npos;
        },
        "the command's end (OSC 133;D) and the next prompt's end (OSC 133;B)");

    auto const transcript = std::string_view { session.transcript() };
    INFO("what the shell wrote: " << core::escape(transcript));
    auto const marks = osc133Marks(transcript.substr(typedAt));
    auto const kinds = kindsOf(marks);
    auto const commandStart = kinds.find('C');
    REQUIRE(commandStart != std::string::npos);
    CHECK(kinds.substr(0, commandStart).find_first_not_of("AB") == std::string::npos);
    REQUIRE(kinds.substr(commandStart) == "CDAB");
    CHECK(marks[commandStart + 1].params == "0");

    if (launch.commandLineSource == vtbackend::CommandLineSource::Reported)
    {
        constexpr auto Key = "cmdline_url="sv;
        auto const params = std::string_view { marks[commandStart].params };
        REQUIRE(params.starts_with(Key));
        CHECK(core::unescapeURL(params.substr(Key.size())) == launch.command);
    }

    auto const* const record = session.terminal().terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine == launch.command);
    CHECK(record->commandLineSource == launch.commandLineSource);
    CHECK(record->exitCode == std::optional { 0 });
}

/// What a script's percent-encoder must make of a command line: one %XX per UTF-8 byte for everything
/// but A-Z a-z 0-9 / . _ ~ -, upper-case hex -- byte for byte what fish's own encoder produces.
constexpr auto EncodingCases = std::array {
    std::pair { "ls"sv, "ls"sv },
    std::pair { "a b"sv, "a%20b"sv },
    std::pair { "A-Z_a.z~/09"sv, "A-Z_a.z~/09"sv },
    std::pair { "x;y=z"sv, "x%3By%3Dz"sv },
    std::pair { "50%"sv, "50%25"sv },
    std::pair { "\"'$`\\"sv, "%22%27%24%60%5C"sv },
    std::pair { "\xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80"sv, "%C3%BC%E2%82%AC%F0%9F%98%80"sv },
    std::pair { "a\nb\tc"sv, "a%0Ab%09c"sv },
    std::pair { "\x1b]0;x\x07"sv, "%1B%5D0%3Bx%07"sv },
    std::pair { "\xff\xfe"sv, "%FF%FE"sv },
};

/// Runs @p launch's shell until it has printed a line ending in ']' -- a script function's result in
/// brackets, which the caller's arguments ask for.
/// @param launch The shell's row, its arguments replaced by the caller.
/// @return What the shell printed.
[[nodiscard]] std::string printedBy(ShellLaunch const& launch)
{
    auto const session = start(launch);
    session->pumpUntil([](std::string_view text) { return text.contains("]\r\n"); }, "the bracketed result");
    return session->transcript();
}

} // namespace

TEST_CASE("osc133Marks reads both terminators and skips a cut-off sequence", "[shell-integration]")
{
    auto const marks = osc133Marks("x\x1b]133;A\x07y\x1b]133;D;0\x1b\\z\x1b]133;C;cmdline_url=l");
    CHECK(kindsOf(marks) == "AD");
    CHECK(marks[1].params == "0");
}

TEST_CASE("bash integration reports prompts, the command line and its end", "[shell-integration][shells][bash]")
{
    auto const session = start(launchFor("bash"));
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launchFor("bash"));
}

TEST_CASE("bash integration sourced twice still marks everything once", "[shell-integration][shells][bash]")
{
    auto launch = launchFor("bash");
    launch.startupText += "source '${SCRIPT}'\n";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launch);
}

TEST_CASE("bash integration survives a prompt framework that rebuilds PS1 later",
          "[shell-integration][shells][bash]")
{
    // A framework sourced after the integration, rebuilding PS1 in a precmd hook of its own the way
    // starship and oh-my-bash do. The mark moves behind it, which counts from the second prompt on.
    auto launch = launchFor("bash");
    launch.startupText += "__framework_prompt() { PS1='fw$ '; }\nprecmd_functions+=(__framework_prompt)\n";
    auto const session = start(launch);
    session->pumpUntil([](std::string_view text) { return text.contains("fw$ "); }, "the framework's prompt");
    auto const emptyLineAt = session->transcript().size();
    session->type("\r");
    awaitPromptEnd(*session, emptyLineAt);
    checkCommandCycle(*session, launch);
}

TEST_CASE("bash integration percent-encodes the command line byte by byte", "[shell-integration][shells][bash]")
{
    for (auto const& [input, encoded]: EncodingCases)
    {
        INFO("input: " << core::escape(input));
        auto launch = launchFor("bash");
        launch.arguments = { "--noprofile",
                             "--norc",
                             "-c",
                             "source \"$1\"; printf '[%s]\\n' \"$(__contour_percent_encode \"$2\")\"",
                             "bash",
                             "${SCRIPT}",
                             std::string { input } };
        CHECK(printedBy(launch).contains(std::format("[{}]", encoded)));
    }
}
```

In `src/contour/CMakeLists.txt` add `        cli/ShellIntegrationShells_test.cpp` directly after `        cli/ShellIntegration_test.cpp` in `add_executable(contour_test …)`, and change

```cmake
    target_link_libraries(contour_test PRIVATE contour_command contour_cli vtbackend core::testing Catch2::Catch2)
```

to

```cmake
    # vthost for resolveExecutablePath(), the PATH walk the shell-integration tests find their shells with.
    target_link_libraries(contour_test PRIVATE contour_command contour_cli vtbackend vthost core::testing Catch2::Catch2)
```

- [ ] **Step 2: Run the bash tests to see them fail**

On Linux (WSL or CI tree; `cmake --preset gcc-debug` once):
Run: `cmake --build --preset gcc-debug --target contour_test && out/gcc-debug/src/contour/contour_test "[shells][bash]"`
Expected: FAIL — "reports prompts…" fails waiting for `the end of a prompt (OSC 133;B)` (today's script sends none); "percent-encodes…" fails because `__contour_percent_encode` is not defined (`[]` printed).

On this Windows box:
Run: `cmake --build --preset clangcl-debug --target contour_test` then `out/build/clangcl-debug/bin/contour_test.exe "[shell-integration]"`
Expected: the build is warning-free; `osc133Marks reads both terminators…` passes, the four bash cases report `skipped`.

- [ ] **Step 3: Put Contour's bash hooks first**

In `src/contour/cli/shell-integration/shell-integration.bash`, delete the old Contour block — lines 382-409, from `# Actual customized code (for Contour) starts here:` through the closing `}` of `precmd()` at the end of the file, together with the blank line before it — and insert the following *before* line 1 (`# bash-preexec.sh -- Bash support for ZSH-like 'preexec' and 'precmd' functions.`), followed by one blank line:

```bash
# Contour shell integration for bash.
#
# Source it from ~/.bashrc. It carries its own copy of bash-preexec
# (https://github.com/rcaloras/bash-preexec), vendored verbatim at the end of this file, for the
# preexec/precmd hooks that bash itself does not have.
#
# Contour's hooks come FIRST and bash-preexec last. bash-preexec's duplicate-inclusion guard returns
# from the whole sourced file, so with the hooks after it, a bash-preexec loaded earlier by something
# else (atuin, starship and others bring their own) -- or this file sourced a second time -- left
# Contour's hooks silently uninstalled.
#
# shellcheck shell=bash

# Not bash: nothing below parses anywhere else. (POSIX syntax for exactly that reason.)
if [ -z "${BASH_VERSION-}" ]; then
    return 1
fi

# The longest command line sent as cmdline_url=, in bytes: a little more than the terminal keeps
# (vtbackend::MaxRecordedCommandLineBytes, 4096), so that it still sees a longer one was cut.
__contour_cmdline_max_bytes=4100

# OSC 133;B as it sits at the end of PS1: \[ \] tell readline it takes no room on the line.
__contour_prompt_end_mark='\[\e]133;B\e\\\]'

# Prints $1 percent-encoded byte by byte, as kitty's cmdline_url= wants it: every byte but
# A-Z a-z 0-9 / . _ ~ - (what fish's own encoder leaves alone, too) becomes %XX. That keeps ';', which
# separates OSC 133's parameters, and the ESC and BEL that would end the sequence out of it.
# Call it in a subshell: it switches the locale.
__contour_percent_encode() {
    # Bytes, not characters: in the C locale ${#text} and ${text:i:1} count bytes, so a multibyte
    # character becomes one %XX per UTF-8 byte.
    LC_ALL=C
    local text=${1:0:__contour_cmdline_max_bytes} byte code i
    for (( i = 0; i < ${#text}; i++ )); do
        byte=${text:i:1}
        case $byte in
            [A-Za-z0-9/._~-]) printf '%s' "$byte" ;;
            *)
                # "'x" is the byte's code in the C locale -- or, in musl's, 0xDF00 plus the byte for one
                # above 0x7F. Its low eight bits are the byte either way.
                printf -v code '%d' "'$byte"
                printf '%%%02X' $(( code & 255 ))
                ;;
        esac
    done
}

__contour_preexec() {
    # Text reflow back on for the command's output.
    printf '\e[?2028h'

    # OSC 133;C -- the command's output begins on the line the cursor is on now -- with the command
    # line, which bash-preexec hands over as $1, exactly as history recorded it.
    printf '\e]133;C;cmdline_url=%s\e\\' "$(__contour_percent_encode "$1")"

    __contour_command_running=1
}

__contour_precmd() {
    # Must be the very first line: anything else would clobber the exit status we are about to report.
    local exit_status=$?

    # OSC 133;D -- the command that just ran has finished. Only once a command actually HAS run: an
    # unconditional D would put a CommandEnd on the very first prompt and invent a command block that
    # never existed.
    if [[ -n "${__contour_command_running:-}" ]]; then
        printf '\e]133;D;%s\e\\' "$exit_status"
        unset __contour_command_running
    fi

    # Text reflow off for the prompt; the working directory (OSC 7); OSC 133;A -- a new prompt starts
    # on this line.
    printf '\e[?2028l\e]7;%s\e\\\e]133;A\e\\' "$PWD"

    # Keep the prompt-end mark the LAST precmd hook, so that a prompt framework which registered a
    # PS1-building hook of its own after this file was sourced (starship, oh-my-bash) cannot build over
    # it. bash-preexec expanded the list before running it, so this counts from the next prompt on.
    local hook hooks=()
    for hook in "${precmd_functions[@]}"; do
        [[ $hook == __contour_mark_prompt_end ]] || hooks+=("$hook")
    done
    precmd_functions=("${hooks[@]}" __contour_mark_prompt_end)
}

# Appends the prompt-end mark to PS1 -- exactly once. PS1 outlives each prompt, and prompt frameworks
# rebuild it from scratch or around its old value, so any copy already in it is taken out first and the
# mark put back at the very end. A framework that rebuilds PS1 from PROMPT_COMMAND rather than from a
# precmd hook still runs after this and drops the mark: that prompt then has no prompt-end column,
# which costs nothing else -- the command line travels with ;C.
__contour_mark_prompt_end() {
    PS1=${PS1//"$__contour_prompt_end_mark"/}$__contour_prompt_end_mark
}

# Registered by name, and once: sourcing this file again must not run every hook twice.
[[ " ${preexec_functions[*]-} " == *" __contour_preexec "* ]] || preexec_functions+=(__contour_preexec)
[[ " ${precmd_functions[*]-} " == *" __contour_precmd "* ]] || precmd_functions+=(__contour_precmd)
[[ " ${precmd_functions[*]-} " == *" __contour_mark_prompt_end "* ]] || precmd_functions+=(__contour_mark_prompt_end)
```

The file must end with the vendored code's last lines (`__bp_install_after_session_init` … `fi;`) and a single trailing newline.

- [ ] **Step 4: Run the bash tests to see them pass**

On Linux: `cmake --build --preset gcc-debug --target contour_test && out/gcc-debug/src/contour/contour_test "[shells][bash]"`
Expected: `All tests passed` (4 test cases).

On this Windows box: `cmake --build --preset clangcl-debug --target contour_test` then `out/build/clangcl-debug/bin/contour_test.exe "[shell-integration]"`
Expected: the embedded-script tests (`the embedded scripts are the ones in the source tree`) pass with the rewritten script; bash cases `skipped`.

- [ ] **Step 5: Format and commit**

Run: `clang-format -i src/contour/cli/ShellIntegrationShells_test.cpp`

```bash
git add src/contour/cli/ShellIntegrationShells_test.cpp src/contour/cli/shell-integration/shell-integration.bash \
        src/contour/CMakeLists.txt
git commit -F - <<'EOF'
shell-integration: mark bash's prompt end and report its command line

PS1 ends in OSC 133;B, put there by a precmd hook that strips any earlier
copy first and keeps itself last, so neither a re-sourced script nor a prompt
framework doubles it. OSC 133;C carries bash-preexec's command line as
cmdline_url=, percent-encoded byte by byte in the C locale.

Contour's hooks now come before the vendored bash-preexec, whose
duplicate-inclusion guard returned from the whole file: with a bash-preexec
loaded elsewhere first (atuin, starship), they were never installed.

The new contour_test cases run each script in its shell on a real PTY,
through a Terminal that answers the shell, and SKIP when it is absent.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.6: zsh

**Files:**
- Modify: `src/contour/cli/shell-integration/shell-integration.zsh` (whole file, 56 lines)
- Modify: `src/contour/cli/ShellIntegrationShells_test.cpp`

**Interfaces:**
- Consumes: Task 3.5's harness (`shellLaunches()`, `start`, `awaitPromptEnd`, `checkCommandCycle`, `printedBy`, `EncodingCases`, `PosixCommand`).
- Produces: zsh's `PROMPT` ends in `%{ESC]133;B BEL%}`; `OSC 133;C;cmdline_url=` from `preexec`'s `$1`; the function `_contour_percent_encode` (sets `REPLY`).

The guard is zsh's twin of bash's: `_contour_mark_prompt_end` (a precmd hook) strips the mark from `PROMPT` and re-appends it; `precmd_hook_contour` keeps it the last precmd hook. zsh runs a hook list from a copy (`callhookfunc` in `Src/utils.c` duplicates the array — which is why a hook may remove itself with `add-zsh-hook -d`), so the move counts from the next prompt on. The mark ends in BEL rather than ST, so that no backslash has to survive prompt expansion under `PROMPT_SUBST`.

- [ ] **Step 1: Write the failing zsh tests**

In `src/contour/cli/ShellIntegrationShells_test.cpp`, add this row to the vector in `shellLaunches()`, after the bash row:

```cpp
        ShellLaunch {
            .shell = "zsh",
            .program = "zsh",
            .scriptFile = "shell-integration.zsh",
            .startupFile = ".zshrc",
            .startupText = "PROMPT='%# '\nsource '${SCRIPT}'\n",
            .arguments = { "-d", "-i" }, // -d: no /etc/zshrc; ZDOTDIR points zsh at HOME's .zshrc
            .command = std::string { PosixCommand },
            .commandLineSource = vtbackend::CommandLineSource::Reported,
        },
```

and append these test cases at the end of the file:

```cpp
TEST_CASE("zsh integration reports prompts, the command line and its end", "[shell-integration][shells][zsh]")
{
    auto const session = start(launchFor("zsh"));
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launchFor("zsh"));
}

TEST_CASE("zsh integration survives a theme that rebuilds PROMPT later", "[shell-integration][shells][zsh]")
{
    // A theme registered after the integration, rebuilding PROMPT in a precmd hook of its own the way
    // powerlevel10k and most oh-my-zsh themes do. The mark moves behind it from the second prompt on.
    auto launch = launchFor("zsh");
    launch.startupText += "_framework_prompt() { PROMPT='fw> ' }\nadd-zsh-hook precmd _framework_prompt\n";
    auto const session = start(launch);
    session->pumpUntil([](std::string_view text) { return text.contains("fw> "); }, "the theme's prompt");
    auto const emptyLineAt = session->transcript().size();
    session->type("\r");
    awaitPromptEnd(*session, emptyLineAt);
    checkCommandCycle(*session, launch);
}

TEST_CASE("zsh integration percent-encodes the command line byte by byte", "[shell-integration][shells][zsh]")
{
    for (auto const& [input, encoded]: EncodingCases)
    {
        INFO("input: " << core::escape(input));
        auto launch = launchFor("zsh");
        launch.arguments = { "-f",
                             "-c",
                             "source \"$1\"; _contour_percent_encode \"$2\"; print -r -- \"[$REPLY]\"",
                             "zsh",
                             "${SCRIPT}",
                             std::string { input } };
        CHECK(printedBy(launch).contains(std::format("[{}]", encoded)));
    }
}
```

- [ ] **Step 2: Run them to see them fail**

On Linux: `cmake --build --preset gcc-debug --target contour_test && out/gcc-debug/src/contour/contour_test "[shells][zsh]"`
Expected: FAIL — the first two wait in vain for `the end of a prompt (OSC 133;B)`; the encoder case prints `[]`. (Windows box: the three cases report `skipped`.)

- [ ] **Step 3: Rewrite the zsh script**

Replace the whole of `src/contour/cli/shell-integration/shell-integration.zsh` with:

```zsh
# vim:et:ts=4:sw=4
#
#// SPDX-License-Identifier: Apache-2.0

# Example hook to change profile based on directory.
# update_profile()
# {
#     case "$PWD" in
#         "$HOME"/work*) contour set profile to work ;;
#         "$HOME"/projects*) contour set profile to main ;;
#         *) contour set profile to mobile ;;
#     esac
# }

autoload -Uz add-zsh-hook

# The longest command line sent as cmdline_url=, in bytes: a little more than the terminal keeps
# (vtbackend::MaxRecordedCommandLineBytes, 4096), so that it still sees a longer one was cut.
typeset -g _contour_cmdline_max_bytes=4100

# OSC 133;B as it sits at the end of PROMPT. %{ %} tell zsh it takes no room on the line; BEL ends
# it, so that no backslash has to survive prompt expansion.
typeset -g _contour_prompt_end_mark=$'%{\e]133;B\a%}'

# Percent-encodes $1 into REPLY, byte by byte, as kitty's cmdline_url= wants it: every byte but
# A-Z a-z 0-9 / . _ ~ - (what fish's own encoder leaves alone, too) becomes %XX. That keeps ';',
# which separates OSC 133's parameters, and the ESC and BEL that would end the sequence out of it.
_contour_percent_encode()
{
    emulate -L zsh
    # Bytes, not characters: without MULTIBYTE, $#text and $text[i] count bytes, so a multibyte
    # character becomes one %XX per UTF-8 byte.
    setopt no_multibyte
    local text=${1[1,_contour_cmdline_max_bytes]} byte hex
    local -i i
    REPLY=
    for (( i = 1; i <= $#text; i++ )); do
        byte=$text[i]
        case $byte in
            ([A-Za-z0-9/._~-]) REPLY+=$byte ;;
            (*)
                hex=$(( [##16] #byte & 255 ))
                (( $#hex < 2 )) && hex=0$hex
                REPLY+=%$hex
                ;;
        esac
    done
}

precmd_hook_contour()
{
    # Must be the very first line: anything else would clobber the exit status we are about to report.
    local exit_status=$?

    # OSC 133;D -- the command that just ran has finished. Only emitted once a command actually HAS run:
    # an unconditional D would put a CommandEnd on the very first prompt and invent a command block that
    # never existed.
    if [[ -n "${_contour_command_running:-}" ]]; then
        print -n "\e]133;D;${exit_status}\e\\" >$TTY
        unset _contour_command_running
    fi

    # Disable text reflow for the command prompt (and below).
    print -n '\e[?2028l' >$TTY

    # OSC 133;A -- a new prompt starts on this line. Together with ;C below this is what lets the terminal
    # tell a prompt apart from a command's output, which is what "copy last command output" reads.
    print -n '\e]133;A\e\\' >$TTY

    # Informs contour terminal about the current working directory, so that e.g. OpenFileManager works.
    echo -ne '\e]7;'$(pwd)'\e\\' >$TTY

    # Keep the prompt-end mark the LAST precmd hook: a theme that rebuilds PROMPT in a precmd hook of its
    # own (powerlevel10k, most oh-my-zsh themes), registered after this file was sourced, would otherwise
    # build over it. zsh runs each prompt's hooks from a copy of the list, so the move counts from the
    # next prompt on.
    precmd_functions=(${precmd_functions:#_contour_mark_prompt_end} _contour_mark_prompt_end)

    # Example hook to update configuration profile based on base directory.
    # update_profile >$TTY
}

preexec_hook_contour()
{
    # Enables text reflow for the main page area again, so that a window resize will reflow again.
    print -n "\e[?2028h" >$TTY

    # OSC 133;C -- the command's output begins on the line the cursor is on now -- with the command line
    # as it was typed (preexec's $1), percent-encoded as kitty's cmdline_url=. $1 is empty while the
    # history mechanism is off; the terminal then reads the command off the screen instead.
    if [[ -n $1 ]]; then
        _contour_percent_encode "$1"
        printf '\e]133;C;cmdline_url=%s\e\\' "$REPLY" >$TTY
    else
        print -n '\e]133;C\e\\' >$TTY
    fi

    _contour_command_running=1
}

# Appends the prompt-end mark to PROMPT -- exactly once. PROMPT outlives each prompt, and themes rebuild
# it from scratch or around its old value, so any copy already in it is taken out first and the mark put
# back at the very end. Sourcing this file again changes nothing.
_contour_mark_prompt_end()
{
    PROMPT=${PROMPT//"$_contour_prompt_end_mark"/}$_contour_prompt_end_mark
}

add-zsh-hook precmd precmd_hook_contour
add-zsh-hook preexec preexec_hook_contour
add-zsh-hook precmd _contour_mark_prompt_end
```

- [ ] **Step 4: Run them to see them pass**

On Linux: `cmake --build --preset gcc-debug --target contour_test && out/gcc-debug/src/contour/contour_test "[shells][zsh]"`
Expected: `All tests passed` (3 test cases).
On this Windows box: `out/build/clangcl-debug/bin/contour_test.exe "[shell-integration]"` (after rebuilding `contour_test`) — the embedded-script tests pass, shell cases `skipped`.

- [ ] **Step 5: Format and commit**

Run: `clang-format -i src/contour/cli/ShellIntegrationShells_test.cpp`

```bash
git add src/contour/cli/shell-integration/shell-integration.zsh src/contour/cli/ShellIntegrationShells_test.cpp
git commit -F - <<'EOF'
shell-integration: mark zsh's prompt end and report its command line

PROMPT ends in OSC 133;B (BEL-terminated inside %{ %}), kept there by a
precmd hook that strips any earlier copy and moves itself behind themes
registered later. OSC 133;C carries preexec's $1 as cmdline_url=,
percent-encoded byte by byte with MULTIBYTE off.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.7: fish — add only what fish leaves out

**Files:**
- Modify: `src/contour/cli/shell-integration/shell-integration.fish` (whole file, 52 lines)
- Modify: `src/contour/cli/ShellIntegrationShells_test.cpp`

**Interfaces:**
- Consumes: Task 3.5's harness.
- Produces: the fish function `__contour_native_marks <fish_version> <mark_prompt_status>` (prints one line per mark fish sends itself: `A`, `C`, `D`, `cmdline`, `B`), the global list `__contour_native`, and the wrapper machinery `__contour_wrap_fish_prompt` / `__contour_prompt_end`.

What the script does per fish (finding 2):

| fish | fish sends | the script adds |
|---|---|---|
| < 4.0, or `no-mark-prompt` | nothing | A, B, C with `cmdline_url`, D |
| 4.0.0 | A, C (bare), D | B (the command is then recovered from the grid) |
| 4.0.1 – 4.2.x | A, C with `cmdline_url`, D | B |
| ≥ 4.3.0 | A, B, C with `cmdline_url`, D | nothing (reflow toggles and OSC 7 only) |

`B` goes at the end of what `fish_prompt` prints, by wrapping it; fish counts an OSC sequence in a prompt as zero-width. The wrap is checked before every prompt (`fish_prompt` event), because a prompt chosen later — `fish_config prompt choose`, starship, tide, or a `fish_prompt` defined after the script is sourced — replaces the wrapper.

- [ ] **Step 1: Write the failing fish tests**

In `src/contour/cli/ShellIntegrationShells_test.cpp` add this row to `shellLaunches()`, after the zsh row:

```cpp
        ShellLaunch {
            .shell = "fish",
            .program = "fish",
            .scriptFile = "shell-integration.fish",
            .startupFile = ".config/fish/config.fish", // XDG_CONFIG_HOME is HOME/.config
            .startupText = "set -g fish_greeting\nfunction fish_prompt\n    printf '> '\nend\nsource '${SCRIPT}'\n",
            .arguments = { "-i" },
            .command = std::string { PosixCommand },
            .commandLineSource = vtbackend::CommandLineSource::Reported,
        },
```

and append:

```cpp
TEST_CASE("fish integration reports prompts, the command line and its end", "[shell-integration][shells][fish]")
{
    // Whichever fish this is: a fish that marks prompts itself must not get a second A, C or D, and
    // one that does not must get all four -- checkCommandCycle() demands exactly CDAB either way.
    auto const session = start(launchFor("fish"));
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launchFor("fish"));
}

TEST_CASE("fish integration marks the end of a prompt defined after it was sourced",
          "[shell-integration][shells][fish]")
{
    auto launch = launchFor("fish");
    launch.startupText = "set -g fish_greeting\nsource '${SCRIPT}'\nfunction fish_prompt\n    printf 'fw> '\nend\n";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launch);
}

TEST_CASE("fish integration leaves to fish the marks fish sends itself", "[shell-integration][shells][fish]")
{
    struct Case
    {
        std::string_view version;
        std::string_view markPromptStatus; ///< `status test-feature mark-prompt`: 0 on, 1 off, 2 unknown.
        std::string_view native;           ///< What __contour_native_marks must print, space-joined.
    };
    constexpr auto Cases = std::array {
        Case { "3.7.1", "2", "" },
        Case { "4.0b1", "2", "A C D" },
        Case { "4.0.0", "2", "A C D" },
        Case { "4.0.1", "2", "A C D cmdline" },
        Case { "4.0.6", "1", "" },
        Case { "4.2.1", "0", "A C D cmdline" },
        Case { "4.3.0", "0", "A C D cmdline B" },
        Case { "4.3.0-12-gabcdef0", "0", "A C D cmdline B" },
        Case { "4.10.0", "0", "A C D cmdline B" },
    };
    for (auto const& [version, markPromptStatus, native]: Cases)
    {
        INFO("fish " << version << ", mark-prompt status " << markPromptStatus);
        auto launch = launchFor("fish");
        launch.arguments = { "-c",
                             "source $argv[1]; set -l marks (__contour_native_marks $argv[2] $argv[3]); "
                             "echo \"[$marks]\"",
                             "${SCRIPT}",
                             std::string { version },
                             std::string { markPromptStatus } };
        CHECK(printedBy(launch).contains(std::format("[{}]", native)));
    }
}
```

- [ ] **Step 2: Run them to see them fail**

On Linux (Ubuntu 24.04's fish is 3.7.x): `cmake --build --preset gcc-debug --target contour_test && out/gcc-debug/src/contour/contour_test "[shells][fish]"`
Expected: FAIL — no `OSC 133;B` in the prompt; `__contour_native_marks` unknown (fish prints an error, no `[…]` line). (Windows box: `skipped`.)

- [ ] **Step 3: Rewrite the fish script**

Replace the whole of `src/contour/cli/shell-integration/shell-integration.fish` with:

```fish
# vim:et:ts=4:sw=4
#
#// SPDX-License-Identifier: Apache-2.0

# Example hook to change profile based on directory.
# function update_profile
#    switch "$PWD"
#        case "$HOME/work"*
#            contour set profile to work
#        case "$HOME/projects"*
#            contour set profile to main
#        case '*'
#            contour set profile to mobile
#    end
# end

# Which OSC 133 marks this fish writes by itself (fish's release notes and its src/terminal.rs):
#   4.0.0 -- A, C and D;   4.0.1 -- C carries cmdline_url=;   4.3.0 -- B.
# `set -Ua fish_features no-mark-prompt` (fish 4.0.6 and later) turns all of them off again. This
# script adds exactly what fish leaves out, and never a second A, C or D.
#
# fish_version:       fish's $version, e.g. 4.0.2, 4.3.0-12-gabcdef0 or 4.0b1.
# mark_prompt_status: the exit status of `status test-feature mark-prompt` -- 0 on, 1 off, and 2 for
#                     a fish older than 4.0.6, which had no way to turn marking off.
# Prints one line per mark fish sends itself: A, C, D, cmdline (C carries the command line), B.
function __contour_native_marks --argument-names fish_version mark_prompt_status
    test "$mark_prompt_status" = 1; and return
    set -l parts (string match -ar '\d+' -- (string replace -r '[^0-9.].*$' '' -- $fish_version)) 0 0 0
    set -l number (math "$parts[1] * 10000 + $parts[2] * 100 + $parts[3]")
    test $number -ge 40000; or return
    printf '%s\n' A C D
    test $number -ge 40001; and echo cmdline
    test $number -ge 40300; and echo B
end

status test-feature mark-prompt
set -l __contour_mark_prompt_status $status
set -g __contour_native (__contour_native_marks $version $__contour_mark_prompt_status)

# OSC 133;B -- the prompt ends here and typed input begins.
function __contour_prompt_end
    printf "\e]133;B\e\\"
end

# Wraps fish_prompt so that the prompt-end mark is the last thing it prints; fish counts an escape
# sequence in a prompt as taking no room, so the mark lands where fish leaves the cursor for typing.
# Checked before every prompt rather than once: a prompt chosen later (`fish_config prompt choose`,
# starship, tide, a fish_prompt defined after this file was sourced) replaces the wrapper.
function __contour_wrap_fish_prompt
    functions -q fish_prompt; or return
    functions fish_prompt | string match -q '*__contour_prompt_end*'; and return
    functions -e __contour_original_fish_prompt
    functions -c fish_prompt __contour_original_fish_prompt
    function fish_prompt
        __contour_original_fish_prompt
        __contour_prompt_end
    end
end

function precmd_hook_contour -d "Shell Integration hook to be invoked before each prompt" -e fish_prompt
    # Must come first: anything else would clobber the exit status we are about to report.
    set -l exit_status $status

    # OSC 133;D -- the command that just ran has finished. Only emitted once a command actually HAS run:
    # an unconditional D would put a CommandEnd on the very first prompt and invent a command block that
    # never existed. Left to fish when fish sends it itself.
    if set -q _contour_command_running; and not contains -- D $__contour_native
        printf "\e]133;D;%s\e\\" $exit_status
    end
    set -e _contour_command_running

    # Disable text reflow for the command prompt (and below).
    printf '\e[?2028l'

    # OSC 133;A -- a new prompt starts on this line -- unless fish says so itself.
    contains -- A $__contour_native; or printf "\e]133;A\e\\"

    # Informs contour terminal about the current working directory, so that e.g. OpenFileManager works.
    printf "\e]7;%s\e\\" $PWD

    # OSC 133;B belongs at the very end of the prompt, which only fish_prompt itself reaches.
    contains -- B $__contour_native; or __contour_wrap_fish_prompt

    # Example hook to update configuration profile based on base directory.
    # update_profile
end

function preexec_hook_contour -d "Run after printing prompt" -e fish_preexec
    # Enables text reflow for the main page area again, so that a window resize will reflow again.
    printf "\e[?2028h"

    # OSC 133;C -- the command's output begins on the line the cursor is on now -- with the command line,
    # percent-encoded byte by byte as kitty's cmdline_url= wants it. Left to fish when fish sends it.
    contains -- C $__contour_native
    or printf "\e]133;C;cmdline_url=%s\e\\" (string escape --style=url -- $argv[1])

    set -g _contour_command_running 1
end
```

- [ ] **Step 4: Run them to see them pass**

On Linux: `cmake --build --preset gcc-debug --target contour_test && out/gcc-debug/src/contour/contour_test "[shells][fish]"`
Expected: `All tests passed` (3 test cases).

- [ ] **Step 5: Check the native path against a real fish ≥ 4.3 (spec §16.1)**

CI runs the distro's fish 3.7, which exercises the "script sends everything" row. Exercise the other end on a Linux machine with fish ≥ 4.3 (e.g. `sudo add-apt-repository -y ppa:fish-shell/release-4 && sudo apt install -y fish`, or `brew install fish`):
Run: `out/gcc-debug/src/contour/contour_test "[shells][fish]"`
Expected: `All tests passed` — `checkCommandCycle()` fails on any doubled A, C or D, so a pass proves the script adds nothing fish already sends. Record the fish version used in the commit body below. If no such fish is at hand, write "native path checked by the version table only" instead.

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/contour/cli/ShellIntegrationShells_test.cpp`

```bash
git add src/contour/cli/shell-integration/shell-integration.fish src/contour/cli/ShellIntegrationShells_test.cpp
git commit -F - <<'EOF'
shell-integration: add to fish only what fish does not mark itself

Spec §16.1 verification, from fish's source at the release tags: fish 4.0.0
sends OSC 133 A, C and D itself, 4.0.1 adds cmdline_url to C, 4.3.0 adds B;
`no-mark-prompt` (4.0.6+) turns all of it off. The script derives from
$version and `status test-feature mark-prompt` which of them fish sends, and
adds only the rest -- B by wrapping fish_prompt, re-checked before every
prompt; C with cmdline_url from fish_preexec's argv for fish before 4.0.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.8: tcsh — the prompt-end mark

**Files:**
- Modify: `src/contour/cli/shell-integration/shell-integration.tcsh` (whole file, 21 lines)
- Modify: `src/contour/cli/ShellIntegrationShells_test.cpp`

**Interfaces:**
- Consumes: Task 3.5's harness; C1 `CommandLineSource::Recovered` (phase 1 recovers the command from the grid between the `;B` column and the `;C` row).
- Produces: tcsh's `prompt` ends in the step-over literal `%{\0337\033[C\033]133;B\007\0338%} `.

tcsh has no hook that sees the command line, so it sends `B` only (spec §10.2) and the terminal reads the command back off the screen. `B`'s column therefore has to be exact. tcsh draws a `%{…%}` literal immediately before the character that follows it, and drops a literal nothing follows (finding 5). So, as iTerm2's tcsh integration does, the prompt's trailing space is taken off and put back *after* the literal — which then reaches the terminal just before that space is drawn, one column short of where typing begins. Inside the literal, DECSC, CUF (one column right), the mark, DECRC: the mark lands at the input column and the cursor ends where tcsh expects it. (At the right margin CUF cannot move; the mark then lands one column early and nothing else changes.) A prompt that does not end in a space gains one. There is no hook after the prompt is drawn, so the rewrite happens once, when the file is sourced: the user sources it after setting `prompt`.

- [ ] **Step 1: Write the failing tcsh test**

In `src/contour/cli/ShellIntegrationShells_test.cpp` add this row to `shellLaunches()`, after the fish row:

```cpp
        ShellLaunch {
            .shell = "tcsh",
            .program = "tcsh",
            .scriptFile = "shell-integration.tcsh",
            .startupFile = ".tcshrc",
            .startupText = "set prompt = \"%# \"\nsource '${SCRIPT}'\n",
            .arguments = { "-i" },
            .command = std::string { PosixCommand },
            .commandLineSource = vtbackend::CommandLineSource::Recovered, // tcsh cannot report it
        },
```

and append:

```cpp
TEST_CASE("tcsh integration ends its prompt where typing begins", "[shell-integration][shells][tcsh]")
{
    // tcsh reports no command line, so the terminal reads it back off the screen from the ;B column
    // on: the record only matches the typed command if that column is exact.
    auto const session = start(launchFor("tcsh"));
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launchFor("tcsh"));
}

TEST_CASE("tcsh integration sourced twice still marks the prompt once", "[shell-integration][shells][tcsh]")
{
    auto launch = launchFor("tcsh");
    launch.startupText += "source '${SCRIPT}'\n";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launch);
}
```

- [ ] **Step 2: Run them to see them fail**

On Linux: `cmake --build --preset gcc-debug --target contour_test && out/gcc-debug/src/contour/contour_test "[shells][tcsh]"`
Expected: FAIL — waiting for `the end of a prompt (OSC 133;B)`. (Windows box: `skipped`.)

- [ ] **Step 3: Rewrite the tcsh script**

Replace the whole of `src/contour/cli/shell-integration/shell-integration.tcsh` with:

```tcsh
# vim:et:ts=4:sw=4
#
#// SPDX-License-Identifier: Apache-2.0

# Escapes are written as \033, not \e: tcsh's builtin echo understands the octal form but not the GNU \e
# extension, and silently prints the latter as literal text. (That is why this file used to emit the
# characters "\e[?2028l..." into the terminal rather than the sequences it meant.)
#
# precmd emits, in order:
#   OSC 133;D  -- the previous command finished, with its exit status,
#   DECRST 2028 -- text reflow off for the prompt,
#   OSC 7      -- the working directory,
#   OSC 133;A  -- a new prompt starts on this line.
# The prompt itself ends in OSC 133;B -- typed input begins here (see the end of this file).
# postcmd emits DECSET 2028 (reflow back on) and OSC 133;C -- the command's output begins here. No tcsh
# hook sees the command line, so ;C carries none: the terminal reads the command back off the screen,
# from the ;B column on.
#
# Together, ;A/;C/;D are what let the terminal tell a prompt apart from a command's output, which is what
# "copy last command output" reads. Unlike the other shells there is no guard against emitting ;D at the
# very first prompt -- a tcsh alias cannot carry that state without clobbering $status -- so the first
# prompt reports a command block with no text in it. The terminal ignores an empty block.
alias precmd 'echo -n "\033]133;D;$status\033\\\033[?2028l\033]7;$PWD\033\\\033]133;A\033\\";'
alias postcmd 'echo -n "\033[?2028h\033]133;C\033\\";'

# OSC 133;B, as a %{...%} literal at the end of the prompt.
#
# tcsh draws a literal together with the character after it, and drops one that nothing follows ("we
# lose the last literal", RefreshPromptpart() in tcsh's ed.refresh.c). So, as iTerm2's integration does,
# the prompt's trailing space comes off and goes back on after the mark -- which then reaches the
# terminal just before that space is drawn, one column short of where typing begins. DECSC, CUF and
# DECRC around it step over the space and back: the mark lands where typing begins, and the cursor ends
# where tcsh expects it. A prompt that does not end in a space gains one.
#
# No tcsh hook runs after the prompt is drawn, so `prompt` is rewritten once, here: source this file
# after setting your prompt. Sourcing it again changes nothing. The prompt travels through the
# environment and sed rather than through quotes, which no csh quoting would let it survive intact.
if ( $?prompt ) then
    if ( "$prompt" !~ *133?B* ) then
        setenv CONTOUR_PROMPT_TEXT "${prompt}@@contour@@"
        set prompt = "`printenv CONTOUR_PROMPT_TEXT | sed -e 's/ @@contour@@/@@contour@@/' -e 's/@@contour@@//'`%{\0337\033[C\033]133;B\007\0338%} "
        unsetenv CONTOUR_PROMPT_TEXT
    endif
endif
```

- [ ] **Step 4: Run them to see them pass**

On Linux: `cmake --build --preset gcc-debug --target contour_test && out/gcc-debug/src/contour/contour_test "[shells][tcsh]"`
Expected: `All tests passed` (2 test cases). If only `record->commandLine == launch.command` fails — the `B` arrived, but what phase 1 recovered differs from the typed command — print `core::escape(record->commandLine)` via `INFO`: a leading `"> "` or space means the column is off (fix the literal here); anything else is phase 1's recovery, to be reported to the coordinator rather than patched here.

- [ ] **Step 5: Commit**

Run: `clang-format -i src/contour/cli/ShellIntegrationShells_test.cpp`

```bash
git add src/contour/cli/shell-integration/shell-integration.tcsh src/contour/cli/ShellIntegrationShells_test.cpp
git commit -F - <<'EOF'
shell-integration: mark tcsh's prompt end at the input column

tcsh drops a trailing %{...%} literal and draws any other right before the
character after it, so the mark goes in front of the prompt's trailing space
and steps over it (DECSC, CUF, OSC 133;B, DECRC). Its column is exact, which
is what lets the terminal read tcsh's command line back off the screen.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.9: PowerShell (only if Task 3.1 passed)

**Files:**
- Create: `src/contour/cli/shell-integration/shell-integration.pwsh`
- Modify: `src/contour/cli/CMakeLists.txt` (line 14, `CONTOUR_SHELL_INTEGRATION_SHELLS`)
- Modify: `src/contour/cli/ShellIntegration_test.cpp`
- Modify: `src/contour/cli/ShellIntegrationShells_test.cpp`

**Interfaces:**
- Consumes: Task 3.1's verdict; `EmbedShellIntegration.cmake` (unchanged: the new row and its `ShellIntegrationPwsh` symbol follow from the list); Task 3.5's harness.
- Produces: `contour generate integration shell pwsh to FILE`; a fifth row of `contour::cli::supportedShells()`; the e2e test `contour_e2e_shell_integration` now compares five scripts.

**If Task 3.1 found that ConPTY swallows OSC 133, skip this task entirely** (the coordinator has been told; spec §16.1).

The script (finding 4): `prompt` is wrapped — `D` with the exit status (`$?`, and `$LASTEXITCODE` when a native program failed), DECRST 2028, OSC 7 when the location is a file-system path, `A`, the original prompt (seeing the `$?` it would have seen), `B`. `PSConsoleHostReadLine` is wrapped so that DECSET 2028 and `C;cmdline_url=` go out after PSReadLine accepted the line and moved to the next one. `D` follows only a `C`: without PSReadLine there is neither, and the terminal sees prompts but no commands. Escapes are spelled `[char]0x1b`, not `` `e ``.

- [ ] **Step 1: Write the failing tests**

In `src/contour/cli/ShellIntegration_test.cpp` append:

```cpp
TEST_CASE("the PowerShell integration is compiled in", "[shell-integration]")
{
    // Its own name, pwsh, as the executable is called: `contour generate integration shell pwsh`.
    auto const script = contour::cli::shellIntegrationScript("pwsh");
    REQUIRE(script.has_value());
    CHECK(script->contains("PSConsoleHostReadLine"));
}
```

In `src/contour/cli/ShellIntegrationShells_test.cpp`, add after the definition of `PosixCommand`:

```cpp
/// The command line typed at PowerShell's prompt: the same characters as PosixCommand.
constexpr auto PwshCommand = "Write-Output \"a  b;c\" 'd\"e' 50% \xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80"sv;

#ifdef _WIN32
/// PowerShell's executable, as PATH has it.
constexpr auto PwshProgram = "pwsh.exe"sv;
#else
/// PowerShell's executable, as PATH has it.
constexpr auto PwshProgram = "pwsh"sv;
#endif
```

add this row to `shellLaunches()`, after the tcsh row:

```cpp
        ShellLaunch {
            .shell = "pwsh",
            .program = std::string { PwshProgram },
            .scriptFile = "shell-integration.ps1", // PowerShell runs only *.ps1 files
            .startupFile = "",
            .startupText = "",
            .arguments = { "-NoLogo", "-NoProfile", "-NoExit", "-Command", ". '${SCRIPT}'" },
            .command = std::string { PwshCommand },
            .commandLineSource = vtbackend::CommandLineSource::Reported,
        },
```

and append:

```cpp
TEST_CASE("PowerShell integration reports prompts, the command line and its end",
          "[shell-integration][shells][pwsh]")
{
    // On Windows this runs through ConPTY, whose passing OSC 133 on is what vtpty_test's
    // ConPty.passesOsc133Through establishes.
    auto const session = start(launchFor("pwsh"));
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launchFor("pwsh"));
}

TEST_CASE("PowerShell integration dot-sourced twice wraps once", "[shell-integration][shells][pwsh]")
{
    auto launch = launchFor("pwsh");
    launch.arguments.back() = ". '${SCRIPT}'; . '${SCRIPT}'";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launch);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target contour_test` then `out/build/clangcl-debug/bin/contour_test.exe "[shell-integration]"`
Expected: FAIL — `the PowerShell integration is compiled in` (`REQUIRE( script.has_value() )`), and the pwsh cases fail in `prepare()` on the same `REQUIRE` (or report `skipped` where pwsh is not installed).

- [ ] **Step 3: Write the script**

Create `src/contour/cli/shell-integration/shell-integration.pwsh`:

```powershell
# vim:et:ts=4:sw=4
#
#// SPDX-License-Identifier: Apache-2.0
#
# Contour shell integration for PowerShell 7 (pwsh).
#
# PowerShell runs a script file only when its name ends in .ps1, so write it to one, and dot-source that
# from your profile ($PROFILE) after anything that sets up your prompt (oh-my-posh, starship):
#
#     contour generate integration shell pwsh to "$(Split-Path $PROFILE)/contour-integration.ps1"
#     . "$(Split-Path $PROFILE)/contour-integration.ps1"
#
# The prompt reports, in order:
#   OSC 133;D    -- the command that just ran has finished, with its exit status,
#   DECRST 2028  -- text reflow off for the prompt,
#   OSC 7        -- the working directory, when the location is a file-system path,
#   OSC 133;A    -- a new prompt starts on this line,
#   ...your own prompt...,
#   OSC 133;B    -- the prompt ends here and typed input begins.
# PSReadLine's PSConsoleHostReadLine is wrapped so that once a line is accepted -- and PSReadLine has
# moved on to the next line -- DECSET 2028 and OSC 133;C go out, C with the command line,
# percent-encoded as kitty's cmdline_url=. VS Code's shellIntegration.ps1 hooks in at the same place. An
# Enter key handler would be too early: it runs before PSReadLine writes the newline, and would mark the
# prompt line as the command's output. Without PSReadLine there is no C, and so no D either.

# Installed once per session: dot-sourcing this again would wrap the wrappers.
if ($null -ne $Global:__ContourState) {
    return
}

$Global:__ContourState = @{
    OriginalPrompt   = $function:prompt
    OriginalReadLine = $null
    CommandRunning   = $false
    # The longest command line sent as cmdline_url=, in bytes: a little more than the terminal keeps
    # (vtbackend::MaxRecordedCommandLineBytes, 4096), so that it still sees a longer one was cut.
    MaxCommandLineBytes = 4100
    # What fish's encoder leaves alone, too; every other UTF-8 byte becomes %XX. That keeps ';', which
    # separates OSC 133's parameters, and the ESC and BEL that would end the sequence out of it.
    Unreserved = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/._~-'
}

function Global:__Contour-Osc([string]$payload) {
    # OSC <payload> ST.
    "$([char]0x1b)]$payload$([char]0x1b)\"
}

function Global:__Contour-PercentEncode([string]$text) {
    $state = $Global:__ContourState
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    $count = [Math]::Min($bytes.Length, $state.MaxCommandLineBytes)
    $encoded = [System.Text.StringBuilder]::new()
    for ($i = 0; $i -lt $count; $i++) {
        $octet = $bytes[$i]
        if ($octet -lt 0x80 -and $state.Unreserved.IndexOf([char]$octet) -ge 0) {
            [void]$encoded.Append([char]$octet)
        } else {
            [void]$encoded.Append('%').Append($octet.ToString('X2'))
        }
    }
    $encoded.ToString()
}

function Global:__Contour-ExitCode([bool]$succeeded, $nativeExitCode, $lastHistoryEntry) {
    if ($succeeded) {
        return 0
    }
    # $? is false after a native program that exited non-zero and after a PowerShell error alike. An
    # error recorded against the command that just ran is the latter, and $LASTEXITCODE -- whatever the
    # last native program left behind -- says nothing about it.
    $isPowerShellError = $null -ne $lastHistoryEntry -and $Error.Count -gt 0 -and
        $Error[0].InvocationInfo.HistoryId -eq $lastHistoryEntry.Id
    if ($isPowerShellError -or -not $nativeExitCode) {
        return 1
    }
    return $nativeExitCode
}

function Global:prompt {
    # First, before anything overwrites them: how the command that just finished went.
    $succeeded = $global:?
    $nativeExitCode = $global:LASTEXITCODE
    Set-StrictMode -Off
    $state = $Global:__ContourState

    $out = ''
    if ($state.CommandRunning) {
        $state.CommandRunning = $false
        $exitCode = __Contour-ExitCode $succeeded $nativeExitCode (Get-History -Count 1)
        $out += __Contour-Osc "133;D;$exitCode"
    }
    $out += "$([char]0x1b)[?2028l"
    $location = $executionContext.SessionState.Path.CurrentLocation
    if ($location.Provider.Name -eq 'FileSystem') {
        $out += __Contour-Osc "7;$($location.ProviderPath)"
    }
    $out += __Contour-Osc '133;A'

    # The original prompt sees the $? it would have seen without this wrapper.
    if (-not $succeeded) {
        Write-Error 'failure' -ErrorAction Ignore
    }
    $out += & $state.OriginalPrompt
    $out += __Contour-Osc '133;B'
    $out
}

if (Get-Module -Name PSReadLine) {
    $Global:__ContourState.OriginalReadLine = $function:PSConsoleHostReadLine

    function Global:PSConsoleHostReadLine {
        $line = & $Global:__ContourState.OriginalReadLine
        # PSReadLine has drawn the accepted line and moved to the next one: the command's output starts
        # where the cursor is now. An empty line runs nothing, so it starts no output either.
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            $Global:__ContourState.CommandRunning = $true
            $commandLine = __Contour-PercentEncode $line
            [Console]::Write("$([char]0x1b)[?2028h" + (__Contour-Osc "133;C;cmdline_url=$commandLine"))
        }
        $line
    }
}
```

- [ ] **Step 4: Register it**

In `src/contour/cli/CMakeLists.txt` change line 14 to:

```cmake
set(CONTOUR_SHELL_INTEGRATION_SHELLS bash fish pwsh tcsh zsh)
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target contour_test` then `out/build/clangcl-debug/bin/contour_test.exe "[shell-integration]"`
Expected: `the PowerShell integration is compiled in`, `the embedded scripts are the ones in the source tree` and `supportedShellsText names exactly the compiled-in shells` pass; the two pwsh cases pass where `pwsh.exe` is installed (else `skipped`); the POSIX cases are `skipped`.
On Linux with pwsh installed (GitHub's `ubuntu-24.04` image has it): `out/gcc-debug/src/contour/contour_test "[shells][pwsh]"` → `All tests passed`.
If the `contour` target builds here: `cmake --build --preset clangcl-debug --target contour` then `ctest --test-dir out/build/clangcl-debug -R contour_e2e_shell_integration --output-on-failure` → `shell-integration: OK (5 shells)`. Otherwise CI is the oracle for that test (README, build notes).

- [ ] **Step 6: Commit**

Run: `clang-format -i src/contour/cli/ShellIntegration_test.cpp src/contour/cli/ShellIntegrationShells_test.cpp`

```bash
git add src/contour/cli/shell-integration/shell-integration.pwsh src/contour/cli/CMakeLists.txt \
        src/contour/cli/ShellIntegration_test.cpp src/contour/cli/ShellIntegrationShells_test.cpp
git commit -F - <<'EOF'
shell-integration: add a PowerShell integration

`contour generate integration shell pwsh` wraps prompt -- OSC 133;D with the
exit status, OSC 7 for file-system locations, A, the original prompt, B -- and
PSConsoleHostReadLine, which sends OSC 133;C with the command line as
cmdline_url= once PSReadLine has accepted the line and moved on, as VS Code's
integration does. An Enter key handler would mark the prompt line as output.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.10: Natively integrating shells (Nushell) — help text and the facts phase 10 documents

**Files:**
- Modify: `src/contour/cli/ShellIntegration.hpp`
- Modify: `src/contour/cli/ShellIntegration.cpp`
- Modify: `src/contour/cli/ContourApp.cpp` (`integrationShellHelpText()`, lines 228-238; `integrationAction()`, lines 553-560)
- Modify: `src/contour/cli/ShellIntegration_test.cpp`
- Modify: `README.md` (lines 88-89)
- Modify: `src/contour/command/ContextMenu.cpp` (comment at lines 138-143)

**Interfaces:**
- Consumes: `supportedShells()`, `supportedShellsText()`, `core::joinHumanReadable` (`Utils.hpp:207`).
- Produces (addition):

```cpp
namespace contour::cli {
struct NativeShellIntegrationRow { std::string_view name {}; std::string_view displayName {}; std::string_view howToEnable {}; };
[[nodiscard]] std::span<NativeShellIntegrationRow const> nativeShellIntegrations() noexcept;
[[nodiscard]] std::string_view nativeShellsText();
[[nodiscard]] std::string unsupportedShellMessage(std::string_view shell);
}
```

Nushell gets no script (spec §10.3). What it gets is a row in a table, so that `contour generate integration shell nu` says how to turn Nushell's own integration on instead of "unsupported shell", and the verb's help names it — the website page itself is phase 10's (Task 10.8), which reads the facts recorded in this phase's commit bodies.

- [ ] **Step 1: Write the failing tests**

In `src/contour/cli/ShellIntegration_test.cpp` add `using namespace std::string_view_literals;` after the includes and append:

```cpp
TEST_CASE("natively integrating shells are named, not scripted", "[shell-integration]")
{
    auto const natives = contour::cli::nativeShellIntegrations();
    REQUIRE(!natives.empty());
    CHECK(std::ranges::contains(natives, "nu"sv, &contour::cli::NativeShellIntegrationRow::name));

    for (auto const& row: natives)
    {
        INFO("shell: " << row.name);
        // Scripted or native, never both: a script for a shell that marks its prompts itself would
        // mark every one of them twice.
        CHECK(!contour::cli::shellIntegrationScript(row.name).has_value());
        CHECK(contour::cli::nativeShellsText().contains(row.displayName));

        auto const message = contour::cli::unsupportedShellMessage(row.name);
        CHECK(message.contains(row.howToEnable));
        CHECK(message.ends_with('\n'));
    }
}

TEST_CASE("unsupportedShellMessage names the supported shells for an unknown one", "[shell-integration]")
{
    auto const message = contour::cli::unsupportedShellMessage("nosuchshell");
    CHECK(message.contains("unsupported shell, nosuchshell"));
    CHECK(message.contains(contour::cli::supportedShellsText()));
    CHECK(message.ends_with('\n'));
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `error C2039: 'nativeShellIntegrations': is not a member of 'contour::cli'`.

- [ ] **Step 3: Declare the native-shell table**

In `src/contour/cli/ShellIntegration.hpp` add `#include <string>` to the includes, and before the closing `} // namespace contour::cli` add:

```cpp
/// A shell that marks its prompts and commands with OSC 133 by itself, so Contour ships no script for it.
struct NativeShellIntegrationRow
{
    std::string_view name {};        ///< Shell name, as a user would pass it to `generate integration shell`.
    std::string_view displayName {}; ///< How the shell names itself, e.g. "Nushell".
    std::string_view howToEnable {}; ///< One sentence on what turns its integration on.
};

/// Every shell that integrates natively. No name here is also in supportedShells(): a shell given a
/// script as well would mark every prompt twice.
/// @return The table.
[[nodiscard]] std::span<NativeShellIntegrationRow const> nativeShellIntegrations() noexcept;

/// The natively integrating shells for a human-readable list, e.g. "Nushell (nu)".
/// @return A view of storage with process lifetime, so callers may hold it.
[[nodiscard]] std::string_view nativeShellsText();

/// What `generate integration` tells a user who asked for a shell it carries no script for.
/// @param shell The shell name asked for.
/// @return For a natively integrating shell, how to turn its own integration on; otherwise which
///         shells are supported. Ends in a newline.
[[nodiscard]] std::string unsupportedShellMessage(std::string_view shell);
```

- [ ] **Step 4: Implement it**

In `src/contour/cli/ShellIntegration.cpp` add `#include <array>` and `#include <format>` to the standard includes, and before the closing `} // namespace contour::cli` add:

```cpp
namespace
{
    /// Shells that speak OSC 133 by themselves (docs/drafts/semantic-blocks.md §10.3, §16.1).
    constexpr auto NativeShellIntegrationTable = std::array {
        NativeShellIntegrationRow {
            .name = "nu",
            .displayName = "Nushell",
            .howToEnable = "Nushell 0.111 or later marks its prompts and commands itself while "
                           "$env.config.shell_integration.osc133 is true, which is the default.",
        },
    };
} // namespace

std::span<NativeShellIntegrationRow const> nativeShellIntegrations() noexcept
{
    return NativeShellIntegrationTable;
}

std::string_view nativeShellsText()
{
    // Function-local static for the reason supportedShellsText() gives: a help string borrows.
    static std::string const text =
        core::joinHumanReadable(nativeShellIntegrations() | std::views::transform([](NativeShellIntegrationRow const& row) {
                                    return std::format("{} ({})", row.displayName, row.name);
                                }));
    return text;
}

std::string unsupportedShellMessage(std::string_view shell)
{
    auto const natives = nativeShellIntegrations();
    if (auto const row = std::ranges::find(natives, shell, &NativeShellIntegrationRow::name); row != natives.end())
        return std::format("{} needs no integration script from Contour. {}\n", row->displayName, row->howToEnable);

    return std::format("Cannot generate shell integration for an unsupported shell, {}. Supported shells: {}.\n",
                       shell,
                       supportedShellsText());
}
```

In `src/contour/cli/ContourApp.cpp`, in `integrationShellHelpText()`, replace

```cpp
        static std::string const text =
            std::format("Shell name to create the integration for. Supported shells: {}",
                        contour::cli::supportedShellsText());
```

with

```cpp
        static std::string const text =
            std::format("Shell name to create the integration for. Supported shells: {}. Integrating "
                        "natively, needing none: {}.",
                        contour::cli::supportedShellsText(),
                        contour::cli::nativeShellsText());
```

and in `integrationAction()` replace

```cpp
            std::cerr << std::format(
                "Cannot generate shell integration for an unsupported shell, {}. Supported shells: {}.\n",
                shell,
                supportedShellsText());
```

with

```cpp
            std::cerr << unsupportedShellMessage(shell);
```

- [ ] **Step 5: Bring the two stale statements in line**

In `README.md` replace

```text
directory. Some features also require shell integration. These can be generated
via the CLI (see below), these currently exist for zsh, fish and tcsh.
```

with

```text
directory. Some features also require shell integration. These can be generated
via the CLI (see below) for bash, zsh, fish, tcsh and PowerShell (`pwsh`);
Nushell integrates natively.
```

(If Task 3.9 was held back, drop ` and PowerShell (\`pwsh\`)` and write `bash, zsh, fish and tcsh`.)

In `src/contour/command/ContextMenu.cpp` replace the comment lines

```cpp
            // two-line banner of a powerlevel10k). The bare command exists only when the shell sends it
            // explicitly (OSC 133;C cmdline_url), which none of the bundled integrations do. A row named
            // "Copy Last Command" that hands back a prompt is a row that lies.
```

with

```cpp
            // two-line banner of a powerlevel10k). The bare command exists only when the shell sends it
            // (OSC 133;C cmdline_url, which the bundled bash, zsh, fish and PowerShell integrations do)
            // or the terminal reads it back from the grid -- and these rows copy lines, not records. A
            // row named "Copy Last Command" that hands back a prompt is a row that lies.
```

(If Task 3.9 was held back, write `bash, zsh and fish`.)

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target contour_test` then `out/build/clangcl-debug/bin/contour_test.exe "[shell-integration]"`
Expected: `All tests passed` apart from the `skipped` shell cases. If `contour` builds here: `out/build/clangcl-debug/bin/contour.exe generate integration shell nu to -` prints `Nushell needs no integration script from Contour. …` to stderr and exits with status 1; `ctest --test-dir out/build/clangcl-debug -R "contour_e2e_(shell_integration|cli_verbs)" --output-on-failure` passes.

- [ ] **Step 7: Commit**

Run: `clang-format -i src/contour/cli/ShellIntegration.hpp src/contour/cli/ShellIntegration.cpp src/contour/cli/ContourApp.cpp src/contour/cli/ShellIntegration_test.cpp src/contour/command/ContextMenu.cpp`

```bash
git add src/contour/cli/ShellIntegration.hpp src/contour/cli/ShellIntegration.cpp src/contour/cli/ContourApp.cpp \
        src/contour/cli/ShellIntegration_test.cpp src/contour/command/ContextMenu.cpp README.md
git commit -F - <<'EOF'
cli: name the shells that integrate natively

Spec §16.1 verification, from nushell's prompt_update.rs and reedline's
semantic_prompt.rs: with shell_integration.osc133 (the default) Nushell sends
133;A;k=i before the prompt, 133;A;k=s before every continuation line,
133;P;k=r before the right prompt, 133;B, a bare 133;C (no cmdline_url) and
133;D;<exit>; the k= markers arrived in 0.111.0. It needs no script, so
`generate integration shell nu` now says how to turn its own on, and the
verb's help lists it.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.11: CI runs every shell, and fails when one skipped

**Files:**
- Modify: `.github/workflows/build.yml` (job `ubuntu_2404_cc_matrix`: after the step `install tmux (oracle for the tmux-interop tests)`, lines 1137-1167; after the step `verify the tmux oracles actually ran`, lines 1273-1296)

**Interfaces:**
- Consumes: the `[shells]` test cases of Tasks 3.5–3.9; the `gcc-debug` tree at `out/gcc-debug`.
- Produces: two workflow steps.

A Catch2 SKIP exits 0, so a green suite cannot tell "the scripts work" from "no shell was installed" — the tmux oracles of the same job went their whole existence without running for exactly that reason (the comment at `build.yml:1274-1281`). PowerShell is excluded from the verification: the `ubuntu-24.04-arm` image need not carry it.

- [ ] **Step 1: Install the shells**

In `.github/workflows/build.yml`, in job `ubuntu_2404_cc_matrix`, insert directly after the line `          tmux -V` that ends the step `install tmux (oracle for the tmux-interop tests)` (line 1167; the macOS job has a step of the same name at line 696 — not that one):

```yaml
      - name: "install the shells the shell-integration tests drive"
        # contour_test runs each bundled integration script in its own shell on a real PTY, and SKIPs
        # a shell that is not installed. bash is on the image already; pwsh is where the image has it.
        run: sudo apt -qy install zsh fish tcsh
```

- [ ] **Step 2: Fail when one of them skipped**

In the same job, insert directly before `      - name: "session OOM isolation under a real systemd user manager"` (line 1297):

```yaml
      - name: "verify the shell-integration tests actually ran"
        # @see "verify the tmux oracles actually ran": a Catch2 SKIP exits 0, so a green suite cannot
        # tell "every script works in its shell" from "no shell was installed". PowerShell is left out:
        # not every runner image carries it.
        run: |
          set -o pipefail
          binary=out/gcc-debug/src/contour/contour_test
          output=$("$binary" '[shells]~[pwsh]' 2>&1) || { printf '%s\n' "$output"; exit 1; }
          printf '%s\n' "$output"
          if printf '%s' "$output" | grep -q 'skipped'; then
            echo "::error::$binary [shells] reported SKIPPED; a shell the integration scripts target is not being exercised"
            exit 1
          fi
```

- [ ] **Step 3: Check the workflow**

Run: `ctest --test-dir out/build/clangcl-debug -R check_workflows --output-on-failure` (actionlint; SKIP when not installed — the CI lint job is then the oracle).
Expected: PASS or SKIP.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/build.yml
git commit -F - <<'EOF'
ci: run the shell-integration scripts in their shells, and insist on it

Installs zsh, fish and tcsh beside bash on the Ubuntu matrix and fails the
job when contour_test's [shells] cases (PowerShell aside) report a skip.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 3.12: Phase gate

**Files:** whatever `/simplify` and the review change.

- [ ] **Step 1: Build clean, test green**

Run: `cmake --build --preset clangcl-debug --target vtpty_test vtbackend_test contour_test contour 2>&1 | Select-String -Pattern "warning"` (PowerShell) — expected: no output. If `contour` fails on the pre-existing `yaml-cpp/emitter.h` problem (README), build the three test targets alone and leave `contour_e2e_*` to CI.
Run: `ctest --test-dir out/build/clangcl-debug --output-on-failure`
Expected: every test that passed in the phase-0 baseline still passes, plus `vtbackend_test`'s new `[sanitize]`/`[osc133]`/`[cmdline_url]` cases, `vtpty_test`'s `ConPty.passesOsc133Through` and `contour_test`'s new cases (shells other than `pwsh` reported as skipped on this box). Note the `N tests passed, M failed` line.
Run: `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure` — expected PASS (or SKIP without `typos`).
Push the branch and confirm on the CI run: the `ubuntu_2404_cc_matrix` jobs' new step `verify the shell-integration tests actually ran` is green, the macOS job's `ctest --preset macos-package` (bash 3.2, zsh, tcsh) is green, and the Windows job (ConPTY probe, pwsh) is green.

- [ ] **Step 2: Audit what clang-tidy cannot reach on this box**

Run (Git Bash): `git diff $PHASE3_START..HEAD -- '*.cpp' '*.hpp' | grep '^+' | grep -v '^+ *//' | grep -Ew 'small|near|far|min|max|interface|boolean|hyper|byte'`
Expected: no IDENTIFIER named after a Windows SDK macro (README); hits inside comments or string literals ("byte's" in CommandLineSanitizer.cpp's ///< comments, "a lone continuation byte" in its test) are fine. Then read every added C++ line of `src/contour/**` once more for `misc-use-internal-linkage` (file-scope helpers inside the anonymous namespace), `misc-const-correctness` and `bugprone-implicit-widening-of-multiplication-result` — the CI clang-tidy job is the only oracle for them.

- [ ] **Step 3: Simplify**

Run `/simplify` over `git diff $PHASE3_START..HEAD`. Apply what it finds; rebuild and rerun Step 1's `ctest`; commit:

```bash
git add -u
git commit -F - <<'EOF'
shell-integration: simplify what phase 3 added

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(No changes → no commit.)

- [ ] **Step 4: Review at xhigh**

Run `/code-review xhigh` on `$PHASE3_START..HEAD` (or dispatch a review subagent with `effort: "xhigh"`), pointing it at README Review Focus item 3 (hostile command lines: 64 KiB `cmdline_url`, embedded `\e[201~`, newlines, bidi overrides, invalid UTF-8) and at every script's quoting. Fix each confirmed finding, rerun Step 1's `ctest`, and commit with the suite's summary line in the body:

```bash
git add -u
git commit -F - <<'EOF'
shell-integration: address the phase 3 review

ctest: <the "N tests passed, M failed" line from Step 1, verbatim>

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(No findings → no commit; give the coordinator the summary line instead.)

- [ ] **Step 5: Report**

Report to the coordinating session (it records progress; this phase does not): the ctest summary line; Task 3.1's verdict and whether Task 3.9 shipped; the fish version Task 3.7 Step 5 checked the native path with (or that it was checked by the table only); and the three facts phase 10's website text must use — fish marks prompts natively **from 4.0** (A, C, D), reports the command line from **4.0.1** and the prompt end from **4.3**; Nushell from **0.111**, with no `cmdline_url`; OSC 133 does / does not survive Contour's ConPTY.
