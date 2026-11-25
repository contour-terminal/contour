// SPDX-License-Identifier: Apache-2.0
//
// extension_host_probe: started by the launcher's tests in the place of the extension host process, it
// reports what it was started with, on the control channel it was handed, and exits once that channel ends.
//
//   extension_host_probe --report <helper arguments> [--check-handle=H]…
//
// The report is one `key=value` line each: `cwd=` its working directory, `env=NAME` per environment variable,
// and `fd=N` per open descriptor (POSIX) or `handle=H:valid|invalid` per --check-handle (Windows); then
// `end`.
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <algorithm>
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
    auto control = Channel::adopt(parsed->control);
    if (!control)
        return ProbeExit::NoControl;

    auto const text = std::format("cwd={}\n{}{}end\n",
                                  std::filesystem::current_path().string(),
                                  environmentLines(environment),
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

} // namespace

// The third parameter is what POSIX systems pass and Windows' C runtime passes too: the environment, read
// without a global.
int main(int argc, char const* argv[], char const* envp[])
{
    auto const arguments = std::vector<std::string_view>(argv + 1, argv + argc);
    if (arguments.empty() || arguments.front() != "--report")
        return static_cast<int>(ProbeExit::Usage);
    return static_cast<int>(report(std::span { arguments }.subspan(1), envp));
}
