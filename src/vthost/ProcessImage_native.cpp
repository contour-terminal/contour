// SPDX-License-Identifier: Apache-2.0
/// @file
/// The operating system's half of DaemonBinary.hpp: the running executable, the process id, and the
/// replacement of the process image.

// clang-format off
#ifdef _WIN32
    #include <windows.h>
#endif
// clang-format on

#include <cerrno>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <vthost/DaemonBinary.hpp>

#ifndef _WIN32
    #include <unistd.h>
#endif

#ifdef __APPLE__
    #include <mach-o/dyld.h>
#endif

namespace vthost
{

namespace fs = std::filesystem;

namespace
{
#ifndef _WIN32
    /// Replaces the process image by `execv`, which keeps the process id.
    class ExecReplacer final: public ProcessImageReplacer
    {
      public:
        [[nodiscard]] std::error_code replaceWith(fs::path const& image,
                                                  std::span<std::string const> argv) override
        {
            // execv takes `char* const[]` for historical reasons: mutable copies spare a const_cast.
            auto arguments = std::vector<std::string>(argv.begin(), argv.end());
            auto pointers = std::vector<char*> {};
            pointers.reserve(arguments.size() + 1);
            for (auto& argument: arguments)
                pointers.push_back(argument.data());
            pointers.push_back(nullptr);
            ::execv(image.c_str(), pointers.data());
            return { errno, std::generic_category() };
        }
    };
#else
    /// Windows has no exec that keeps the process: a daemon there is spawned from its image instead.
    class UnsupportedReplacer final: public ProcessImageReplacer
    {
      public:
        [[nodiscard]] std::error_code replaceWith(fs::path const&, std::span<std::string const>) override
        {
            return std::make_error_code(std::errc::not_supported);
        }
    };
#endif
} // namespace

std::unique_ptr<ProcessImageReplacer> makeNativeProcessImageReplacer()
{
#ifndef _WIN32
    return std::make_unique<ExecReplacer>();
#else
    return std::make_unique<UnsupportedReplacer>();
#endif
}

std::expected<fs::path, std::error_code> currentExecutablePath()
{
#ifdef __linux__
    auto ec = std::error_code {};
    auto path = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return std::unexpected(ec);
    return path;
#elifdef __APPLE__
    auto size = std::uint32_t { 0 };
    std::ignore = ::_NSGetExecutablePath(nullptr, &size);
    auto buffer = std::string(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0)
        return std::unexpected(std::make_error_code(std::errc::value_too_large));
    buffer = std::string { buffer.c_str() }; // the size counted the terminating NUL
    auto ec = std::error_code {};
    auto path = fs::canonical(buffer, ec); // it may name the executable through a symbolic link
    if (ec)
        return std::unexpected(ec);
    return path;
#elifdef _WIN32
    auto buffer = std::wstring(MAX_PATH, L'\0');
    while (true)
    {
        auto const length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return std::unexpected(
                std::error_code { static_cast<int>(::GetLastError()), std::system_category() });
        if (length < buffer.size())
        {
            buffer.resize(length);
            return fs::path { buffer };
        }
        buffer.resize(buffer.size() * 2); // truncated: GetModuleFileNameW does not say by how much
    }
#else
    return std::unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

std::uint64_t currentProcessId() noexcept
{
#ifdef _WIN32
    return ::GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

} // namespace vthost
