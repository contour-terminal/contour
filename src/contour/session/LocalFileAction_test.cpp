// SPDX-License-Identifier: Apache-2.0
#include <contour/session/LocalFileAction.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <string_view>

using contour::session::ExecutableRule;
using contour::session::LocalFileAction;

TEST_CASE("LocalFileAction: a file is run, edited or handed to the desktop", "[contour][session][hyperlink]")
{
    // Both platforms' rules, on every platform: an execute bit decides on POSIX, the name's suffix on
    // Windows, as Qt's QFileInfo::isExecutable() does there. Only a regular file is run or edited.
    using core::platform::testing::FileEntry;
    auto const files = core::platform::testing::InMemoryFileSystem {
        FileEntry { .path = "/work", .isDirectory = true },
        FileEntry { .path = "/work/tool", .isExecutable = true },
        FileEntry { .path = "/work/notes.txt", .content = "x" },
        FileEntry { .path = "/work/marked.txt", .isExecutable = true },
        FileEntry { .path = "/work/setup.exe", .content = "x" },
        FileEntry { .path = "/work/BUILD.CMD", .content = "x" },
        FileEntry { .path = "/work/folder.exe", .isDirectory = true },
        // Symbolic links: followed to what they name, as QFileInfo follows them; the suffix is the link's
        // own.
        FileEntry { .path = "/work/run", .content = "/work/tool", .isSymlink = true },
        FileEntry { .path = "/work/link.exe", .content = "/work/notes.txt", .isSymlink = true },
        FileEntry { .path = "/work/folder-link", .content = "/work", .isSymlink = true },
    };
    auto constexpr Editor = std::string_view { "vi" };
    auto constexpr NoEditor = std::string_view {};

    auto const [path, rule, editor, expected] =
        GENERATE_COPY(table<std::string_view, ExecutableRule, std::string_view, LocalFileAction>({
            // POSIX: the execute bit.
            { "/work/tool", ExecutableRule::ExecuteBit, NoEditor, LocalFileAction::Run },
            { "/work/marked.txt", ExecutableRule::ExecuteBit, Editor, LocalFileAction::Run },
            { "/work/notes.txt", ExecutableRule::ExecuteBit, Editor, LocalFileAction::Edit },
            { "/work/notes.txt", ExecutableRule::ExecuteBit, NoEditor, LocalFileAction::OpenOnDesktop },
            { "/work/setup.exe", ExecutableRule::ExecuteBit, Editor, LocalFileAction::Edit },
            // Windows: the suffix, in any case.
            { "/work/setup.exe", ExecutableRule::Suffix, NoEditor, LocalFileAction::Run },
            { "/work/BUILD.CMD", ExecutableRule::Suffix, NoEditor, LocalFileAction::Run },
            { "/work/tool", ExecutableRule::Suffix, Editor, LocalFileAction::Edit },
            { "/work/marked.txt", ExecutableRule::Suffix, Editor, LocalFileAction::Edit },
            { "/work/notes.txt", ExecutableRule::Suffix, NoEditor, LocalFileAction::OpenOnDesktop },
            // No regular file: the desktop's, whatever its name or bits.
            { "/work", ExecutableRule::ExecuteBit, Editor, LocalFileAction::OpenOnDesktop },
            { "/work/folder.exe", ExecutableRule::Suffix, Editor, LocalFileAction::OpenOnDesktop },
            { "/work/missing", ExecutableRule::ExecuteBit, Editor, LocalFileAction::OpenOnDesktop },
            { "/work/missing.exe", ExecutableRule::Suffix, Editor, LocalFileAction::OpenOnDesktop },
            // Symbolic links.
            { "/work/run", ExecutableRule::ExecuteBit, Editor, LocalFileAction::Run },
            { "/work/link.exe", ExecutableRule::Suffix, NoEditor, LocalFileAction::Run },
            { "/work/folder-link", ExecutableRule::ExecuteBit, Editor, LocalFileAction::OpenOnDesktop },
        }));
    INFO("path: " << path << ", rule: " << static_cast<int>(rule) << ", editor: \"" << editor << '"');

    CHECK(contour::session::localFileAction(std::filesystem::path { path }, files, rule, editor) == expected);
}

TEST_CASE("LocalFileAction: a failure names what was to be done", "[contour][session][hyperlink]")
{
    // One row per arm, so that "Could not ..." says whether the file was to be run or opened in an editor.
    auto const [action, text] = GENERATE(table<LocalFileAction, std::string_view>({
        { LocalFileAction::Run, "run" },
        { LocalFileAction::Edit, "open in an editor" },
        { LocalFileAction::OpenOnDesktop, "open" },
    }));
    CHECK(contour::session::describeAction(action) == text);
}
