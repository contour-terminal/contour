// SPDX-License-Identifier: Apache-2.0
//
// What a contained process can load, said library by library (Windows only): the loader probe, which imports
// nothing but kernel32, is started through the launcher Contour starts the extension host with, and loads
// what an executable that runs contained loads as it starts, in the order the loader initialises it. A
// library that cannot load is named with its Win32 error -- 1114 (ERROR_DLL_INIT_FAILED), 5
// (ERROR_ACCESS_DENIED), 126 (ERROR_MOD_NOT_FOUND) -- where the process importing it would only end with one
// status before its main().
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
    #include <algorithm>
    #include <array>
    #include <cstdint>
    #include <filesystem>
    #include <format>
    #include <memory>
    #include <optional>
    #include <ranges>
    #include <string>
    #include <string_view>
    #include <utility>
    #include <vector>

    #include <vtextension/wasm/remote/process/AppContainer_win32.hpp>
    #include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
    #include <vtextension/wasm/remote/process/Process_win32.hpp>
    #include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
    #include <vtextension/wasm/remote/process/testing/PeImports_win32.hpp>
    #include <vtextension/wasm/testing/Wine.hpp>

namespace process = vtextension::wasm::remote::process;
using process::ProcessIsolation;

namespace
{

/// The executables that run contained, as this suite knows them: the probe, and the extension host where the
/// suite starts one.
constexpr auto ContainedExecutables = std::to_array<char const*>({
    VTEXTENSION_PROBE_PATH,
    #ifdef VTEXTENSION_HELPER_PATH
    VTEXTENSION_HELPER_PATH,
    #endif
});

/// The prefix of an extension API set's name: one a system may lack, unlike an `api-ms-win-*` contract.
constexpr auto ExtensionApiSetPrefix = std::string_view { "ext-ms-win-" };

/// What one run of the loader probe is asked to load.
enum class LoadedLibraries : uint8_t
{
    None,              ///< Nothing: whether it starts at all.
    ExecutableImports, ///< What an executable of ContainedExecutables loads as it starts.
};

/// One run of the loader probe.
struct LoaderRun
{
    std::string_view what;      ///< What it shows.
    ProcessIsolation isolation; ///< What it is started in.
    LoadedLibraries libraries;  ///< What it loads.
};

/// Every run: each uncontained first, which shows the probe and the libraries load at all.
constexpr auto LoaderRuns = std::to_array<LoaderRun>({
    { .what = "uncontained, nothing",
      .isolation = ProcessIsolation::None,
      .libraries = LoadedLibraries::None },
    { .what = "uncontained, an executable's libraries",
      .isolation = ProcessIsolation::None,
      .libraries = LoadedLibraries::ExecutableImports },
    { .what = "contained, nothing",
      .isolation = ProcessIsolation::AppContainer,
      .libraries = LoadedLibraries::None },
    { .what = "contained, an executable's libraries",
      .isolation = ProcessIsolation::AppContainer,
      .libraries = LoadedLibraries::ExecutableImports },
});

/// @return @p path in UTF-8.
[[nodiscard]] std::string utf8Of(std::filesystem::path const& path)
{
    auto const utf8 = path.u8string();
    return std::string { utf8.begin(), utf8.end() };
}

/// @return The lines of @p text.
[[nodiscard]] std::vector<std::string> linesOf(std::string const& text)
{
    auto lines = std::vector<std::string> {};
    for (auto const piece: text | std::views::split('\n'))
    {
        auto line = std::string { piece.begin(), piece.end() };
        if (line.ends_with('\r'))
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
    }
    return lines;
}

/// @return The rest of the line of @p lines that starts with @p prefix; nothing if none does.
[[nodiscard]] std::optional<std::string> afterPrefix(std::vector<std::string> const& lines,
                                                     std::string_view prefix)
{
    auto const line =
        std::ranges::find_if(lines, [&](auto const& entry) { return entry.starts_with(prefix); });
    if (line == lines.end())
        return std::nullopt;
    return line->substr(prefix.size());
}

/// Starts the loader probe in @p isolation, through the launcher Contour uses, asked to load @p libraries;
/// checks that it ran to its end, loaded each, and exited cleanly; and says what it reported, on every run.
void checkLoaderRun(ProcessIsolation isolation, std::vector<std::filesystem::path> const& libraries)
{
    auto standardError = process::testing::CapturedStandardError::create();
    REQUIRE(standardError.has_value());
    auto refusals = std::vector<std::string> {};
    auto const launcher = process::makeWin32ProcessLauncher(
        process::systemAppContainerApi(),
        [&refusals](std::string_view line) { refusals.emplace_back(line); });
    auto arguments = std::vector<std::string> {};
    for (auto const& library: libraries)
        arguments.push_back(utf8Of(library));
    auto const launched = launcher->launch(process::LaunchSpec {
        .executable = VTEXTENSION_LOADER_PROBE_PATH,
        .arguments = [arguments](auto /*handles*/) { return arguments; },
        .environment = process::defaultHelperEnvironment(),
        .standardError = standardError->handle(),
        .isolation = isolation,
    });
    for (auto const& line: refusals)
        UNSCOPED_INFO(line);
    REQUIRE(launched.has_value());
    auto const status = process::testing::awaitReap(*launcher, **launched);
    REQUIRE(status.has_value());
    auto const text = standardError->text();
    // Said whether the run passes or fails: what a container lets a process load is the point of the run.
    WARN(std::format("the loader probe ({}): {}\n{}",
                     isolation == ProcessIsolation::AppContainer ? "contained" : "uncontained",
                     process::exitStatusText(*status),
                     text));
    CHECK(*status == process::ExitStatus { .kind = process::ExitKind::Exited, .value = 0 });
    auto const lines = linesOf(text);
    CHECK(std::ranges::contains(lines, std::string { "started" }));
    CHECK(afterPrefix(lines, "console=").has_value());
    for (auto const& library: arguments)
    {
        INFO(library);
        auto const outcome = afterPrefix(lines, std::format("library={} ", library)).value_or("(no line)");
        // An extension API set is optional: one missing on its own (ERROR_MOD_NOT_FOUND) does not keep a
        // process from starting, whose loader resolves it only where the system has it.
        if (library.starts_with(ExtensionApiSetPrefix) && outcome == "error:126")
            WARN(std::format("{}: not on this system, which a process starts without", library));
        else
            CHECK(outcome == "ok");
    }
    CHECK(std::ranges::contains(lines, std::string { "end" }));
}

} // namespace

TEST_CASE("LoaderProbe.importsKernel32Only", "[vtextension][appcontainer]")
{
    // It must start wherever a process can, so that what it says is about the libraries it is given.
    CHECK(process::testing::startupImportsOf(VTEXTENSION_LOADER_PROBE_PATH)
          == std::vector<std::string> { "kernel32.dll" });
}

TEST_CASE("LoaderProbe.knownDllsAreRead", "[vtextension][appcontainer]")
{
    // The import closure resolves a known DLL from the system directory alone: the list must be read.
    auto const known = process::testing::knownDlls();
    WARN(std::format("known DLLs read from the registry: {}", known.size()));
    if (!vtextension::wasm::testing::runsOnWine())
        CHECK(!known.empty());
}

TEST_CASE("LoaderProbe.containedProcessLoadsWhatItsExecutableImports", "[vtextension][appcontainer]")
{
    for (auto const& run: LoaderRuns)
    {
        INFO(run.what);
        // Wine stubs app containers: a contained run there would prove nothing.
        if (run.isolation == ProcessIsolation::AppContainer && vtextension::wasm::testing::runsOnWine())
            continue;
        switch (run.libraries)
        {
            case LoadedLibraries::None: checkLoaderRun(run.isolation, {}); break;
            case LoadedLibraries::ExecutableImports:
                for (auto const* const executable: ContainedExecutables)
                {
                    INFO(executable);
                    auto const closure = process::testing::importClosureOf(executable);
                    INFO("not found beside it nor in the system directory: "
                         << (closure.unresolved.empty() ? std::string { "none" }
                                                        : closure.unresolved.front()));
                    CHECK(closure.unresolved.empty());
                    // Read at all: every executable imports kernel32, and more.
                    CHECK(!closure.libraries.empty());
                    checkLoaderRun(run.isolation, closure.libraries);
                }
                break;
        }
    }
}
#endif
