// SPDX-License-Identifier: Apache-2.0
#include <vthost/InstalledExecutable.hpp>

#include <core/Utf8Path.hpp>

#include <array>
#include <utility>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <Windows.h>
#endif

namespace vthost
{

namespace
{
#ifdef _WIN32
    /// Reads the recorded folder from the registry's 64-bit view, where the installers write it.
    class RegistryInstallRootSource final: public InstallRootSource
    {
      public:
        [[nodiscard]] std::optional<std::string> recordedRoot(InstallRootHive hive) const override
        {
            auto const root = hive == InstallRootHive::LocalMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
            auto size = DWORD {};
            constexpr auto Flags = DWORD { RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY };
            if (::RegGetValueW(
                    root, L"Software\\Contour\\Contour", L"InstallRoot", Flags, nullptr, nullptr, &size)
                != ERROR_SUCCESS)
                return std::nullopt;
            auto text = std::wstring(size / sizeof(wchar_t), L'\0');
            if (::RegGetValueW(
                    root, L"Software\\Contour\\Contour", L"InstallRoot", Flags, nullptr, text.data(), &size)
                != ERROR_SUCCESS)
                return std::nullopt;
            text.resize(wcsnlen(text.c_str(), text.size()));
            return core::toUtf8(text);
        }
    };
#else
    /// The platforms without an installer that records anything.
    class NoInstallRootSource final: public InstallRootSource
    {
      public:
        [[nodiscard]] std::optional<std::string> recordedRoot(InstallRootHive) const override
        {
            return std::nullopt;
        }
    };
#endif
} // namespace

std::unique_ptr<InstallRootSource> makeNativeInstallRootSource()
{
#ifdef _WIN32
    return std::make_unique<RegistryInstallRootSource>();
#else
    return std::make_unique<NoInstallRootSource>();
#endif
}

std::filesystem::path installedExecutableOf(InstallRootSource const& roots,
                                            core::platform::FileSystem const& fileSystem)
{
    for (auto const hive: std::array { InstallRootHive::LocalMachine, InstallRootHive::CurrentUser })
    {
        auto const root = roots.recordedRoot(hive);
        if (!root || root->empty())
            continue;
        auto const executable = core::pathFromTrustedUtf8(*root) / "bin" / "contour.exe";
        if (fileSystem.exists(executable))
            return executable;
    }
    return {};
}

} // namespace vthost
