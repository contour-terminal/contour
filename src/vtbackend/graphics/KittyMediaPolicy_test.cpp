// SPDX-License-Identifier: Apache-2.0
//
// The pure kitty file-medium policy: which canonical paths and which opened files a command may read. The
// paths are spelled POSIX-like and made absolute on the platform (onDrive()), so every row but the system
// trees' runs on Windows too, under the path case the platform's policy compares with.

#include <vtbackend/graphics/KittyMediaDefaults.hpp>
#include <vtbackend/graphics/KittyMediaPolicy.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <optional>
#include <string>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace vtbackend::kitty_graphics;

namespace
{
/// @return @p path as an absolute path on this platform: on Windows on drive `C:`, elsewhere as it is. A
///         relative @p path stays relative.
[[nodiscard]] std::filesystem::path onDrive(std::string_view path)
{
#ifdef _WIN32
    if (path.starts_with('/'))
        return std::filesystem::path { std::format("C:{}", path) }.lexically_normal();
#endif
    return std::filesystem::path { path };
}

/// @return @p path as a command sends it, made absolute as onDrive() does: on Windows prefixed with `C:`.
[[nodiscard]] std::string requestFor(std::string_view path)
{
#ifdef _WIN32
    if (path.starts_with('/'))
        return std::format("C:{}", path);
#endif
    return std::string { path };
}

/// The policy every case judges against: temp root `/tmp`, allow-list root under a home directory.
[[nodiscard]] MediaPolicy testPolicy()
{
    return MediaPolicy { .transmission = FileTransmission::Enabled,
                         .temporaryRoots = { onDrive("/tmp") },
                         .temporaryFileOnlyRoots = { onDrive("/dev/shm") },
                         .allowedRoots = { onDrive("/home/u/.claude/plugins/data") },
                         .sizeLimit = 1024,
                         .pathCase = platformPathCase() };
}

/// One row of the path table: what checkPath() answers for a path and medium.
struct PathRow
{
    std::string_view path;
    Medium medium;
    std::optional<MediaRefusal> refusal; ///< nullopt: accepted.
};

/// Checks what checkPath() answers for @p row under @p policy.
void checkRow(MediaPolicy const& policy, PathRow const& row)
{
    INFO(std::format("{} t={}", row.path, static_cast<char>(row.medium)));
    auto const verdict = checkPath(policy, onDrive(row.path), row.medium);
    if (row.refusal)
    {
        REQUIRE_FALSE(verdict.has_value());
        CHECK(verdict.error() == *row.refusal);
    }
    else
        CHECK(verdict.has_value());
}
} // namespace

TEST_CASE("KittyMediaPolicy.checkPath", "[kitty][media]")
{
    auto const row = GENERATE(
        PathRow { "/tmp/tty-graphics-protocol-x.rgba", Medium::File, std::nullopt },
        PathRow { "/tmp/tty-graphics-protocol-x.rgba", Medium::TemporaryFile, std::nullopt },
        PathRow { "/home/u/.claude/plugins/data/rich-terminal/a.png", Medium::File, std::nullopt },
        PathRow { "/dev/shm/tty-graphics-protocol-y", Medium::TemporaryFile, std::nullopt },
        PathRow { "/home/u/.ssh/id_ed25519", Medium::File, MediaRefusal::OutsideRoots },
        PathRow { "/tmpx/tty-graphics-protocol-x.rgba", Medium::File, MediaRefusal::OutsideRoots },
        PathRow { "/tmp", Medium::File, MediaRefusal::OutsideRoots },
        PathRow { "tmp/tty-graphics-protocol-x.rgba", Medium::File, MediaRefusal::OutsideRoots },
        PathRow { "/home/u/.claude/plugins/data/.hidden/a.png", Medium::File, MediaRefusal::HiddenComponent },
        PathRow {
            "/tmp/.x/tty-graphics-protocol-x.rgba", Medium::TemporaryFile, MediaRefusal::HiddenComponent },
        // Read without the marker, as kitty does: the marker decides only the deletion.
        PathRow { "/tmp/a.png", Medium::TemporaryFile, std::nullopt },
        PathRow { "/home/u/.claude/plugins/data/tty-graphics-protocol-a",
                  Medium::TemporaryFile,
                  MediaRefusal::OutsideRoots },
        PathRow {
            "/tmp/tty-graphics-protocol-x.rgba", Medium::SharedMemory, MediaRefusal::UnsupportedMedium },
        PathRow { "tty-graphics-protocol-shm", Medium::SharedMemory, MediaRefusal::UnsupportedMedium },
        PathRow { "/tmp/tty-graphics-protocol-x.rgba", Medium::Direct, MediaRefusal::UnsupportedMedium });
    checkRow(testPolicy(), row);
}

#ifndef _WIN32
TEST_CASE("KittyMediaPolicy.checkPath.system_trees", "[kitty][media]")
{
    // POSIX's: on Windows `/proc` and `/dev` name no tree, and are not even absolute paths there.
    auto const row =
        GENERATE(PathRow { "/dev/shm/x", Medium::File, MediaRefusal::SystemPath },
                 PathRow { "/dev/shm/tty-graphics-protocol-y", Medium::File, MediaRefusal::SystemPath },
                 PathRow { "/tmp/../home/u/.ssh/id_ed25519", Medium::File, MediaRefusal::HiddenComponent },
                 PathRow { "/proc/self/environ", Medium::File, MediaRefusal::SystemPath },
                 PathRow { "/sys/kernel/notes", Medium::File, MediaRefusal::SystemPath },
                 PathRow { "/dev/sda", Medium::File, MediaRefusal::SystemPath });
    checkRow(testPolicy(), row);
}
#endif

TEST_CASE("KittyMediaPolicy.checkPath.path_case", "[kitty][media]")
{
    // Windows compares a root's components ignoring ASCII case, and either separator alike; the other
    // platforms byte for byte.
    auto policy = testPolicy();
    auto const spelling = onDrive("/TMP/tty-graphics-protocol-x.rgba");

    policy.pathCase = PathCase::Insensitive;
    CHECK(checkPath(policy, spelling, Medium::File).has_value());
    CHECK(enclosingRoot(policy, spelling, Medium::File) == onDrive("/tmp"));

    policy.pathCase = PathCase::Sensitive;
    auto const verdict = checkPath(policy, spelling, Medium::File);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error() == MediaRefusal::OutsideRoots);
}

TEST_CASE("KittyMediaPolicy.checkPath.disabled_refuses_everything", "[kitty][media]")
{
    auto policy = testPolicy();
    policy.transmission = FileTransmission::Disabled;
    auto const medium = GENERATE(Medium::File, Medium::TemporaryFile, Medium::SharedMemory);
    auto const verdict = checkPath(policy, onDrive("/tmp/tty-graphics-protocol-x.rgba"), medium);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error() == MediaRefusal::Disabled);
}

TEST_CASE("KittyMediaPolicy.checkPath.temp_root_below_a_hidden_directory", "[kitty][media]")
{
    // A `$TMPDIR` that itself lies below a hidden directory of another root still accepts its own files:
    // the components counted are the ones below the root that matched.
    auto policy = testPolicy();
    policy.temporaryRoots.emplace_back(onDrive("/tmp/.private/u"));
    CHECK(checkPath(policy, onDrive("/tmp/.private/u/tty-graphics-protocol-a"), Medium::TemporaryFile)
              .has_value());
}

TEST_CASE("KittyMediaPolicy.requestedPath", "[kitty][media]")
{
    auto const policy = testPolicy();

    SECTION("accepted: the normal path, its root and what lies below it")
    {
        auto const requested =
            requestedPath(policy, Medium::File, requestFor("/tmp/./d/../d/tty-graphics-protocol-a"));
        REQUIRE(requested.has_value());
        CHECK(requested->path == onDrive("/tmp/d/tty-graphics-protocol-a"));
        CHECK(requested->root == onDrive("/tmp"));
        CHECK(requested->belowRoot == std::vector<std::filesystem::path> { "d", "tty-graphics-protocol-a" });
    }

    SECTION("UTF-8 is decoded as UTF-8, whatever the code page")
    {
        auto const requested = requestedPath(policy, Medium::File, requestFor("/tmp/b\xC3\xBCro.png"));
        REQUIRE(requested.has_value());
        CHECK(requested->belowRoot.back() == std::filesystem::path { u8"b\u00FCro.png" });
    }

    struct Refused
    {
        std::string path;
        Medium medium;
        MediaRefusal refusal;
    };
    auto const row = GENERATE(
        Refused { "", Medium::File, MediaRefusal::Unreadable },
        Refused { "/tmp/a\0b"s, Medium::File, MediaRefusal::Unreadable },
        Refused { "/home/u/.ssh/id_ed25519", Medium::File, MediaRefusal::OutsideRoots },
        Refused { "/tmp/a", Medium::Direct, MediaRefusal::UnsupportedMedium },
        Refused { std::string(MaxFilePathLength + 1, 'a'), Medium::File, MediaRefusal::PathTooLong });
    INFO(row.path);
    auto const requested = requestedPath(policy, row.medium, requestFor(row.path));
    REQUIRE_FALSE(requested.has_value());
    CHECK(requested.error() == row.refusal);
}

TEST_CASE("KittyMediaPolicy.checkOpenedFile", "[kitty][media]")
{
    auto const accepted =
        OpenedFile { .kind = FileKind::Regular, .owner = FileOwner::Reader, .linkCount = 1, .size = 1024 };
    CHECK(checkOpenedFile(testPolicy(), accepted).has_value());

    struct FileCase
    {
        OpenedFile file;
        MediaRefusal refusal;
    };
    auto const row = GENERATE_COPY(
        FileCase {
            OpenedFile { .kind = FileKind::Other, .owner = FileOwner::Reader, .linkCount = 1, .size = 4 },
            MediaRefusal::NotRegularFile },
        FileCase {
            OpenedFile { .kind = FileKind::Regular, .owner = FileOwner::Other, .linkCount = 1, .size = 4 },
            MediaRefusal::WrongOwner },
        FileCase {
            OpenedFile { .kind = FileKind::Regular, .owner = FileOwner::Reader, .linkCount = 2, .size = 4 },
            MediaRefusal::MultipleLinks },
        FileCase { OpenedFile {
                       .kind = FileKind::Regular, .owner = FileOwner::Reader, .linkCount = 1, .size = 1025 },
                   MediaRefusal::TooLarge });
    auto const verdict = checkOpenedFile(testPolicy(), row.file);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error() == row.refusal);
}

TEST_CASE("KittyMediaPolicy.deletionAfterRead", "[kitty][media]")
{
    auto const policy = testPolicy();
    auto const deletion = [&](std::string_view path, Medium medium) {
        return deletionAfterRead(policy, onDrive(path), medium);
    };
    CHECK(deletion("/tmp/tty-graphics-protocol-x.rgba", Medium::TemporaryFile) == DeletionAfterRead::Delete);
    CHECK(deletion("/tmp/tty-graphics-protocol-x.rgba", Medium::File) == DeletionAfterRead::Keep);
    CHECK(deletion("/home/u/.claude/plugins/data/rich-terminal/a.png", Medium::File)
          == DeletionAfterRead::Keep);
    CHECK(deletion("/home/u/.claude/plugins/data/tty-graphics-protocol-a", Medium::TemporaryFile)
          == DeletionAfterRead::Keep);
    CHECK(deletion("/tmp/a.png", Medium::TemporaryFile) == DeletionAfterRead::Keep);
    // The marker counts anywhere in the full path, as the protocol has it.
    CHECK(deletion("/tmp/tty-graphics-protocol-d/a.png", Medium::TemporaryFile) == DeletionAfterRead::Delete);
}

TEST_CASE("KittyMediaPolicy.isRemoteShellClient", "[kitty][media]")
{
    auto const remote = GENERATE("ssh"sv,
                                 "mosh-client"sv,
                                 "mosh"sv,
                                 "telnet"sv,
                                 "rlogin"sv,
                                 "et"sv,
                                 "autossh"sv,
                                 "sshpass"sv,
                                 "kitten"sv,
                                 "/usr/bin/ssh"sv,
                                 // A multiplexer's client relays whatever its panes run, ssh included.
                                 "tmux"sv,
                                 "screen"sv,
                                 "zellij"sv,
                                 // A container's or a VM's front end relays another filesystem's programs.
                                 "docker"sv,
                                 "podman"sv,
                                 "kubectl"sv,
                                 "distrobox-enter"sv,
                                 "vagrant"sv,
                                 // Windows names its images with `.exe`, in any case, and either separator.
                                 "ssh.exe"sv,
                                 "SSH.EXE"sv,
                                 "C:\\Windows\\System32\\OpenSSH\\ssh.exe"sv,
                                 "C:/Program Files/PuTTY/plink.exe"sv,
                                 "wsl.exe"sv,
                                 "wslhost.exe"sv,
                                 "hvc.exe"sv,
                                 "Docker.exe"sv);
    CHECK(isRemoteShellClient(remote));
    auto const local = GENERATE("bash"sv,
                                "zsh"sv,
                                "claude"sv,
                                "node"sv,
                                "sshd"sv,
                                ""sv,
                                "/usr/bin/bash"sv,
                                "pwsh.exe"sv,
                                "node.exe"sv,
                                "sshd.exe"sv,
                                ".exe"sv,
                                "ssh.exe.bak"sv,
                                // A shell running WSL's programs is not itself the relay: wsl.exe is.
                                "bash.exe"sv);
    CHECK_FALSE(isRemoteShellClient(local));
}

TEST_CASE("KittyMediaPolicy.enclosingRoot", "[kitty][media]")
{
    auto policy = testPolicy();
    policy.temporaryRoots = { onDrive("/tmp"), onDrive("/tmp/a"), onDrive("/tmp/.h") };
    policy.allowedRoots = { onDrive("/home/u/.claude/plugins/data"), onDrive("/tmp/a/b") };

    struct RootCase
    {
        std::string_view path;
        Medium medium;
        std::optional<std::string_view> root;
    };
    auto const row = GENERATE(
        // The deepest root wins.
        RootCase { "/tmp/a/x", Medium::File, "/tmp/a" },
        RootCase { "/tmp/a/b/x", Medium::File, "/tmp/a/b" },
        RootCase { "/tmp/x", Medium::File, "/tmp" },
        // A root with a hidden component between it and the path is skipped; one below that component is not.
        RootCase { "/tmp/.h/x", Medium::File, "/tmp/.h" },
        RootCase { "/tmp/a/.h/x", Medium::File, std::nullopt },
        // t=t never walks from an allow-list root: /tmp/a/b is one, so /tmp/a encloses.
        RootCase { "/tmp/a/b/x", Medium::TemporaryFile, "/tmp/a" },
        RootCase { "/home/u/.claude/plugins/data/x", Medium::TemporaryFile, std::nullopt },
        RootCase { "/dev/shm/x", Medium::TemporaryFile, "/dev/shm" },
        RootCase { "/dev/shm/x", Medium::File, std::nullopt },
        RootCase { "/elsewhere/x", Medium::File, std::nullopt });

    INFO(std::format("{} t={}", row.path, static_cast<char>(row.medium)));
    auto const root = enclosingRoot(policy, onDrive(row.path), row.medium);
    if (row.root)
    {
        REQUIRE(root.has_value());
        CHECK(*root == onDrive(*row.root));
    }
    else
        CHECK_FALSE(root.has_value());
}
