// SPDX-License-Identifier: Apache-2.0

#include <contour/platform/ConsoleAttach.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <utility>

using namespace contour::platform;

TEST_CASE("only a redirected standard handle is restored after attaching", "[consoleattach]")
{
    static constexpr auto Table = std::array {
        std::pair { StdHandleKind::Redirected, AfterAttach::Restore },
        std::pair { StdHandleKind::Console, AfterAttach::TakeConsole },
        std::pair { StdHandleKind::Absent, AfterAttach::TakeConsole },
    };
    for (auto const& [kind, expected]: Table)
        CHECK(afterAttach(kind) == expected);
}

TEST_CASE("a standard handle is classified by its validity, console mode and file type", "[consoleattach]")
{
    struct Row
    {
        HandleValidity validity {};
        ConsoleMode consoleMode {};
        StdFileType fileType {};
        StdHandleKind expected {};
    };
    static constexpr auto Table = std::array {
        // A console buffer is a character device whose console mode can be read.
        Row { HandleValidity::Valid, ConsoleMode::Available, StdFileType::Char, StdHandleKind::Console },
        // `> out.txt`, `| more`: a file and a pipe.
        Row { HandleValidity::Valid, ConsoleMode::Unavailable, StdFileType::Disk, StdHandleKind::Redirected },
        Row { HandleValidity::Valid, ConsoleMode::Unavailable, StdFileType::Pipe, StdHandleKind::Redirected },
        // `> NUL`, `> COM1`: a character device that is no console.
        Row { HandleValidity::Valid, ConsoleMode::Unavailable, StdFileType::Char, StdHandleKind::Redirected },
        Row { HandleValidity::Valid, ConsoleMode::Unavailable, StdFileType::Unknown, StdHandleKind::Absent },
        // Null or INVALID_HANDLE_VALUE: nothing to keep, whatever the rest says.
        Row {
            HandleValidity::Invalid, ConsoleMode::Unavailable, StdFileType::Unknown, StdHandleKind::Absent },
        Row { HandleValidity::Invalid, ConsoleMode::Unavailable, StdFileType::Disk, StdHandleKind::Absent },
    };
    for (auto const& row: Table)
    {
        CAPTURE(static_cast<int>(row.validity),
                static_cast<int>(row.consoleMode),
                static_cast<int>(row.fileType));
        CHECK(classifyStdHandle(row.validity, row.consoleMode, row.fileType) == row.expected);
    }
}
