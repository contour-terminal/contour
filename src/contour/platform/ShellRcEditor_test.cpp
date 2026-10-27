// SPDX-License-Identifier: Apache-2.0
//
// The rc-file editor writes files users keep by hand, so these tests are the hostile cases: a file
// without a trailing newline, a symlink into a dotfiles repository, CRLF endings, a read-only file, a
// full disk, a file edited between the read and the write. Everything runs over an in-memory file
// system and a fake environment; no test reaches the real HOME.

#include <contour/platform/ShellRcEditor.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>

#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/testing/ForwardingFileSystem.hpp>

using namespace contour::platform;
using contour::config::HostPlatform;
using contour::config::RcState;
using contour::config::ShellKind;
using contour::config::ShellRows;

namespace
{
namespace fs = std::filesystem;

constexpr auto Line = std::string_view { "if true; then :; fi # Contour shell integration" };
/// The fake HOME.
fs::path home()
{
    return "/home/me";
}

/// How the fake disk misbehaves when asked to append.
enum class DiskFault : uint8_t
{
    None = 0,
    ReadOnly,     ///< "Permission denied": nothing is written.
    Full,         ///< "No space left on device": nothing is written.
    FullMidway,   ///< Half of the bytes land, then "No space left on device".
    SilentPrefix, ///< Only the first `landBytes` land, yet the append reports success (an ignored close
                  ///< error).
};

/// An in-memory disk that records every mutating call, can fail an append, resolves symlinks the way a
/// real disk does for weaklyCanonical, and can run a hook after each read (to change a file mid-flight).
struct RcDisk final: vthost::testing::ForwardingFileSystem
{
    struct Call
    {
        std::string what;
        fs::path path;
    };

    mutable std::vector<Call> calls;
    std::map<fs::path, fs::path> links;
    DiskFault fault = DiskFault::None;
    std::size_t landBytes = 0;                  ///< How many bytes DiskFault::SilentPrefix lets through.
    mutable std::function<void(int)> afterRead; ///< Gets the 1-based read count.
    mutable int reads = 0;
    std::function<void()> afterCreateDirectories; ///< Runs once the directories exist.

    [[nodiscard]] int count(std::string_view what, fs::path const& path) const
    {
        return static_cast<int>(
            std::ranges::count_if(calls, [&](Call const& c) { return c.what == what && c.path == path; }));
    }

    [[nodiscard]] bool touched(std::string_view what) const
    {
        return std::ranges::any_of(calls, [&](Call const& c) { return c.what == what; });
    }

    [[nodiscard]] fs::path weaklyCanonical(fs::path const& path) const override
    {
        if (auto const link = links.find(path); link != links.end())
            return link->second;
        return files.weaklyCanonical(path);
    }

    [[nodiscard]] std::expected<std::string, std::string> readFile(fs::path const& path) const override
    {
        auto result = files.readFile(path);
        if (afterRead)
            afterRead(++reads);
        return result;
    }

    [[nodiscard]] std::expected<void, std::string> appendFile(fs::path const& path,
                                                              std::string_view content) const override
    {
        calls.push_back({ "appendFile", path });
        switch (fault)
        {
            case DiskFault::None: return files.appendFile(path, content);
            case DiskFault::ReadOnly: return std::unexpected("Permission denied");
            case DiskFault::Full: return std::unexpected("No space left on device");
            case DiskFault::FullMidway:
                std::ignore = files.appendFile(path, content.substr(0, content.size() / 2));
                return std::unexpected("No space left on device");
            case DiskFault::SilentPrefix: return files.appendFile(path, content.substr(0, landBytes));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> writeFile(fs::path const& path,
                                                             std::string_view content) const override
    {
        calls.push_back({ "writeFile", path });
        return files.writeFile(path, content);
    }

    [[nodiscard]] std::expected<void, std::string> rename(fs::path const& from,
                                                          fs::path const& to) const override
    {
        calls.push_back({ "rename", to });
        return files.rename(from, to);
    }

    [[nodiscard]] std::expected<void, std::string> replaceFile(fs::path const& from,
                                                               fs::path const& to) const override
    {
        calls.push_back({ "replaceFile", to });
        return files.replaceFile(from, to);
    }

    [[nodiscard]] std::expected<std::unique_ptr<std::ostream>, std::string> openWrite(
        fs::path const& path, core::platform::WriteMode mode) const override
    {
        calls.push_back({ "openWrite", path });
        return files.openWrite(path, mode);
    }

    [[nodiscard]] std::expected<void, std::string> createDirectories(fs::path const& path) const override
    {
        calls.push_back({ "createDirectories", path });
        auto result = files.createDirectories(path);
        if (afterCreateDirectories)
            afterCreateDirectories();
        return result;
    }

    [[nodiscard]] std::expected<bool, std::string> remove(fs::path const& path) const override
    {
        calls.push_back({ "remove", path });
        return files.remove(path);
    }

    [[nodiscard]] std::string text(fs::path const& path) const
    {
        return files.readFile(path).value_or("<none>");
    }
};

/// A test harness: the disk, the environment and the editor under test.
struct Rig
{
    RcDisk disk;
    core::testing::FakeEnvironment env;
    FileShellRcEditor editor;

    explicit Rig(std::map<std::string, std::string, std::less<>> variables = { { "HOME", "/home/me" } },
                 HostPlatform host = HostPlatform::Other):
        env { std::move(variables) }, editor { disk, env, host }
    {
    }
};

/// The zsh rc file under the fake HOME.
fs::path zshRc()
{
    return home() / ".zshrc";
}
} // namespace

TEST_CASE("ShellRcEditor: an absent file is Absent, and install creates it with exactly the line",
          "[shellrc]")
{
    auto rig = Rig {};
    auto const inspection = rig.editor.inspect(ShellKind::Zsh);
    REQUIRE(inspection.has_value());
    CHECK(*inspection == RcInspection { zshRc(), RcState::Absent });
    CHECK(!rig.disk.files.exists(zshRc()));

    auto const installed = rig.editor.install(ShellKind::Zsh, Line);
    REQUIRE(installed.has_value());
    CHECK(*installed == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == std::string(Line) + "\n");
    CHECK(rig.editor.inspect(ShellKind::Zsh)->state == RcState::Installed);
}

TEST_CASE("ShellRcEditor: a file without a trailing newline gets one before the line", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == "export A=1\n" + std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: a file ending in a newline gets no blank line", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == "export A=1\n" + std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: an existing empty file gets the line alone", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: CRLF files keep their endings and the line starts on its own line", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\r\nexport B=2\r\n");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == "export A=1\r\nexport B=2\r\n" + std::string(Line) + "\n");

    auto unterminated = Rig {};
    unterminated.disk.files.addFile(zshRc(), "export A=1\r");
    REQUIRE(unterminated.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);
    CHECK(unterminated.disk.text(zshRc()) == "export A=1\r\n" + std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: a second install writes nothing", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);
    auto const afterFirst = rig.disk.text(zshRc());
    rig.disk.calls.clear();

    CHECK(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::AlreadyInstalled);
    CHECK(rig.disk.text(zshRc()) == afterFirst);
    CHECK(rig.disk.calls.empty());
}

TEST_CASE("ShellRcEditor: an existing marker block counts as installed, wherever it is", "[shellrc]")
{
    auto rig = Rig {};
    auto const content = "export A=1\n# >>> mine\n" + std::string(Line) + "\n# <<<\nexport B=2";
    rig.disk.files.addFile(zshRc(), content);
    CHECK(rig.editor.inspect(ShellKind::Zsh)->state == RcState::Installed);
    CHECK(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::AlreadyInstalled);
    CHECK(rig.disk.text(zshRc()) == content);
}

TEST_CASE("ShellRcEditor: what counts as an installed rc file", "[shellrc]")
{
    struct Case
    {
        ShellKind shell;
        std::string_view content;
        RcState expected;
    };
    constexpr auto Cases = std::array {
        Case { ShellKind::Zsh, "source ~/.config/contour/shell-integration.zsh\n", RcState::Installed },
        Case { ShellKind::Zsh, "  . /opt/contour/shell-integration.zsh", RcState::Installed },
        Case {
            ShellKind::Zsh, "eval \"$(contour generate integration shell zsh to -)\"", RcState::Installed },
        Case { ShellKind::Zsh, "export A=1 # Contour shell integration", RcState::Installed },
        Case { ShellKind::Zsh, "# source ~/shell-integration.zsh\n", RcState::Absent },
        Case { ShellKind::Zsh, "  # contour generate integration shell zsh to -\n", RcState::Absent },
        Case { ShellKind::Zsh, "source ~/shell-integration.bash\n", RcState::Absent },
        Case { ShellKind::Zsh, "echo shell-integration.zsh\n", RcState::Absent },
        Case { ShellKind::Zsh, "eval \"$(contour generate integration shell bash to -)\"", RcState::Absent },
        Case { ShellKind::Bash, "source ~/shell-integration.bash\r\n", RcState::Installed },
        Case {
            ShellKind::Fish, "contour generate integration shell fish to - | source\n", RcState::Installed },
        Case { ShellKind::Fish, "", RcState::Absent },
    };
    for (auto const& c: Cases)
    {
        INFO(c.content);
        CHECK(rcStateOf(c.shell, c.content) == c.expected);
    }
}

TEST_CASE("ShellRcEditor: a manual install counts, and install writes nothing", "[shellrc]")
{
    auto zsh = Rig {};
    zsh.disk.files.addFile(zshRc(), "source ~/.config/contour/shell-integration.zsh");
    CHECK(zsh.editor.inspect(ShellKind::Zsh)->state == RcState::Installed);
    CHECK(zsh.editor.install(ShellKind::Zsh, Line) == RcInstall::AlreadyInstalled);
    CHECK(zsh.disk.text(zshRc()) == "source ~/.config/contour/shell-integration.zsh");
    CHECK(!zsh.disk.touched("appendFile"));

    auto fish = Rig {};
    auto const fishRc = home() / ".config/fish/config.fish";
    fish.disk.files.addFile(home() / ".config/fish/conf.d/contour.fish", "# manual");
    CHECK(fish.editor.inspect(ShellKind::Fish)->state == RcState::Installed);
    CHECK(fish.editor.install(ShellKind::Fish, Line) == RcInstall::AlreadyInstalled);
    CHECK(!fish.disk.files.exists(fishRc));
}

TEST_CASE("ShellRcEditor: where each shell's file is", "[shellrc]")
{
    struct Case
    {
        ShellKind shell;
        std::map<std::string, std::string, std::less<>> variables;
        HostPlatform host;
        fs::path expected;
    };
    auto const cases = std::vector<Case> {
        { ShellKind::Zsh, { { "HOME", "/home/me" } }, HostPlatform::Other, "/home/me/.zshrc" },
        { ShellKind::Zsh,
          { { "HOME", "/home/me" }, { "ZDOTDIR", "/home/me/.config/zsh" } },
          HostPlatform::Other,
          "/home/me/.config/zsh/.zshrc" },
        { ShellKind::Zsh,
          { { "HOME", "/home/me" }, { "ZDOTDIR", "" } },
          HostPlatform::Other,
          "/home/me/.zshrc" },
        { ShellKind::Zsh,
          { { "HOME", "/home/me" }, { "ZDOTDIR", "relative" } },
          HostPlatform::Other,
          "/home/me/.zshrc" },
        { ShellKind::Bash, { { "HOME", "/home/me" } }, HostPlatform::Other, "/home/me/.bashrc" },
        { ShellKind::Bash, { { "HOME", "/home/me" } }, HostPlatform::KdePlasma, "/home/me/.bashrc" },
        { ShellKind::Bash, { { "HOME", "/home/me" } }, HostPlatform::MacOS, "/home/me/.bash_profile" },
        { ShellKind::Fish,
          { { "HOME", "/home/me" } },
          HostPlatform::Other,
          "/home/me/.config/fish/config.fish" },
        { ShellKind::Fish,
          { { "HOME", "/home/me" }, { "XDG_CONFIG_HOME", "/xdg" } },
          HostPlatform::Other,
          "/xdg/fish/config.fish" },
        { ShellKind::Fish,
          { { "HOME", "/home/me" }, { "XDG_CONFIG_HOME", "xdg" } },
          HostPlatform::Other,
          "/home/me/.config/fish/config.fish" },
    };
    for (auto const& c: cases)
    {
        INFO(c.expected.string());
        auto rig = Rig { c.variables, c.host };
        REQUIRE(rig.editor.inspect(c.shell).has_value());
        CHECK(rig.editor.inspect(c.shell)->file == c.expected);
        REQUIRE(rig.editor.install(c.shell, Line) == RcInstall::Appended);
        CHECK(rig.disk.text(c.expected) == std::string(Line) + "\n");
    }
}

TEST_CASE("ShellRcEditor: fish creates its config directory first", "[shellrc]")
{
    auto rig = Rig {};
    REQUIRE(!rig.disk.files.isDirectory(home() / ".config/fish"));
    REQUIRE(rig.editor.install(ShellKind::Fish, Line) == RcInstall::Appended);
    CHECK(rig.disk.files.isDirectory(home() / ".config/fish"));
    CHECK(rig.disk.text(home() / ".config/fish/config.fish") == std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: no HOME, no file", "[shellrc]")
{
    for (auto const& variables: std::vector<std::map<std::string, std::string, std::less<>>> {
             {}, { { "HOME", "" } }, { { "HOME", "relative/home" } } })
    {
        auto rig = Rig { variables };
        for (auto const& row: ShellRows)
        {
            CHECK(rig.editor.inspect(row.kind).error() == ShellRcError::NoHome);
            CHECK(rig.editor.install(row.kind, Line).error() == ShellRcError::NoHome);
        }
        CHECK(rig.disk.calls.empty());
    }
}

TEST_CASE("ShellRcEditor: a directory or a dangling link at the rc path is NotAFile", "[shellrc]")
{
    auto directory = Rig {};
    directory.disk.files.addDirectory(zshRc());
    CHECK(directory.editor.inspect(ShellKind::Zsh).error() == ShellRcError::NotAFile);
    CHECK(directory.editor.install(ShellKind::Zsh, Line).error() == ShellRcError::NotAFile);
    CHECK(!directory.disk.touched("appendFile"));

    auto dangling = Rig {};
    dangling.disk.files.addSymlink(zshRc(), home() / "dotfiles/zshrc");
    dangling.disk.links[zshRc()] = home() / "dotfiles/zshrc";
    CHECK(dangling.editor.install(ShellKind::Zsh, Line).error() == ShellRcError::NotAFile);
    CHECK(!dangling.disk.touched("appendFile"));
    CHECK(!dangling.disk.files.exists(home() / "dotfiles/zshrc"));
}

TEST_CASE("ShellRcEditor: an unreadable file is Unreadable and is left alone", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    rig.disk.files.denyAccess(zshRc());
    CHECK(rig.editor.inspect(ShellKind::Zsh).error() == ShellRcError::Unreadable);
    CHECK(rig.editor.install(ShellKind::Zsh, Line).error() == ShellRcError::Unreadable);
    CHECK(!rig.disk.touched("appendFile"));
}

TEST_CASE("ShellRcEditor: a symlinked rc is appended through, and the link and its target survive",
          "[shellrc]")
{
    auto rig = Rig {};
    auto const target = home() / "dotfiles/zshrc";
    rig.disk.files.addFile(
        target, "export A=1\n", fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    rig.disk.files.addSymlink(zshRc(), target);
    rig.disk.links[zshRc()] = target;

    REQUIRE(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);

    CHECK(rig.disk.files.isSymlink(zshRc()));
    CHECK(rig.disk.text(target) == "export A=1\n" + std::string(Line) + "\n");
    CHECK(rig.disk.files.permissions(target).value()
          == (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
    CHECK(rig.disk.count("appendFile", target) == 1);
    CHECK(rig.disk.count("appendFile", zshRc()) == 0);
    CHECK(rig.editor.inspect(ShellKind::Zsh)->state == RcState::Installed);
}

TEST_CASE("ShellRcEditor: only ever appends to the rc path", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);
    CHECK(rig.disk.count("appendFile", zshRc()) == 1);
    for (auto const* const forbidden: { "writeFile", "rename", "replaceFile", "openWrite", "remove" })
    {
        INFO(forbidden);
        CHECK(!rig.disk.touched(forbidden));
    }
}

TEST_CASE("ShellRcEditor: preserves the rc file's permissions", "[shellrc]")
{
    auto rig = Rig {};
    auto const perms = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
    rig.disk.files.addFile(zshRc(), "export A=1\n", perms);
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line) == RcInstall::Appended);
    CHECK(rig.disk.files.permissions(zshRc()).value() == perms);
}

TEST_CASE("ShellRcEditor: a read-only file stays exactly as it was", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n", fs::perms::owner_read);
    rig.disk.fault = DiskFault::ReadOnly;
    CHECK(rig.editor.install(ShellKind::Zsh, Line).error() == ShellRcError::Unwritable);
    CHECK(rig.disk.text(zshRc()) == "export A=1\n");
}

TEST_CASE("ShellRcEditor: a full disk never truncates the file", "[shellrc]")
{
    auto nothingWritten = Rig {};
    nothingWritten.disk.files.addFile(zshRc(), "export A=1\nexport B=2");
    nothingWritten.disk.fault = DiskFault::Full;
    CHECK(nothingWritten.editor.install(ShellKind::Zsh, Line).error() == ShellRcError::Unwritable);
    CHECK(nothingWritten.disk.text(zshRc()) == "export A=1\nexport B=2");
    CHECK(!nothingWritten.disk.touched("writeFile"));

    auto midway = Rig {};
    midway.disk.files.addFile(zshRc(), "export A=1\nexport B=2");
    midway.disk.fault = DiskFault::FullMidway;
    CHECK(midway.editor.install(ShellKind::Zsh, Line).error() == ShellRcError::PartialWrite);
    // The user's own bytes are all still there, in place; only more bytes followed them.
    CHECK(midway.disk.text(zshRc()).starts_with("export A=1\nexport B=2"));
    CHECK(!midway.disk.touched("writeFile"));
}

TEST_CASE("ShellRcEditor: an append that reports success but lands only a prefix is a PartialWrite",
          "[shellrc]")
{
    auto const line = contour::window::shellRcLine(ShellKind::Zsh, "/usr/bin/contour").value();
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    rig.disk.fault = DiskFault::SilentPrefix;
    rig.disk.landBytes = line.size() / 2;
    CHECK(rig.editor.install(ShellKind::Zsh, line).error() == ShellRcError::PartialWrite);
    CHECK(rig.disk.text(zshRc()).starts_with("export A=1\n"));
}

TEST_CASE("ShellRcEditor: a cut-short line counts as a partial install, and nothing more is appended",
          "[shellrc]")
{
    for (auto const shell: { ShellKind::Bash, ShellKind::Zsh, ShellKind::Fish })
    {
        auto const line = contour::window::shellRcLine(shell, "/usr/bin/contour").value();
        auto const rc = [&](Rig const& rig) {
            return rig.editor.inspect(shell)->file;
        };
        // Every cut from one byte up to the line minus its marker's last byte and the line ending: the
        // last two cuts leave a whole, merely unterminated or terminated line.
        for (auto const landed: std::views::iota(std::size_t { 1 }, line.size()))
        {
            INFO("shell " << static_cast<int>(shell) << " landed " << landed);
            auto rig = Rig {};
            rig.disk.files.addFile(rc(rig), "export A=1\n");
            rig.disk.fault = DiskFault::SilentPrefix;
            rig.disk.landBytes = landed;
            REQUIRE(rig.editor.install(shell, line).error() == ShellRcError::PartialWrite);

            auto const inspected = rig.editor.inspect(shell);
            REQUIRE(inspected.has_value());
            CHECK(inspected->state == RcState::Partial);

            auto const before = rig.disk.text(rc(rig));
            rig.disk.fault = DiskFault::None;
            CHECK(rig.editor.install(shell, line).error() == ShellRcError::PartialWrite);
            CHECK(rig.disk.text(rc(rig)) == before);
        }
    }
}

TEST_CASE("rcStateOf: an unterminated last line that is a prefix of Contour's line is Partial", "[shellrc]")
{
    CHECK(rcStateOf(ShellKind::Zsh, "export A=1\nif [ \"$TERM_PROGRAM\" = contour ]; then eval \"$('/x/c")
          == RcState::Partial);
    CHECK(rcStateOf(ShellKind::Zsh, "export A=1\nif [ \"$TERM") == RcState::Partial);
    CHECK(rcStateOf(ShellKind::Fish,
                    "if test \"$TERM_PROGRAM\" = contour; '/x/c' generate integration shell fi")
          == RcState::Partial);
    // The user's own unterminated last line is not ours.
    CHECK(rcStateOf(ShellKind::Zsh, "export A=1\nexport B=2") == RcState::Absent);
    CHECK(rcStateOf(ShellKind::Zsh, "export A=1\n") == RcState::Absent);
    // A whole line, with or without its line ending, is Installed.
    auto const line = contour::window::shellRcLine(ShellKind::Zsh, "/usr/bin/contour").value();
    CHECK(rcStateOf(ShellKind::Zsh, line) == RcState::Installed);
    CHECK(rcStateOf(ShellKind::Zsh, line + "\n") == RcState::Installed);
}

TEST_CASE("ShellRcEditor: a file edited between the read and the write is refused", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    // Read 1 decides, read 2 re-checks: the user saves the file in between.
    rig.disk.afterRead = [&](int read) {
        if (read == 1)
            rig.disk.files.addFile(zshRc(), "export A=1\nexport EDITED=1\n");
    };
    CHECK(rig.editor.install(ShellKind::Zsh, Line).error() == ShellRcError::Changed);
    CHECK(rig.disk.text(zshRc()) == "export A=1\nexport EDITED=1\n");
    CHECK(!rig.disk.touched("appendFile"));
}

TEST_CASE("ShellRcEditor: a file created between the read and the write is refused", "[shellrc]")
{
    auto rig = Rig {};
    auto const fishRc = home() / ".config/fish/config.fish";
    // The first read finds no file; the config directory is then made, and the user saves one meanwhile.
    rig.disk.afterCreateDirectories = [&] {
        rig.disk.files.addFile(fishRc, "set -x RACE 1\n");
    };
    CHECK(rig.editor.install(ShellKind::Fish, Line).error() == ShellRcError::Changed);
    CHECK(rig.disk.text(fishRc) == "set -x RACE 1\n");
    CHECK(!rig.disk.touched("appendFile"));
}

TEST_CASE("ShellRcEditor: a line that is not one line is refused", "[shellrc]")
{
    auto rig = Rig {};
    for (auto const line: { std::string_view {}, std::string_view { "a\nb" }, std::string_view { "a\r" } })
        CHECK(rig.editor.install(ShellKind::Zsh, line).error() == ShellRcError::InvalidLine);
    CHECK(!rig.disk.touched("appendFile"));
}

TEST_CASE("ShellRcEditor: an unknown shell has no file", "[shellrc]")
{
    auto rig = Rig {};
    CHECK(*rig.editor.inspect(ShellKind::Unknown) == RcInspection {});
    CHECK(rig.editor.install(ShellKind::Unknown, Line).error() == ShellRcError::Unwritable);
    CHECK(rig.disk.calls.empty());
}

TEST_CASE("NullShellRcEditor: inspects nothing and writes nothing", "[shellrc]")
{
    auto editor = NullShellRcEditor {};
    for (auto const& row: ShellRows)
    {
        CHECK(*editor.inspect(row.kind) == RcInspection {});
        CHECK(editor.inspect(row.kind)->state == RcState::Unknown);
        CHECK(editor.install(row.kind, Line).error() == ShellRcError::Unwritable);
    }
}

TEST_CASE("ShellRcEditor: every error has words", "[shellrc]")
{
    constexpr auto Errors =
        std::array { ShellRcError::NoHome,      ShellRcError::Unreadable, ShellRcError::Unwritable,
                     ShellRcError::NotAFile,    ShellRcError::Changed,    ShellRcError::InvalidLine,
                     ShellRcError::PartialWrite };
    static_assert(Errors.size() == std::to_underlying(ShellRcError::PartialWrite) + 1U);
    CHECK(describe(ShellRcError::NoHome) == "the home directory is not known");
    CHECK(describe(ShellRcError::Unreadable) == "the file could not be read");
    CHECK(describe(ShellRcError::Unwritable) == "the file could not be written");
    CHECK(describe(ShellRcError::NotAFile) == "it is not a regular file");
    for (auto const error: Errors)
        CHECK(!describe(error).empty());
}
