// SPDX-License-Identifier: Apache-2.0
//
// What starts contained needs no window station: the extension host, and the probe that stands in for it,
// import nothing that loads user32 (Windows only). Compiled into both helper suites, each of which
// reads its own extension host -- Contour's, on the build's engine, and the twin on WAMR's -- and the probe.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
    #include <algorithm>
    #include <array>
    #include <string>
    #include <string_view>

    #include <vtextension/wasm/remote/process/testing/PeImports_win32.hpp>

using vtextension::wasm::remote::process::testing::startupImportsOf;

namespace
{

/// What a process that may run contained does not load as it starts: user32, whose initialisation connects
/// to the process's window station -- which, a service's (an SSH session's, say), admits no app container,
/// so that the loader would end the process before its main() -- and the system libraries that load it
/// themselves. The extension host draws nothing, and needs none of them.
constexpr auto WindowStationLibraries = std::to_array<std::string_view>({
    "user32.dll",
    "gdi32.dll",
    "ole32.dll",
    "oleaut32.dll",
    "shell32.dll",
    "shlwapi.dll",
    "comctl32.dll",
});

} // namespace

TEST_CASE("StartupImports.containedExecutablesLoadNoWindowStation", "[vtextension][appcontainer]")
{
    // What starts contained -- the extension host, and the probe that stands in for it -- needs no window
    // station: where one does not admit app containers, a process that loads user32 ends in the loader.
    for (auto const* const executable: { VTEXTENSION_HELPER_PATH, VTEXTENSION_PROBE_PATH })
    {
        INFO(executable);
        auto const imports = startupImportsOf(executable);
        // Read at all: every executable imports kernel32.
        CHECK(std::ranges::contains(imports, std::string { "kernel32.dll" }));
        for (auto const& library: imports)
        {
            INFO(library);
            CHECK_FALSE(std::ranges::contains(WindowStationLibraries, std::string_view { library }));
        }
    }
}
#endif
