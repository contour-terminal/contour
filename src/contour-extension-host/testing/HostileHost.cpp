// SPDX-License-Identifier: Apache-2.0
//
// contour-extension-host-hostile: the extension host, misbehaving as `--hostile=<name>` says
// (HostileModes.hpp), and otherwise the real one. Started by the crash and hostility tests only; never
// installed.
#include <cstdio>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <contour-extension-host/testing/HostileModes.hpp>
#include <vtextension/wasm/host/HelperEngine.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>
#include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>

namespace transport = vtextension::wasm::remote::transport;
using contour::extension_host::testing::HostileMode;
using contour::extension_host::testing::hostileModeNamed;
using contour::extension_host::testing::HostileOption;

namespace
{

/// The exit code of a hostile host whose own command line is wrong: none of the real host's codes.
constexpr auto HostileSetupFailed = 76;

/// Ends the hostile host before it served, saying why.
[[nodiscard]] int refuse(char const* reason)
{
    std::println(stderr, "contour-extension-host-hostile: {}", reason);
    return HostileSetupFailed;
}

/// Runs the real host on @p arguments.
[[nodiscard]] int serve(std::vector<std::string> const& arguments)
{
    auto views = std::vector<std::string_view> {};
    for (auto const& argument: arguments)
        views.emplace_back(argument);
    return vtextension::wasm::host::runExtensionHost(views, &vtextension::wasm::host::makeHelperEngine);
}

/// Runs the real host with a wake signal of its own that nobody signals, in place of the one Contour hands
/// it: the host's listener never wakes, so the page's flags are never read. Contour's end stays open and
/// unread, as a stuck host's would.
[[nodiscard]] int ignoreInterrupts(std::vector<std::string_view> const& arguments)
{
    auto parsed = transport::parseHelperArguments(arguments);
    if (!parsed)
        return serve({ arguments.begin(), arguments.end() });
    auto silent = transport::makeWakePair();
    if (!silent)
        return refuse("no wake signal of its own can be made");
    auto const copy = transport::testing::duplicate(silent->helper.nativeHandle());
    if (!copy)
        return refuse("no wake signal of its own can be handed over");
    parsed->wake = *copy;
    return serve(transport::formatHelperArguments(*parsed));
}

} // namespace

int main(int argc, char const* argv[])
{
    auto arguments = std::vector<std::string_view> {};
    auto mode = std::optional<HostileMode> {};
    for (auto const argument: std::vector<std::string_view>(argv + 1, argv + argc))
    {
        if (!argument.starts_with(HostileOption))
        {
            arguments.push_back(argument);
            continue;
        }
        mode = hostileModeNamed(argument.substr(HostileOption.size()));
        if (!mode)
            return refuse("no such hostile mode");
    }
    if (!mode)
        return refuse("no hostile mode given");
    switch (*mode)
    {
        case HostileMode::IgnoreInterrupt: return ignoreInterrupts(arguments);
    }
    return refuse("no such hostile mode");
}
