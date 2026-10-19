// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

// What following a hyperlink to a file on this machine does with it: run it, open it in the editor, or hand
// it to the desktop. A decision over the app's filesystem, and so tested over an in-memory one, with either
// platform's rule on every platform.

namespace contour::session
{

/// How a platform tells a program from any other file.
enum class ExecutableRule : uint8_t
{
    /// POSIX: a regular file with any execute bit, as FileSystem::isExecutableFile() asks. QFileInfo, which
    /// decided it before, asked access(2) whether this user may execute it: a file only others may execute,
    /// or one on a noexec mount, is now run -- and the kernel refuses the exec -- where it was edited.
    ExecuteBit = 0,
    /// Windows: a regular file whose name ends in one of WindowsExecutableSuffixes.
    Suffix,
};

/// The rule of the platform this is built for.
inline constexpr auto NativeExecutableRule =
#ifdef _WIN32
    ExecutableRule::Suffix;
#else
    ExecutableRule::ExecuteBit;
#endif

/// The suffixes a file is a program by on Windows, matched without regard to ASCII case: Qt's rule for
/// QFileInfo::isExecutable() there (qtbase src/corelib/io/qfilesystemengine_win.cpp, fillPermissions(), with
/// NTFS permission checks off, Qt's default), which is what decided it before. Not FileSystem's own answer,
/// which on Windows counts any file that is no directory, a text file included. Two nuances of Qt's, both of
/// which would run a file this does not: Qt reads a .lnk shortcut as its target, so a shortcut to a program
/// was run (and CreateProcess cannot start a .lnk), where it is now edited or handed to the desktop, which
/// follows it; and Qt folds case by Unicode's simple mapping, where this folds ASCII only, so a suffix
/// spelled with U+0130 (Latin capital I with dot above), which Qt folds to 'i', is no program here.
inline constexpr auto WindowsExecutableSuffixes =
    std::array<std::string_view, 5> { ".exe", ".com", ".bat", ".pif", ".cmd" };

/// What following a hyperlink to a local file does with it.
enum class LocalFileAction : uint8_t
{
    OpenOnDesktop = 0, ///< Not a regular file (a directory, a missing path), or no editor to open it in.
    Run,               ///< A program: run.
    Edit,              ///< Any other regular file: opened in the editor.
};

/// Decides what following a hyperlink to @p path does.
/// @param path The local file the link names.
/// @param fileSystem The files of this machine.
/// @param rule How a program is told from any other file.
/// @param editor The editor a file is opened in ($EDITOR); empty for none.
/// @return What to do with @p path.
[[nodiscard]] LocalFileAction localFileAction(std::filesystem::path const& path,
                                              core::platform::FileSystem const& fileSystem,
                                              ExecutableRule rule,
                                              std::string_view editor);

/// What @p action does, as a message about its failing names it.
/// @param action What following the link was to do.
/// @return "run" for Run, "open in an editor" for Edit, "open" for OpenOnDesktop.
[[nodiscard]] constexpr std::string_view describeAction(LocalFileAction action) noexcept
{
    switch (action)
    {
        case LocalFileAction::Run: return "run";
        case LocalFileAction::Edit: return "open in an editor";
        case LocalFileAction::OpenOnDesktop: break;
    }
    return "open";
}

} // namespace contour::session
