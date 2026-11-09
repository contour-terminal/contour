// SPDX-License-Identifier: Apache-2.0

// clang-format off
#ifdef _WIN32
    #include <windows.h>
#endif
// clang-format on

#include <vtpty/ExecutablePath.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

#ifdef __APPLE__
    #include <mach-o/dyld.h>
#endif

#if defined(__FreeBSD__) || defined(__NetBSD__)
    #include <sys/sysctl.h>
    #include <sys/types.h>
#endif

#ifdef __linux__
    #include <unistd.h>
#endif

#include <cerrno>

namespace vtpty
{

namespace fs = std::filesystem;

namespace
{
    class SelfExecutableCategory final: public std::error_category
    {
      public:
        [[nodiscard]] char const* name() const noexcept override { return "vtpty.selfExecutable"; }

        [[nodiscard]] std::string message(int value) const override
        {
            switch (static_cast<SelfExecutableError>(value))
            {
                case SelfExecutableError::NoProcFileSystem:
                    return "/proc is not mounted, and programs are started through it";
                case SelfExecutableError::NoWayToName:
                    return "this platform has no way to name the running executable, through which "
                           "programs are started";
            }
            return "unknown error";
        }
    };
} // namespace

std::error_category const& selfExecutableCategory() noexcept
{
    static auto const category = SelfExecutableCategory {};
    return category;
}

std::error_code makeErrorCode(SelfExecutableError error) noexcept
{
    return std::error_code { static_cast<int>(error), selfExecutableCategory() };
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
    // FreeBSD among them: what relies on knowing the installation -- the daemon's own images, relocation,
    // bundled fonts -- has not been brought up there. selfExecutablePath() still knows the image.
    return std::unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

std::expected<std::string, std::error_code> selfExecutablePath()
{
#ifdef __linux__
    // Resolved by the spawned process, which still runs this image: the running file, even where an upgrade
    // replaced or removed it on disk. Checked here, where the error can still say that /proc is missing,
    // rather than as posix_spawn()'s ENOENT.
    constexpr auto Self = "/proc/self/exe";
    if (::access(Self, F_OK) != 0)
        return std::unexpected(makeErrorCode(SelfExecutableError::NoProcFileSystem));
    return std::string { Self };
#elif defined(__FreeBSD__) || defined(__NetBSD__)
    // The executable's path does not change while it runs: asked once, not on every spawn.
    static auto const cached = []() -> std::expected<std::string, std::error_code> {
    #ifdef __FreeBSD__
        auto name = std::array { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    #else
        auto name = std::array { CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME };
    #endif
        auto const nameLength = static_cast<u_int>(name.size());
        auto size = std::size_t { 0 };
        if (::sysctl(name.data(), nameLength, nullptr, &size, nullptr, 0) != 0)
            return std::unexpected(std::error_code { errno, std::generic_category() });
        auto buffer = std::string(size, '\0');
        if (::sysctl(name.data(), nameLength, buffer.data(), &size, nullptr, 0) != 0)
            return std::unexpected(std::error_code { errno, std::generic_category() });
        return std::string { buffer.c_str() }; // the size counted the terminating NUL
    }();
    return cached;
#else
    // The executable's path does not change while it runs: asked once, not on every spawn. OpenBSD has no
    // way to name it at all, and says so here.
    static auto const cached = currentExecutablePath()
                                   .transform([](fs::path const& path) { return path.string(); })
                                   .transform_error([](std::error_code const& error) {
                                       return error == std::errc::not_supported
                                                  ? makeErrorCode(SelfExecutableError::NoWayToName)
                                                  : error;
                                   });
    return cached;
#endif
}

} // namespace vtpty
