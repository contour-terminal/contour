// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only support for the Windows cases that need a volume of their own: a small virtual disk made,
/// formatted and attached through diskpart, and the rule for a case that needs the elevation diskpart
/// does. Beside the tests, because it needs Catch2; compiled into no module.

#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#include <windows.h>

namespace core::fs::test
{

/// Skips a case that needs elevation, or fails it where `CORE_TEST_REQUIRE_ELEVATED=1` says the
/// runner is elevated (Windows CI), so a lost privilege fails rather than skips there.
/// @param why What could not be done, for the message.
inline void requireElevationOrSkip(char const* why)
{
    if (core::LiveEnvironment {}.get("CORE_TEST_REQUIRE_ELEVATED") == "1")
        FAIL("CORE_TEST_REQUIRE_ELEVATED=1, but " << why);
    SKIP(why);
}

/// @return A drive letter no volume has, from Z down; nullopt where every one is taken.
[[nodiscard]] inline std::optional<char> freeDriveLetter()
{
    auto const used = ::GetLogicalDrives();
    for (auto const index: std::views::iota(0, 26) | std::views::reverse)
        if ((used & (DWORD { 1 } << static_cast<unsigned>(index))) == 0)
            return static_cast<char>('A' + index);
    return std::nullopt;
}

/// Runs a diskpart script.
/// @return Whether diskpart ran it and succeeded; never where it is missing or needs elevation.
[[nodiscard]] inline bool runDiskpart(std::filesystem::path const& script, std::string const& text)
{
    {
        auto file = std::ofstream { script };
        file << text;
        if (!file.good())
            return false;
    }
    // CreateProcessW, not std::system: a child of its own, with nothing but its exit code read.
    auto command = L"diskpart.exe /s \"" + script.wstring() + L"\"";
    auto startup = STARTUPINFOW {};
    startup.cb = sizeof(startup);
    auto process = PROCESS_INFORMATION {};
    if (::CreateProcessW(nullptr,
                         command.data(),
                         nullptr,
                         nullptr,
                         FALSE,
                         CREATE_NO_WINDOW,
                         nullptr,
                         nullptr,
                         &startup,
                         &process)
        == FALSE)
        return false;
    // Bounded: a diskpart that hangs is a volume not made, not a case that never ends.
    auto code = DWORD { 1 };
    if (::WaitForSingleObject(process.hProcess, 120'000) == WAIT_OBJECT_0)
        ::GetExitCodeProcess(process.hProcess, &code);
    else
        ::TerminateProcess(process.hProcess, 1);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return code == 0;
}

/// A small virtual disk formatted with one file system and attached at a drive letter or a folder of
/// its own, detached on destruction.
class VirtualVolume
{
  public:
    /// @param directory Where the disk file and the scripts go.
    /// @param fileSystem What diskpart formats it with: `fat32`, `ntfs`.
    VirtualVolume(std::filesystem::path directory, std::string fileSystem):
        _directory { std::move(directory) }, _fileSystem { std::move(fileSystem) }
    {
    }

    VirtualVolume(VirtualVolume const&) = delete;
    VirtualVolume(VirtualVolume&&) = delete;
    VirtualVolume& operator=(VirtualVolume const&) = delete;
    VirtualVolume& operator=(VirtualVolume&&) = delete;

    ~VirtualVolume()
    {
        if (_isAttached)
            std::ignore = runDiskpart(_directory / "detach.txt",
                                      "select vdisk file=\"" + disk().string() + "\"\ndetach vdisk\n");
    }

    /// Attaches the disk.
    /// @param folder An empty folder of another volume to mount it on; nullopt for a free drive letter.
    /// @return Its root, `X:\` or @p folder; nullopt where no disk could be made and attached here.
    [[nodiscard]] std::optional<std::filesystem::path> attach(
        std::optional<std::filesystem::path> folder = std::nullopt)
    {
        auto const letter = freeDriveLetter();
        if (!folder && !letter)
            return std::nullopt;
        auto const assignment = folder ? "assign mount=\"" + folder->string() + "\"\n"
                                       : "assign letter=" + std::string(1, *letter) + "\n";
        auto const path = disk().string();
        _isAttached =
            runDiskpart(_directory / "attach.txt",
                        "create vdisk file=\"" + path + "\" maximum=64 type=expandable\n"
                            + "select vdisk file=\"" + path + "\"\nattach vdisk\ncreate partition primary\n"
                            + "format fs=" + _fileSystem + " quick label=CORECPP\n" + assignment);
        if (!_isAttached)
            return std::nullopt;
        auto root = folder ? *folder : std::filesystem::path { std::string(1, *letter) + ":\\" };
        auto error = std::error_code {};
        if (!std::filesystem::is_directory(root, error))
            return std::nullopt;
        return root;
    }

  private:
    [[nodiscard]] std::filesystem::path disk() const { return _directory / "volume.vhdx"; }

    std::filesystem::path _directory;
    std::string _fileSystem;
    bool _isAttached = false;
};

} // namespace core::fs::test
