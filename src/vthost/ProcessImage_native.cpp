// SPDX-License-Identifier: Apache-2.0
/// @file
/// The operating system's half of DaemonBinary.hpp: the replacement of the process image. The process id
/// is ProcessId.cpp's, in vthost_image beside DaemonBinary.cpp, which asks for it.

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

} // namespace vthost
