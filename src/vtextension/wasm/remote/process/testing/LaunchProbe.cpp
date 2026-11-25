// SPDX-License-Identifier: Apache-2.0
//
// extension_host_probe: started by the launcher's tests in the place of the extension host process, it
// reports what it was started with, on the control channel it was handed, and exits once that channel ends.
//
//   extension_host_probe --report <helper arguments> [--check-handle=H]…
//   extension_host_probe --parent <helper arguments> <helper executable> [its extra arguments]…
//
// Before it reports, it does to itself what the extension host does: it binds itself to its parent (exiting
// as the host does if that is not the process named), and caps its resources to its `--data-bytes`.
//
// With --parent, it is a hosting process of its own instead: bound to its parent likewise, it starts a
// supervisor and has it start the helper executable, as Contour does, and once the helper said Hello sends
// `helper=<pid>` and `end` lines on its control channel, and then waits for that channel to end -- or to be
// killed, which is what the tests of a helper outliving its parent do.
//
// The report is one `key=value` line each: `cwd=` its working directory, `env=NAME` per environment variable,
// `stdin=`, `stdout=` and `stderr=` each `null` if it is the null device, `terminal` if a terminal, else
// `other`; (POSIX) `pid=` its process id and `sid=` its session's; `caps=` `apply` or `skip` as
// the build caps, and (POSIX) `nproc=` and `data=` its limits, a number or `unlimited`; and `fd=N` per open
// descriptor (POSIX) or `handle=H:valid|invalid` per --check-handle (Windows); then `end`.
#include <core/Clock.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessHostConnector.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/RecordingLauncher.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/resource.h>
    #include <sys/stat.h>

    #include <ranges>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using vtextension::wasm::remote::transport::Channel;
using vtextension::wasm::remote::transport::parseHelperArguments;

namespace
{

/// Exit codes, each a way the probe failed before it reported.
enum class ProbeExit : uint8_t
{
    Reported = 0,     ///< It reported, and its control channel ended.
    Usage = 2,        ///< Its command line is not what it takes.
    NoControl = 3,    ///< Its control channel cannot be adopted.
    ReportFailed = 4, ///< The report could not be sent.
    ParentGone = 5,   ///< It is not its parent's child: it exits as the host does then.
    NoHelper = 6,     ///< With --parent: no helper could be started.
};

/// The prefix of the option naming a handle whose validity is reported.
constexpr auto CheckHandle = std::string_view { "--check-handle=" };

/// @return The names of the variables of @p environment (POSIX: main's, null-terminated; Windows: this
///         process's), one line each.
[[nodiscard]] std::string environmentLines([[maybe_unused]] char const* const* environment)
{
    auto lines = std::string {};
#ifdef _WIN32
    auto* const block = ::GetEnvironmentStringsW();
    for (auto const* entry = block; *entry != L'\0'; entry += std::wstring_view { entry }.size() + 1)
    {
        auto const text = std::wstring_view { entry };
        // `=C:=C:\…`: the per-drive working directories, which are no variables.
        if (text.starts_with(L'='))
            continue;
        auto const name = std::filesystem::path { text.substr(0, text.find(L'=')) }.string();
        lines += std::format("env={}\n", name);
    }
    ::FreeEnvironmentStringsW(block);
#else
    for (auto const* const* entry = environment; *entry != nullptr; ++entry)
    {
        auto const text = std::string_view { *entry };
        lines += std::format("env={}\n", text.substr(0, text.find('=')));
    }
#endif
    return lines;
}

/// The standard streams the report names, as the system numbers them.
enum class StandardStream : uint8_t
{
    Input,
    Output,
    Error,
};

/// @return What @p stream is: `null` for the null device, `terminal` for a terminal (a console on Windows),
///         else `other`.
[[nodiscard]] std::string_view deviceOf(StandardStream stream)
{
#ifdef _WIN32
    constexpr auto Handles = std::array<DWORD, 3> { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    auto* const handle = ::GetStdHandle(Handles[std::to_underlying(stream)]);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || ::GetFileType(handle) != FILE_TYPE_CHAR)
        return "other";
    // NUL is a character device, and unlike a console it has no console mode.
    auto mode = DWORD { 0 };
    return ::GetConsoleMode(handle, &mode) ? "terminal" : "null";
#else
    constexpr auto Descriptors = std::array { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    auto const descriptor = Descriptors[std::to_underlying(stream)];
    struct stat opened {};
    struct stat nullDevice {};
    if (::fstat(descriptor, &opened) == 0 && ::stat("/dev/null", &nullDevice) == 0
        && opened.st_dev == nullDevice.st_dev && opened.st_ino == nullDevice.st_ino)
        return "null";
    return ::isatty(descriptor) == 1 ? "terminal" : "other";
#endif
}

/// @return Its process and session, one line each (POSIX); nothing on Windows, which has no sessions.
[[nodiscard]] std::string sessionLines()
{
#ifdef _WIN32
    return {};
#else
    return std::format("pid={}\nsid={}\n", ::getpid(), ::getsid(0));
#endif
}

/// @return The open handles, one line each: every descriptor (POSIX), or the validity of each of @p checked
///         (Windows).
[[nodiscard]] std::string handleLines([[maybe_unused]] std::span<std::string_view const> checked)
{
    auto lines = std::string {};
#ifdef _WIN32
    for (auto const value: checked)
    {
        auto number = uintptr_t {};
        std::from_chars(value.data(), value.data() + value.size(), number);
        auto flags = DWORD { 0 };
        auto const isValid = ::GetHandleInformation(std::bit_cast<HANDLE>(number), &flags) != FALSE;
        lines += std::format("handle={}:{}\n", value, isValid ? "valid" : "invalid");
    }
#else
    // Asked of each number rather than listed from /proc/self/fd or /dev/fd, whose listing holds a descriptor
    // of its own. The launcher's children start with few, and low, numbers; well below this bound.
    constexpr auto Highest = 4096L;
    auto const openMax = ::sysconf(_SC_OPEN_MAX);
    auto const limit = openMax > 0 ? std::min(openMax, Highest) : Highest;
    for (auto const descriptor: std::views::iota(0, static_cast<int>(limit)))
        if (::fcntl(descriptor, F_GETFD) != -1)
            lines += std::format("fd={}\n", descriptor);
#endif
    return lines;
}

/// @return The caps it runs under, one line each.
[[nodiscard]] std::string capLines()
{
    namespace host = vtextension::wasm::host;
    auto lines = std::format("caps={}\n", host::BuildCapPolicy == host::CapPolicy::Apply ? "apply" : "skip");
#ifndef _WIN32
    auto const limitText = [](int resource) {
        struct rlimit limit {};
        if (::getrlimit(resource, &limit) != 0)
            return std::string { "unknown" };
        return limit.rlim_cur == RLIM_INFINITY ? std::string { "unlimited" } : std::to_string(limit.rlim_cur);
    };
    lines += std::format("nproc={}\ndata={}\n", limitText(RLIMIT_NPROC), limitText(RLIMIT_DATA));
#endif
    return lines;
}

/// Reports what the probe was started with, then waits for its control channel to end.
[[nodiscard]] ProbeExit report(std::span<std::string_view const> arguments, char const* const* environment)
{
    auto helperArguments = std::vector<std::string_view> {};
    auto checked = std::vector<std::string_view> {};
    for (auto const argument: arguments)
        if (argument.starts_with(CheckHandle))
            checked.push_back(argument.substr(CheckHandle.size()));
        else
            helperArguments.push_back(argument);
    auto const parsed = parseHelperArguments(helperArguments);
    if (!parsed)
        return ProbeExit::Usage;
    if (!vtextension::wasm::host::bindToParent(parsed->parentProcessId))
        return ProbeExit::ParentGone;
    auto control = Channel::adopt(parsed->control);
    if (!control)
        return ProbeExit::NoControl;
    std::ignore =
        vtextension::wasm::host::capResources(parsed->dataBytes, vtextension::wasm::host::BuildCapPolicy);

    auto const text = std::format("cwd={}\nstdin={}\nstdout={}\nstderr={}\n{}{}{}{}end\n",
                                  std::filesystem::current_path().string(),
                                  deviceOf(StandardStream::Input),
                                  deviceOf(StandardStream::Output),
                                  deviceOf(StandardStream::Error),
                                  sessionLines(),
                                  environmentLines(environment),
                                  capLines(),
                                  handleLines(checked));
    auto const bytes = std::span { reinterpret_cast<uint8_t const*>(text.data()), text.size() };
    if (!control->send(bytes))
        return ProbeExit::ReportFailed;

    // Whatever arrives is ignored; the end of the stream ends the probe.
    auto buffer = std::array<uint8_t, 64> {};
    while (control->receiveSome(buffer))
    {
    }
    return ProbeExit::Reported;
}

/// Starts a helper as Contour does, says which process it is, then waits for its control channel to end.
[[nodiscard]] ProbeExit parent(std::span<std::string_view const> arguments)
{
    namespace process = vtextension::wasm::remote::process;
    // The helper arguments are the `--name=value` ones; the helper executable is the first that is not.
    auto const firstOther = std::ranges::find_if(
        arguments, [](std::string_view argument) { return !argument.starts_with("--"); });
    if (firstOther == arguments.end())
        return ProbeExit::Usage;
    auto const parsed = parseHelperArguments({ arguments.begin(), firstOther });
    if (!parsed)
        return ProbeExit::Usage;
    if (!vtextension::wasm::host::bindToParent(parsed->parentProcessId))
        return ProbeExit::ParentGone;
    auto control = Channel::adopt(parsed->control);
    if (!control)
        return ProbeExit::NoControl;

    auto const recording =
        std::make_shared<process::testing::RecordingLauncher>(process::makeSystemProcessLauncher());
    auto supervisor = process::ExtensionHostSupervisor::start(process::SupervisorConfig {
        .launcher = recording, .clock = &core::defaultSteadyClock(), .teardownGrace = {} });
    if (!supervisor)
        return ProbeExit::NoHelper;
    auto connector = process::ProcessHostConnector {
        (*supervisor)->mailbox(),
        process::ProcessHostConfig { .helper = std::filesystem::path { *firstOther },
                                     .environment = process::defaultHelperEnvironment(),
                                     .extraArguments = { std::next(firstOther), arguments.end() },
                                     .caps = {},
                                     .standardError = std::nullopt },
        vtextension::wasm::remote::errorLogWriter()
    };
    auto connection = connector.connect();
    if (!connection || recording->processIds().empty())
        return ProbeExit::NoHelper;
    // Told only once the helper bound itself to this process: it says Hello after that, never before.
    auto helperControl = vtextension::wasm::remote::transport::PduStream {
        std::move(connection->control),
        vtextension::wasm::remote::proto::ChannelKind::Control,
        vtextension::wasm::remote::proto::Direction::ToParent,
        vtextension::wasm::remote::proto::PduLimits {}
    };
    auto const hello = helperControl.receive();
    if (!hello || !std::holds_alternative<vtextension::wasm::remote::proto::Hello>(*hello))
        return ProbeExit::NoHelper;

    auto const text = std::format("helper={}\nend\n", recording->processIds().front());
    if (!control->send(std::span { reinterpret_cast<uint8_t const*>(text.data()), text.size() }))
        return ProbeExit::ReportFailed;
    auto buffer = std::array<uint8_t, 64> {};
    while (control->receiveSome(buffer))
    {
    }
    return ProbeExit::Reported;
}

} // namespace

// The third parameter is what POSIX systems pass and Windows' C runtime passes too: the environment, read
// without a global.
int main(int argc, char const* argv[], char const* envp[])
{
    auto const arguments = std::vector<std::string_view>(argv + 1, argv + argc);
    if (arguments.empty() || (arguments.front() != "--report" && arguments.front() != "--parent"))
        return static_cast<int>(ProbeExit::Usage);
    auto const rest = std::span { arguments }.subspan(1);
    auto const exit = arguments.front() == "--report" ? report(rest, envp) : parent(rest);
    if (exit == ProbeExit::ParentGone)
        return vtextension::wasm::host::exitCodeOf(vtextension::wasm::host::HostProcessExit::ParentGone);
    return static_cast<int>(exit);
}
