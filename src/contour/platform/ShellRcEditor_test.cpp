// SPDX-License-Identifier: Apache-2.0
//
// The rc-file editor writes files users keep by hand, so these tests are the hostile cases: a file
// without a trailing newline, a symlink into a dotfiles repository, CRLF endings, a read-only file, a
// full disk, a file edited between the read and the write. Everything runs over an in-memory file
// system and a fake environment; no test reaches the real HOME.

#include <contour/config/ExecutionPolicy.hpp>
#include <contour/platform/ExecutionPolicySource.hpp>
#include <contour/platform/KnownFolders.hpp>
#include <contour/platform/ShellRcEditor.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>

#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
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
/// A session that sets nothing relevant to where a startup file lives.
RcEnvironment const noSession {};

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
    NullKnownFolders knownFolders;
    NoExecutionPolicySource executionPolicy;
    FileShellRcEditor editor;

    explicit Rig(std::map<std::string, std::string, std::less<>> variables = { { "HOME", "/home/me" } },
                 HostPlatform host = HostPlatform::Other):
        env { std::move(variables) },
        editor { disk, env, knownFolders, executionPolicy, host, RunningAs::User }
    {
    }
};

/// A Documents folder the test chooses, or none.
struct FakeKnownFolders final: IKnownFolders
{
    std::optional<fs::path> folder;

    explicit FakeKnownFolders(std::optional<fs::path> documents): folder { std::move(documents) } {}

    [[nodiscard]] std::optional<fs::path> documents() const override { return folder; }
};

/// Whether FakeExecutionPolicySource lets a policy write succeed.
enum class PolicyWrites : uint8_t
{
    Succeed = 0,
    Fail, ///< "Access denied": the policy stays as it was.
};

/// An execution policy the test sets, which records every attempt to allow the user's scripts.
struct FakeExecutionPolicySource final: IExecutionPolicySource
{
    contour::config::EffectivePolicy policy { .policy = contour::config::ExecutionPolicy::RemoteSigned,
                                              .scope = contour::config::PolicyScope::Default };
    PolicyWrites writes = PolicyWrites::Succeed;
    std::vector<ShellKind> attempts;   ///< Every allowUserScripts() call, successful or not.
    std::function<void()> duringWrite; ///< Runs inside allowUserScripts(), to see the disk at that moment.

    [[nodiscard]] contour::config::EffectivePolicy effectivePolicy(ShellKind /*shell*/) const override
    {
        return policy;
    }

    [[nodiscard]] std::expected<void, std::string> allowUserScripts(ShellKind shell) override
    {
        attempts.push_back(shell);
        if (duringWrite)
            duringWrite();
        if (writes == PolicyWrites::Fail)
            return std::unexpected(std::string { "Access denied" });
        policy = { .policy = contour::config::ExecutionPolicy::RemoteSigned,
                   .scope = contour::config::PolicyScope::CurrentUser };
        return {};
    }
};

/// A harness for PowerShell's profiles: a chosen Documents folder and execution policy.
struct ProfileRig
{
    RcDisk disk;
    core::testing::FakeEnvironment env;
    FakeKnownFolders knownFolders;
    FakeExecutionPolicySource executionPolicy;
    FileShellRcEditor editor;

    explicit ProfileRig(std::optional<fs::path> documents,
                        HostPlatform host = HostPlatform::Windows,
                        std::map<std::string, std::string, std::less<>> variables = { { "HOME",
                                                                                        "/home/me" } }):
        env { std::move(variables) },
        knownFolders { std::move(documents) },
        editor { disk, env, knownFolders, executionPolicy, host, RunningAs::User }
    {
    }
};

/// The Documents folder of the PowerShell tests: rooted on every host.
fs::path documents()
{
    return "/home/me/OneDrive/Documents";
}

/// PowerShell 7's profile under documents().
fs::path pwshProfile()
{
    return documents() / "PowerShell/Microsoft.PowerShell_profile.ps1";
}

/// Windows PowerShell's profile under documents().
fs::path windowsPowerShellProfile()
{
    return documents() / "WindowsPowerShell/Microsoft.PowerShell_profile.ps1";
}

/// @return @p ascii as UTF-16LE, without a byte order mark; ASCII only, so the test needs no encoder.
std::string utf16Le(std::string_view ascii)
{
    auto out = std::string {};
    for (auto const c: ascii)
    {
        out.push_back(c);
        out.push_back('\0');
    }
    return out;
}

constexpr auto Utf8Bom = std::string_view { "\xEF\xBB\xBF" };
constexpr auto Utf16LeBom = std::string_view { "\xFF\xFE" };

/// Contour's line for @p shell, running a binary under a non-ASCII home directory.
std::string nonAsciiLine(ShellKind shell)
{
    return contour::window::shellRcLine(shell, fs::path { std::u8string_view { u8"/home/Müller/contour" } })
        .value();
}

/// Contour's line for @p shell, running an ASCII path.
std::string asciiLine(ShellKind shell)
{
    return contour::window::shellRcLine(shell, "/usr/bin/contour").value();
}

/// The zsh rc file under the fake HOME.
fs::path zshRc()
{
    return home() / ".zshrc";
}
} // namespace

TEST_CASE("ShellRcEditor: an absent file is Absent, and install creates it with a line ending, then the line",
          "[shellrc]")
{
    auto rig = Rig {};
    auto const inspection = rig.editor.inspect(ShellKind::Zsh, noSession);
    REQUIRE(inspection.has_value());
    CHECK(*inspection == RcInspection { zshRc(), RcState::Absent });
    CHECK(!rig.disk.files.exists(zshRc()));

    auto const installed = rig.editor.install(ShellKind::Zsh, Line, noSession);
    REQUIRE(installed.has_value());
    CHECK(*installed == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == "\n" + std::string(Line) + "\n");
    CHECK(rig.editor.inspect(ShellKind::Zsh, noSession)->state == RcState::Installed);
}

TEST_CASE("ShellRcEditor: a file without a trailing newline still has the line on its own line", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == "export A=1\n" + std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: a file ending in a newline gets one blank line before the line", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == "export A=1\n\n" + std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: an existing empty file gets a blank line, then the line", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == "\n" + std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: CRLF files keep their endings and the line starts on its own line", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\r\nexport B=2\r\n");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);
    CHECK(rig.disk.text(zshRc()) == "export A=1\r\nexport B=2\r\n\n" + std::string(Line) + "\n");

    auto unterminated = Rig {};
    unterminated.disk.files.addFile(zshRc(), "export A=1\r");
    REQUIRE(unterminated.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);
    CHECK(unterminated.disk.text(zshRc()) == "export A=1\r\n" + std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: a second install writes nothing", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);
    auto const afterFirst = rig.disk.text(zshRc());
    rig.disk.calls.clear();

    CHECK(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::AlreadyInstalled);
    CHECK(rig.disk.text(zshRc()) == afterFirst);
    CHECK(rig.disk.calls.empty());
}

TEST_CASE("ShellRcEditor: an existing marker block counts as installed, wherever it is", "[shellrc]")
{
    auto rig = Rig {};
    auto const content = "export A=1\n# >>> mine\n" + std::string(Line) + "\n# <<<\nexport B=2";
    rig.disk.files.addFile(zshRc(), content);
    CHECK(rig.editor.inspect(ShellKind::Zsh, noSession)->state == RcState::Installed);
    CHECK(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::AlreadyInstalled);
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
    CHECK(zsh.editor.inspect(ShellKind::Zsh, noSession)->state == RcState::Installed);
    CHECK(zsh.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::AlreadyInstalled);
    CHECK(zsh.disk.text(zshRc()) == "source ~/.config/contour/shell-integration.zsh");
    CHECK(!zsh.disk.touched("appendFile"));

    auto fish = Rig {};
    auto const fishRc = home() / ".config/fish/config.fish";
    fish.disk.files.addFile(home() / ".config/fish/conf.d/contour.fish", "# manual");
    CHECK(fish.editor.inspect(ShellKind::Fish, noSession)->state == RcState::Installed);
    CHECK(fish.editor.install(ShellKind::Fish, Line, noSession) == RcInstall::AlreadyInstalled);
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
        REQUIRE(rig.editor.inspect(c.shell, noSession).has_value());
        CHECK(rig.editor.inspect(c.shell, noSession)->file == c.expected);
        REQUIRE(rig.editor.install(c.shell, Line, noSession) == RcInstall::Appended);
        CHECK(rig.disk.text(c.expected) == "\n" + std::string(Line) + "\n");
    }
}

TEST_CASE("ShellRcEditor: the session's ZDOTDIR and XDG_CONFIG_HOME win over Contour's own", "[shellrc]")
{
    struct Case
    {
        ShellKind shell;
        RcEnvironment session;
        fs::path expected;
    };
    auto const cases = std::array {
        Case { ShellKind::Zsh, { { "ZDOTDIR", "/zdot" } }, "/zdot/.zshrc" },
        Case { ShellKind::Zsh, { { "ZDOTDIR", "" } }, "/home/me/.zshrc" }, // removed from the shell's env
        Case { ShellKind::Zsh, { { "ZDOTDIR", "relative" } }, "/home/me/.zshrc" },
        Case { ShellKind::Zsh, { { "XDG_CONFIG_HOME", "/xdg" } }, "/contour-zdot/.zshrc" },
        Case { ShellKind::Fish, { { "XDG_CONFIG_HOME", "/xdg" } }, "/xdg/fish/config.fish" },
        Case { ShellKind::Fish, { { "XDG_CONFIG_HOME", "" } }, "/home/me/.config/fish/config.fish" },
    };
    for (auto const& c: cases)
    {
        INFO(c.expected.string());
        // Contour's own environment sets both, to places the shell would not look.
        auto rig = Rig {
            { { "HOME", "/home/me" }, { "ZDOTDIR", "/contour-zdot" }, { "XDG_CONFIG_HOME", "/contour-xdg" } }
        };
        REQUIRE(rig.editor.inspect(c.shell, c.session).has_value());
        CHECK(rig.editor.inspect(c.shell, c.session)->file == c.expected);
        REQUIRE(rig.editor.install(c.shell, Line, c.session) == RcInstall::Appended);
        CHECK(rig.disk.text(c.expected) == "\n" + std::string(Line) + "\n");
    }
}

TEST_CASE("ShellRcEditor: a superuser's Contour inspects and writes nothing", "[shellrc]")
{
    auto disk = RcDisk {};
    auto const env = core::testing::FakeEnvironment { { { "HOME", "/home/me" } } };
    auto const knownFolders = NullKnownFolders {};
    auto executionPolicy = NoExecutionPolicySource {};
    auto const editor = std::make_unique<FileShellRcEditor>(
        disk, env, knownFolders, executionPolicy, HostPlatform::Other, RunningAs::Superuser);
    for (auto const& row: ShellRows)
    {
        CHECK(editor->inspect(row.kind, noSession).error() == ShellRcError::Superuser);
        CHECK(editor->install(row.kind, Line, noSession).error() == ShellRcError::Superuser);
    }
    CHECK(disk.calls.empty());
    CHECK(!describe(ShellRcError::Superuser).empty());
}

TEST_CASE("ShellRcEditor: fish creates its config directory first", "[shellrc]")
{
    auto rig = Rig {};
    REQUIRE(!rig.disk.files.isDirectory(home() / ".config/fish"));
    REQUIRE(rig.editor.install(ShellKind::Fish, Line, noSession) == RcInstall::Appended);
    CHECK(rig.disk.files.isDirectory(home() / ".config/fish"));
    CHECK(rig.disk.text(home() / ".config/fish/config.fish") == "\n" + std::string(Line) + "\n");
}

TEST_CASE("ShellRcEditor: no HOME, no file", "[shellrc]")
{
    for (auto const& variables: std::vector<std::map<std::string, std::string, std::less<>>> {
             {}, { { "HOME", "" } }, { { "HOME", "relative/home" } } })
    {
        auto rig = Rig { variables };
        for (auto const& row: ShellRows | std::views::filter([](contour::config::ShellRow const& row) {
                                  return contour::config::isInstallableOn(row, HostPlatform::Other);
                              }))
        {
            CHECK(rig.editor.inspect(row.kind, noSession).error() == ShellRcError::NoHome);
            CHECK(rig.editor.install(row.kind, Line, noSession).error() == ShellRcError::NoHome);
        }
        CHECK(rig.disk.calls.empty());
    }
}

TEST_CASE("ShellRcEditor: a directory or a dangling link at the rc path is NotAFile", "[shellrc]")
{
    auto directory = Rig {};
    directory.disk.files.addDirectory(zshRc());
    CHECK(directory.editor.inspect(ShellKind::Zsh, noSession).error() == ShellRcError::NotAFile);
    CHECK(directory.editor.install(ShellKind::Zsh, Line, noSession).error() == ShellRcError::NotAFile);
    CHECK(!directory.disk.touched("appendFile"));

    auto dangling = Rig {};
    dangling.disk.files.addSymlink(zshRc(), home() / "dotfiles/zshrc");
    dangling.disk.links[zshRc()] = home() / "dotfiles/zshrc";
    CHECK(dangling.editor.install(ShellKind::Zsh, Line, noSession).error() == ShellRcError::NotAFile);
    CHECK(!dangling.disk.touched("appendFile"));
    CHECK(!dangling.disk.files.exists(home() / "dotfiles/zshrc"));
}

TEST_CASE("ShellRcEditor: an unreadable file is Unreadable and is left alone", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    rig.disk.files.denyAccess(zshRc());
    CHECK(rig.editor.inspect(ShellKind::Zsh, noSession).error() == ShellRcError::Unreadable);
    CHECK(rig.editor.install(ShellKind::Zsh, Line, noSession).error() == ShellRcError::Unreadable);
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

    REQUIRE(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);

    CHECK(rig.disk.files.isSymlink(zshRc()));
    CHECK(rig.disk.text(target) == "export A=1\n\n" + std::string(Line) + "\n");
    CHECK(rig.disk.files.permissions(target).value()
          == (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
    CHECK(rig.disk.count("appendFile", target) == 1);
    CHECK(rig.disk.count("appendFile", zshRc()) == 0);
    CHECK(rig.editor.inspect(ShellKind::Zsh, noSession)->state == RcState::Installed);
}

TEST_CASE("ShellRcEditor: only ever appends to the rc path", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1");
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);
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
    REQUIRE(rig.editor.install(ShellKind::Zsh, Line, noSession) == RcInstall::Appended);
    CHECK(rig.disk.files.permissions(zshRc()).value() == perms);
}

TEST_CASE("ShellRcEditor: a read-only file stays exactly as it was", "[shellrc]")
{
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n", fs::perms::owner_read);
    rig.disk.fault = DiskFault::ReadOnly;
    CHECK(rig.editor.install(ShellKind::Zsh, Line, noSession).error() == ShellRcError::Unwritable);
    CHECK(rig.disk.text(zshRc()) == "export A=1\n");
}

TEST_CASE("ShellRcEditor: a full disk never truncates the file", "[shellrc]")
{
    auto nothingWritten = Rig {};
    nothingWritten.disk.files.addFile(zshRc(), "export A=1\nexport B=2");
    nothingWritten.disk.fault = DiskFault::Full;
    CHECK(nothingWritten.editor.install(ShellKind::Zsh, Line, noSession).error() == ShellRcError::Unwritable);
    CHECK(nothingWritten.disk.text(zshRc()) == "export A=1\nexport B=2");
    CHECK(!nothingWritten.disk.touched("writeFile"));

    auto midway = Rig {};
    midway.disk.files.addFile(zshRc(), "export A=1\nexport B=2");
    midway.disk.fault = DiskFault::FullMidway;
    auto const realLine = contour::window::shellRcLine(ShellKind::Zsh, "/usr/bin/contour").value();
    CHECK(midway.editor.install(ShellKind::Zsh, realLine, noSession).error() == ShellRcError::AppendCutShort);
    // The user's own bytes are all still there, in place; only more bytes followed them.
    CHECK(midway.disk.text(zshRc()).starts_with("export A=1\nexport B=2"));
    CHECK(!midway.disk.touched("writeFile"));
}

TEST_CASE("ShellRcEditor: an append that reports success but lands only a prefix is an AppendCutShort",
          "[shellrc]")
{
    auto const line = contour::window::shellRcLine(ShellKind::Zsh, "/usr/bin/contour").value();
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    rig.disk.fault = DiskFault::SilentPrefix;
    rig.disk.landBytes = line.size() / 2;
    CHECK(rig.editor.install(ShellKind::Zsh, line, noSession).error() == ShellRcError::AppendCutShort);
    CHECK(rig.disk.text(zshRc()).starts_with("export A=1\n"));
}

TEST_CASE("ShellRcEditor: a cut-short line counts as a partial install, and nothing more is appended",
          "[shellrc]")
{
    for (auto const shell: { ShellKind::Bash, ShellKind::Zsh, ShellKind::Fish })
    {
        auto const line = contour::window::shellRcLine(shell, "/usr/bin/contour").value();
        auto const rc = [&](Rig const& rig) {
            return rig.editor.inspect(shell, noSession)->file;
        };
        // The chunk is a line ending, the line, its line ending. Every cut from two bytes up to the whole
        // line without its line ending: all but the last leave a part of the line (Partial); the last leaves
        // the whole line, unterminated, which is installed. (A cut after the first byte is tested below.)
        for (auto const landed: std::views::iota(std::size_t { 2 }, line.size() + 2))
        {
            INFO("shell " << static_cast<int>(shell) << " landed " << landed);
            auto const wholeLine = landed == line.size() + 1;
            auto rig = Rig {};
            rig.disk.files.addFile(rc(rig), "export A=1\n");
            rig.disk.fault = DiskFault::SilentPrefix;
            rig.disk.landBytes = landed;
            REQUIRE(rig.editor.install(shell, line, noSession).error()
                    == (wholeLine ? ShellRcError::AppendUnterminated : ShellRcError::AppendCutShort));

            auto const inspected = rig.editor.inspect(shell, noSession);
            REQUIRE(inspected.has_value());
            CHECK(inspected->state == (wholeLine ? RcState::Installed : RcState::Partial));

            auto const before = rig.disk.text(rc(rig));
            rig.disk.fault = DiskFault::None;
            auto const again = rig.editor.install(shell, line, noSession);
            if (wholeLine)
                CHECK(again == RcInstall::AlreadyInstalled);
            else
                CHECK(again.error() == ShellRcError::PartialWrite);
            CHECK(rig.disk.text(rc(rig)) == before);
        }
    }
}

TEST_CASE("ShellRcEditor: what an append cut short left behind decides which error says so", "[shellrc]")
{
    auto const line = contour::window::shellRcLine(ShellKind::Zsh, "/usr/bin/contour").value();
    auto const append = [&](std::string_view before, std::size_t landed) {
        auto rig = Rig {};
        rig.disk.files.addFile(zshRc(), std::string(before));
        rig.disk.fault = DiskFault::SilentPrefix;
        rig.disk.landBytes = landed;
        return rig.editor.install(ShellKind::Zsh, line, noSession).error();
    };
    // Part of Contour's line landed.
    CHECK(append("export A=1\n", line.size() / 2) == ShellRcError::AppendCutShort);
    // Only the line ending that opens the chunk landed.
    CHECK(append("export A=1", 1) == ShellRcError::AppendLineEndingOnly);
    // Only the leading line ending landed.
    CHECK(append("export A=1\n", 1) == ShellRcError::AppendLineEndingOnly);
    // The whole line landed, but not its line ending.
    CHECK(append("export A=1\n", line.size() + 1) == ShellRcError::AppendUnterminated);
    // The file was already cut short before: nothing is written at all.
    CHECK(append("export A=1\nif [ \"$TERM", 0) == ShellRcError::PartialWrite);
}

TEST_CASE("ShellRcEditor: a cut-short install is inspected with the line to delete", "[shellrc]")
{
    for (auto const shell: { ShellKind::Bash, ShellKind::Zsh, ShellKind::Fish })
    {
        auto const line = contour::window::shellRcLine(shell, "/usr/bin/contour").value();
        auto rig = Rig {};
        auto const rc = rig.editor.inspect(shell, noSession)->file;
        auto const cut = line.substr(0, line.size() / 2);
        rig.disk.files.addFile(rc, "export A=1\n" + cut);

        auto const inspected = rig.editor.inspect(shell, noSession);
        REQUIRE(inspected.has_value());
        CHECK(inspected->state == RcState::Partial);
        CHECK(inspected->partialLine == cut);

        // Only a cut-short install carries a line to delete.
        auto clean = Rig {};
        clean.disk.files.addFile(rc, "export A=1\n");
        CHECK(clean.editor.inspect(shell, noSession)->partialLine.empty());
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
    // Not even when it opens with the same guard: only Contour's own continuation makes it Contour's.
    CHECK(rcStateOf(ShellKind::Zsh, "if [ \"$TERM_PROGRAM\" = contour ]; then echo hi") == RcState::Absent);
    CHECK(rcStateOf(ShellKind::Bash, "x\nif [ \"$TERM_PROGRAM\" = contour ]; then echo hi")
          == RcState::Absent);
    CHECK(rcStateOf(ShellKind::Fish, "if test \"$TERM_PROGRAM\" = contour; echo hi") == RcState::Absent);
    CHECK(rcStateOf(ShellKind::Zsh, "export A=1\n") == RcState::Absent);
    // A whole line, with or without its line ending, is Installed.
    auto const line = contour::window::shellRcLine(ShellKind::Zsh, "/usr/bin/contour").value();
    CHECK(rcStateOf(ShellKind::Zsh, line) == RcState::Installed);
    CHECK(rcStateOf(ShellKind::Zsh, line + "\n") == RcState::Installed);
}

TEST_CASE("ShellRcEditor.rcStateOf.PowerShell", "[shellrc]")
{
    struct Case
    {
        ShellKind shell;
        std::string_view content;
        RcState expected;
    };
    constexpr auto Cases = std::array {
        Case {
            ShellKind::Pwsh, ". \"$(Split-Path $PROFILE)/contour-integration.ps1\"\n", RcState::Installed },
        Case { ShellKind::Pwsh, "# . contour-integration.ps1\n", RcState::Absent },
        Case { ShellKind::Pwsh, "  . C:\\contour\\shell-integration.pwsh\r\n", RcState::Installed },
        Case { ShellKind::Pwsh, ". ~/shell-integration.ps1", RcState::Installed },
        Case { ShellKind::Pwsh, "echo contour-integration.ps1\n", RcState::Absent },
        Case { ShellKind::Pwsh,
               "contour generate integration shell pwsh to - | Out-String | Invoke-Expression\n",
               RcState::Installed },
        Case { ShellKind::WindowsPowerShell,
               "contour generate integration shell pwsh to - | Out-String | Invoke-Expression\n",
               RcState::Installed },
        Case { ShellKind::WindowsPowerShell, ". \"$HOME\\contour-integration.ps1\"\n", RcState::Installed },
        Case { ShellKind::Pwsh,
               "if ($env:TERM_PROGRAM -eq 'contour') { . ([scriptblock]::Create((& '/opt/con",
               RcState::Partial },
        // PowerShell's files mean nothing to a POSIX shell.
        Case { ShellKind::Bash, ". ~/contour-integration.ps1\n", RcState::Absent },
    };
    for (auto const& c: Cases)
    {
        INFO(c.content);
        CHECK(rcStateOf(c.shell, c.content) == c.expected);
    }
}

TEST_CASE("ShellRow: the line Contour appends is its guard followed by its continuation", "[shellrc]")
{
    for (auto const& row: contour::config::ShellRows)
    {
        auto const line = contour::window::shellRcLine(row.kind, "/usr/bin/contour").value();
        CHECK(line.starts_with(std::string(row.guard) + std::string(row.continuation)));
    }
}

TEST_CASE("ShellRcEditor: a save without a final newline between the recheck and the append cannot glue",
          "[shellrc]")
{
    auto const line = contour::window::shellRcLine(ShellKind::Zsh, "/usr/bin/contour").value();
    auto rig = Rig {};
    rig.disk.files.addFile(zshRc(), "export A=1\n");
    // Read 1 decides, read 2 re-checks; the user then saves a last line with no line ending.
    rig.disk.afterRead = [&](int read) {
        if (read == 2)
            rig.disk.files.addFile(zshRc(), "export A=1\nexport B=2");
    };
    CHECK(rig.editor.install(ShellKind::Zsh, line, noSession).value() == RcInstall::Appended);
    auto const text = rig.disk.text(zshRc());
    CHECK(text.starts_with("export A=1\nexport B=2\n"));
    CHECK(text.contains("\n" + line + "\n"));
    CHECK(rcStateOf(ShellKind::Zsh, text) == RcState::Installed);
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
    CHECK(rig.editor.install(ShellKind::Zsh, Line, noSession).error() == ShellRcError::Changed);
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
    CHECK(rig.editor.install(ShellKind::Fish, Line, noSession).error() == ShellRcError::Changed);
    CHECK(rig.disk.text(fishRc) == "set -x RACE 1\n");
    CHECK(!rig.disk.touched("appendFile"));
}

TEST_CASE("ShellRcEditor: a line that is not one line is refused", "[shellrc]")
{
    auto rig = Rig {};
    for (auto const line: { std::string_view {}, std::string_view { "a\nb" }, std::string_view { "a\r" } })
        CHECK(rig.editor.install(ShellKind::Zsh, line, noSession).error() == ShellRcError::InvalidLine);
    CHECK(!rig.disk.touched("appendFile"));
}

TEST_CASE("ShellRcEditor: an unknown shell has no file", "[shellrc]")
{
    auto rig = Rig {};
    CHECK(*rig.editor.inspect(ShellKind::Unknown, noSession) == RcInspection {});
    CHECK(rig.editor.install(ShellKind::Unknown, Line, noSession).error() == ShellRcError::Unwritable);
    CHECK(rig.disk.calls.empty());
}

TEST_CASE("NullShellRcEditor: inspects nothing and writes nothing", "[shellrc]")
{
    auto editor = NullShellRcEditor {};
    for (auto const& row: ShellRows)
    {
        CHECK(*editor.inspect(row.kind, noSession) == RcInspection {});
        CHECK(editor.inspect(row.kind, noSession)->state == RcState::Unknown);
        CHECK(editor.install(row.kind, Line, noSession).error() == ShellRcError::Unwritable);
    }
}

TEST_CASE("ShellRcEditor: every error has words", "[shellrc]")
{
    constexpr auto Errors = std::array { ShellRcError::NoHome,
                                         ShellRcError::Unreadable,
                                         ShellRcError::Unwritable,
                                         ShellRcError::NotAFile,
                                         ShellRcError::Changed,
                                         ShellRcError::InvalidLine,
                                         ShellRcError::PartialWrite,
                                         ShellRcError::Superuser,
                                         ShellRcError::AppendCutShort,
                                         ShellRcError::AppendLineEndingOnly,
                                         ShellRcError::AppendUnterminated,
                                         ShellRcError::NoDocuments,
                                         ShellRcError::UnsupportedEncoding,
                                         ShellRcError::AnsiProfile,
                                         ShellRcError::PolicyUnchanged,
                                         ShellRcError::PolicyBlocked };
    static_assert(Errors.size() == std::to_underlying(ShellRcError::PolicyBlocked) + 1U);
    CHECK(describe(ShellRcError::NoHome) == "the home directory is not known");
    CHECK(describe(ShellRcError::Unreadable) == "the file could not be read");
    CHECK(describe(ShellRcError::Unwritable) == "the file could not be written");
    CHECK(describe(ShellRcError::NotAFile) == "it is not a regular file");
    for (auto const error: Errors)
        CHECK(!describe(error).empty());
    // Only an error that left the file as it was may be told after "was not changed".
    CHECK(!changedFile(ShellRcError::Unwritable));
    CHECK(!changedFile(ShellRcError::PartialWrite));
    CHECK(changedFile(ShellRcError::AppendCutShort));
    CHECK(changedFile(ShellRcError::AppendLineEndingOnly));
    CHECK(changedFile(ShellRcError::AppendUnterminated));
    CHECK(describe(ShellRcError::NoDocuments) == "the Documents folder is not known");
    CHECK(describe(ShellRcError::UnsupportedEncoding) == "it is in a text encoding Contour does not write");
    CHECK(describe(ShellRcError::AnsiProfile)
          == "it has no byte-order mark, so Windows PowerShell would misread the non-ASCII characters in "
             "Contour's line");
    CHECK(describe(ShellRcError::PolicyUnchanged) == "PowerShell's script policy could not be changed");
    CHECK(describe(ShellRcError::PolicyBlocked) == "PowerShell's script policy does not let the profile run");
    for (auto const error: { ShellRcError::NoDocuments,
                             ShellRcError::UnsupportedEncoding,
                             ShellRcError::AnsiProfile,
                             ShellRcError::PolicyUnchanged,
                             ShellRcError::PolicyBlocked })
        CHECK(!changedFile(error));
}

TEST_CASE("ShellRcEditor: a path rooted at / is a place on every host, a relative one on none", "[shellrc]")
{
    // The editor and the rc line speak POSIX paths: `/home/me` has no drive on Windows, so is_absolute()
    // would refuse it there and the same test would pass on one host and fail on another.
    CHECK(contour::config::isRootedPath("/home/me"));
    CHECK(contour::config::isRootedPath("/usr/bin/contour"));
    CHECK_FALSE(contour::config::isRootedPath("relative"));
    CHECK_FALSE(contour::config::isRootedPath("./contour"));
    CHECK_FALSE(contour::config::isRootedPath(""));
}

TEST_CASE("ShellRcEditor.powershell.locationOnWindows", "[shellrc]")
{
    auto rig = ProfileRig { documents() };
    CHECK(rig.editor.inspect(ShellKind::Pwsh, noSession)->file == pwshProfile());
    CHECK(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->file == windowsPowerShellProfile());

#ifdef _WIN32
    // A drive is a root only where there are drives.
    auto onDrive = ProfileRig { fs::path { "D:/OneDrive/Documents" } };
    CHECK(onDrive.editor.inspect(ShellKind::Pwsh, noSession)->file
          == fs::path { "D:/OneDrive/Documents/PowerShell/Microsoft.PowerShell_profile.ps1" });
    CHECK(onDrive.editor.inspect(ShellKind::WindowsPowerShell, noSession)->file
          == fs::path { "D:/OneDrive/Documents/WindowsPowerShell/Microsoft.PowerShell_profile.ps1" });
#endif

    // HOME does not stand in for an unknown or relative Documents folder.
    for (auto const& folder: { std::optional<fs::path> {}, std::optional<fs::path> { "Documents" } })
    {
        auto none = ProfileRig { folder };
        for (auto const shell: { ShellKind::Pwsh, ShellKind::WindowsPowerShell })
        {
            CHECK(none.editor.inspect(shell, noSession).error() == ShellRcError::NoDocuments);
            CHECK(none.editor.install(shell, asciiLine(shell), noSession).error()
                  == ShellRcError::NoDocuments);
        }
        CHECK(none.disk.calls.empty());
        CHECK(none.disk.reads == 0);
    }
}

TEST_CASE("ShellRcEditor.powershell.locationOnLinux", "[shellrc]")
{
    auto rig = ProfileRig { std::nullopt,
                            HostPlatform::Other,
                            { { "HOME", "/home/me" }, { "XDG_CONFIG_HOME", "/x" } } };
    auto const profile = fs::path { "/x/powershell/Microsoft.PowerShell_profile.ps1" };
    CHECK(rig.editor.inspect(ShellKind::Pwsh, noSession)->file == profile);
    REQUIRE(rig.editor.install(ShellKind::Pwsh, asciiLine(ShellKind::Pwsh), noSession)
            == RcInstall::Appended);
    CHECK(rig.disk.text(profile) == std::string(Utf8Bom) + "\n" + asciiLine(ShellKind::Pwsh) + "\n");

    // Windows PowerShell is Windows only: nothing to inspect, nothing to write, not even a read.
    rig.disk.calls.clear();
    rig.disk.reads = 0;
    CHECK(*rig.editor.inspect(ShellKind::WindowsPowerShell, noSession) == RcInspection {});
    CHECK(rig.editor.install(ShellKind::WindowsPowerShell, asciiLine(ShellKind::WindowsPowerShell), noSession)
              .error()
          == ShellRcError::Unwritable);
    CHECK(rig.disk.calls.empty());
    CHECK(rig.disk.reads == 0);
}

TEST_CASE("ShellRcEditor.powershell.newProfile", "[shellrc]")
{
    for (auto const shell: { ShellKind::Pwsh, ShellKind::WindowsPowerShell })
    {
        auto rig = ProfileRig { documents() };
        auto const profile = rig.editor.inspect(shell, noSession)->file;
        REQUIRE(!rig.disk.files.isDirectory(profile.parent_path()));
        auto const line = nonAsciiLine(shell);
        REQUIRE(rig.editor.install(shell, line, noSession) == RcInstall::Appended);
        CHECK(rig.disk.files.isDirectory(profile.parent_path()));
        CHECK(rig.disk.text(profile) == std::string(Utf8Bom) + "\n" + line + "\n");
        CHECK(rig.editor.inspect(shell, noSession)->state == RcState::Installed);
    }

    // `New-Item $PROFILE -Force`, as PowerShell's documentation suggests, leaves an empty file: it is new
    // too.
    auto rig = ProfileRig { documents() };
    rig.disk.files.addFile(windowsPowerShellProfile(), "");
    auto const line = nonAsciiLine(ShellKind::WindowsPowerShell);
    REQUIRE(rig.editor.install(ShellKind::WindowsPowerShell, line, noSession) == RcInstall::Appended);
    CHECK(rig.disk.text(windowsPowerShellProfile()) == std::string(Utf8Bom) + "\n" + line + "\n");
}

TEST_CASE("ShellRcEditor.powershell.utf16Profile", "[shellrc]")
{
    auto rig = ProfileRig { documents() };
    auto const before = std::string(Utf16LeBom) + utf16Le("Set-Alias ll ls\r\n");
    rig.disk.files.addFile(windowsPowerShellProfile(), before);
    CHECK(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->state == RcState::Absent);

    auto const line = asciiLine(ShellKind::WindowsPowerShell);
    REQUIRE(rig.editor.install(ShellKind::WindowsPowerShell, line, noSession) == RcInstall::Appended);
    CHECK(rig.disk.text(windowsPowerShellProfile()) == before + utf16Le("\n" + line + "\n"));
    CHECK(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->state == RcState::Installed);
}

TEST_CASE("ShellRcEditor.powershell.utf16AlreadyInstalled", "[shellrc]")
{
    auto rig = ProfileRig { documents() };
    auto const line = asciiLine(ShellKind::WindowsPowerShell);
    auto const before = std::string(Utf16LeBom) + utf16Le("Set-Alias ll ls\r\n" + line + "\r\n");
    rig.disk.files.addFile(windowsPowerShellProfile(), before);
    CHECK(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->state == RcState::Installed);
    CHECK(rig.editor.install(ShellKind::WindowsPowerShell, line, noSession) == RcInstall::AlreadyInstalled);
    CHECK(rig.disk.text(windowsPowerShellProfile()) == before);
    CHECK(!rig.disk.touched("appendFile"));
}

TEST_CASE("ShellRcEditor.powershell.utf16CutShort", "[shellrc]")
{
    // A UTF-16 profile cut short mid-line is Partial, and the line to delete is told in UTF-8.
    auto rig = ProfileRig { documents() };
    auto const line = asciiLine(ShellKind::WindowsPowerShell);
    auto const cut = line.substr(0, line.size() / 2);
    rig.disk.files.addFile(windowsPowerShellProfile(),
                           std::string(Utf16LeBom) + utf16Le("Set-Alias ll ls\n" + cut));
    auto const inspection = rig.editor.inspect(ShellKind::WindowsPowerShell, noSession);
    CHECK(inspection->state == RcState::Partial);
    CHECK(inspection->partialLine == cut);
    CHECK(rig.editor.install(ShellKind::WindowsPowerShell, line, noSession).error()
          == ShellRcError::PartialWrite);

    // An append that lands an odd number of bytes leaves text no decoder reads: it was cut short.
    auto odd = ProfileRig { documents() };
    odd.disk.files.addFile(windowsPowerShellProfile(),
                           std::string(Utf16LeBom) + utf16Le("Set-Alias ll ls\n"));
    odd.disk.fault = DiskFault::SilentPrefix;
    odd.disk.landBytes = 7;
    CHECK(odd.editor.install(ShellKind::WindowsPowerShell, line, noSession).error()
          == ShellRcError::AppendCutShort);
}

TEST_CASE("ShellRcEditor.powershell.ansiProfile", "[shellrc]")
{
    auto const before = std::string { "Set-Alias ll ls\n" };

    // Windows PowerShell reads a BOM-less file in the ANSI code page, which would garble Contour's path.
    auto windows = ProfileRig { documents() };
    windows.disk.files.addFile(windowsPowerShellProfile(), before);
    CHECK(windows.editor
              .install(ShellKind::WindowsPowerShell, nonAsciiLine(ShellKind::WindowsPowerShell), noSession)
              .error()
          == ShellRcError::AnsiProfile);
    CHECK(windows.disk.text(windowsPowerShellProfile()) == before);
    CHECK(!windows.disk.touched("appendFile"));
    CHECK(windows.executionPolicy.attempts.empty());

    // An ASCII line reads the same in every code page.
    auto ascii = ProfileRig { documents() };
    ascii.disk.files.addFile(windowsPowerShellProfile(), before);
    auto const asciiOnly = asciiLine(ShellKind::WindowsPowerShell);
    REQUIRE(ascii.editor.install(ShellKind::WindowsPowerShell, asciiOnly, noSession) == RcInstall::Appended);
    CHECK(ascii.disk.text(windowsPowerShellProfile()) == before + "\n" + asciiOnly + "\n");

    // With a byte order mark Windows PowerShell reads UTF-8 too.
    auto marked = ProfileRig { documents() };
    marked.disk.files.addFile(windowsPowerShellProfile(), std::string(Utf8Bom) + before);
    auto const windowsLine = nonAsciiLine(ShellKind::WindowsPowerShell);
    REQUIRE(marked.editor.install(ShellKind::WindowsPowerShell, windowsLine, noSession)
            == RcInstall::Appended);
    CHECK(marked.disk.text(windowsPowerShellProfile())
          == std::string(Utf8Bom) + before + "\n" + windowsLine + "\n");

    // PowerShell 7 reads a BOM-less file as UTF-8.
    auto pwsh = ProfileRig { documents() };
    pwsh.disk.files.addFile(pwshProfile(), before);
    auto const line = nonAsciiLine(ShellKind::Pwsh);
    REQUIRE(pwsh.editor.install(ShellKind::Pwsh, line, noSession) == RcInstall::Appended);
    CHECK(pwsh.disk.text(pwshProfile()) == before + "\n" + line + "\n");
}

TEST_CASE("ShellRcEditor.powershell.unsupportedEncoding", "[shellrc]")
{
    auto rig = ProfileRig { documents() };
    auto const before = std::string { "\xFE\xFF\0S\0e\0t", 8 }; // UTF-16BE
    rig.disk.files.addFile(pwshProfile(), before);
    CHECK(rig.editor.inspect(ShellKind::Pwsh, noSession).error() == ShellRcError::UnsupportedEncoding);
    CHECK(rig.editor.install(ShellKind::Pwsh, asciiLine(ShellKind::Pwsh), noSession).error()
          == ShellRcError::UnsupportedEncoding);
    CHECK(rig.disk.text(pwshProfile()) == before);
    CHECK(!rig.disk.touched("appendFile"));

    // A UTF-16 file that is not valid UTF-16 cannot be read.
    auto malformed = ProfileRig { documents() };
    malformed.disk.files.addFile(pwshProfile(), std::string(Utf16LeBom) + "S");
    CHECK(malformed.editor.inspect(ShellKind::Pwsh, noSession).error() == ShellRcError::Unreadable);
    CHECK(malformed.editor.install(ShellKind::Pwsh, asciiLine(ShellKind::Pwsh), noSession).error()
          == ShellRcError::Unreadable);
    CHECK(!malformed.disk.touched("appendFile"));
}

TEST_CASE("ShellRcEditor.powershell.policyFix", "[shellrc]")
{
    using contour::config::ExecutionPolicy;
    using contour::config::PolicyScope;
    using contour::config::ProfileScripts;

    auto rig = ProfileRig { documents() };
    CHECK(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->scripts == ProfileScripts::Run);

    rig.executionPolicy.policy = { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::Default };
    CHECK(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->scripts
          == ProfileScripts::NeedUserPolicy);

    auto appendedBeforePolicy = std::optional<bool> {};
    rig.executionPolicy.duringWrite = [&] {
        appendedBeforePolicy = rig.disk.touched("appendFile");
    };
    REQUIRE(
        rig.editor.install(ShellKind::WindowsPowerShell, asciiLine(ShellKind::WindowsPowerShell), noSession)
        == RcInstall::Appended);
    CHECK(rig.executionPolicy.attempts == std::vector { ShellKind::WindowsPowerShell });
    CHECK(appendedBeforePolicy == false);
    CHECK(rig.disk.touched("appendFile"));
    CHECK(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->scripts == ProfileScripts::Run);
}

TEST_CASE("ShellRcEditor.powershell.policyFixFails", "[shellrc]")
{
    using contour::config::ExecutionPolicy;
    using contour::config::PolicyScope;

    for (auto const& existing:
         { std::optional<std::string> {}, std::optional<std::string> { "Set-Alias ll ls\n" } })
    {
        auto rig = ProfileRig { documents() };
        if (existing)
            rig.disk.files.addFile(windowsPowerShellProfile(), *existing);
        rig.executionPolicy.policy = { .policy = ExecutionPolicy::Undefined, .scope = PolicyScope::Default };
        rig.executionPolicy.writes = PolicyWrites::Fail;
        CHECK(rig.editor
                  .install(ShellKind::WindowsPowerShell, asciiLine(ShellKind::WindowsPowerShell), noSession)
                  .error()
              == ShellRcError::PolicyUnchanged);
        CHECK(rig.executionPolicy.attempts.size() == 1);
        CHECK(!rig.disk.touched("appendFile"));
        CHECK(rig.disk.files.exists(windowsPowerShellProfile()) == existing.has_value());
        if (existing)
            CHECK(rig.disk.text(windowsPowerShellProfile()) == *existing);
    }
}

TEST_CASE("ShellRcEditor.powershell.policyBlocked", "[shellrc]")
{
    using contour::config::EffectivePolicy;
    using contour::config::ExecutionPolicy;
    using contour::config::PolicyScope;
    using contour::config::ProfileScripts;

    struct Case
    {
        EffectivePolicy policy;
        ProfileScripts scripts;
    };
    constexpr auto Cases = std::array {
        Case { .policy = { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::GroupPolicy },
               .scripts = ProfileScripts::BlockedByGroupPolicy },
        Case { .policy = { .policy = ExecutionPolicy::AllSigned, .scope = PolicyScope::LocalMachine },
               .scripts = ProfileScripts::SignedOnly },
    };
    for (auto const& c: Cases)
    {
        auto rig = ProfileRig { documents() };
        rig.executionPolicy.policy = c.policy;
        CHECK(rig.editor.inspect(ShellKind::Pwsh, noSession)->scripts == c.scripts);
        CHECK(rig.editor.install(ShellKind::Pwsh, asciiLine(ShellKind::Pwsh), noSession).error()
              == ShellRcError::PolicyBlocked);
        CHECK(rig.executionPolicy.attempts.empty());
        CHECK(!rig.disk.touched("appendFile"));
        CHECK(!rig.disk.files.exists(pwshProfile()));
    }
}

TEST_CASE("ShellRcEditor.powershell.policyFixIntoMissingProfile", "[shellrc]")
{
    using contour::config::ExecutionPolicy;
    using contour::config::PolicyScope;

    auto rig = ProfileRig { documents() };
    rig.executionPolicy.policy = { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::Default };
    auto profileDuringWrite = std::optional<bool> {};
    rig.executionPolicy.duringWrite = [&] {
        profileDuringWrite = rig.disk.files.exists(windowsPowerShellProfile());
    };
    auto const line = nonAsciiLine(ShellKind::WindowsPowerShell);
    REQUIRE(rig.editor.install(ShellKind::WindowsPowerShell, line, noSession) == RcInstall::Appended);
    CHECK(rig.executionPolicy.attempts == std::vector { ShellKind::WindowsPowerShell });
    CHECK(profileDuringWrite == false);
    CHECK(rig.disk.text(windowsPowerShellProfile()) == std::string(Utf8Bom) + "\n" + line + "\n");
}

TEST_CASE("ShellRcEditor.powershell.installedButStopped", "[shellrc]")
{
    using contour::config::ExecutionPolicy;
    using contour::config::PolicyScope;
    using contour::config::ProfileScripts;

    auto const installed =
        std::string(Utf8Bom) + "Set-Alias ll ls\n" + asciiLine(ShellKind::WindowsPowerShell) + "\n";

    SECTION("a policy the user may change is fixed, and no line is added")
    {
        auto rig = ProfileRig { documents() };
        rig.disk.files.addFile(windowsPowerShellProfile(), installed);
        rig.executionPolicy.policy = { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::Default };
        REQUIRE(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->scripts
                == ProfileScripts::NeedUserPolicy);
        CHECK(rig.editor.install(
                  ShellKind::WindowsPowerShell, asciiLine(ShellKind::WindowsPowerShell), noSession)
              == RcInstall::AlreadyInstalled);
        CHECK(rig.executionPolicy.attempts == std::vector { ShellKind::WindowsPowerShell });
        CHECK(!rig.disk.touched("appendFile"));
        CHECK(rig.disk.text(windowsPowerShellProfile()) == installed);
        CHECK(rig.editor.inspect(ShellKind::WindowsPowerShell, noSession)->scripts == ProfileScripts::Run);
    }

    SECTION("a policy write that fails is reported")
    {
        auto rig = ProfileRig { documents() };
        rig.disk.files.addFile(windowsPowerShellProfile(), installed);
        rig.executionPolicy.policy = { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::Default };
        rig.executionPolicy.writes = PolicyWrites::Fail;
        CHECK(rig.editor
                  .install(ShellKind::WindowsPowerShell, asciiLine(ShellKind::WindowsPowerShell), noSession)
                  .error()
              == ShellRcError::PolicyUnchanged);
        CHECK(rig.disk.text(windowsPowerShellProfile()) == installed);
    }

    SECTION("group policy, or signed scripts only, blocks it")
    {
        for (auto const policy: { contour::config::EffectivePolicy { .policy = ExecutionPolicy::Restricted,
                                                                     .scope = PolicyScope::GroupPolicy },
                                  contour::config::EffectivePolicy { .policy = ExecutionPolicy::AllSigned,
                                                                     .scope = PolicyScope::CurrentUser } })
        {
            auto rig = ProfileRig { documents() };
            rig.disk.files.addFile(windowsPowerShellProfile(), installed);
            rig.executionPolicy.policy = policy;
            CHECK(
                rig.editor
                    .install(ShellKind::WindowsPowerShell, asciiLine(ShellKind::WindowsPowerShell), noSession)
                    .error()
                == ShellRcError::PolicyBlocked);
            CHECK(rig.executionPolicy.attempts.empty());
            CHECK(rig.disk.text(windowsPowerShellProfile()) == installed);
        }
    }

    SECTION("a policy that lets it run changes nothing")
    {
        auto rig = ProfileRig { documents() };
        rig.disk.files.addFile(windowsPowerShellProfile(), installed);
        CHECK(rig.editor.install(
                  ShellKind::WindowsPowerShell, asciiLine(ShellKind::WindowsPowerShell), noSession)
              == RcInstall::AlreadyInstalled);
        CHECK(rig.executionPolicy.attempts.empty());
    }
}
