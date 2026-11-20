// SPDX-License-Identifier: Apache-2.0
//
// The pure kitty file-medium policy: which canonical paths and which opened files a command may read.

#include <vtbackend/graphics/KittyMediaPolicy.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <optional>

using namespace vtbackend::kitty_graphics;

namespace
{
/// The policy every case judges against: temp root `/tmp`, allow-list root under a home directory.
[[nodiscard]] MediaPolicy testPolicy()
{
    return MediaPolicy { .transmission = FileTransmission::Enabled,
                         .temporaryRoots = { "/tmp", "/dev/shm" },
                         .allowedRoots = { "/home/u/.claude/plugins/data" },
                         .userId = 1000,
                         .sizeLimit = 1024 };
}

/// One row of the path table: what checkPath() answers for a path and medium.
struct PathCase
{
    std::string_view path;
    Medium medium;
    std::optional<MediaRefusal> refusal; ///< nullopt: accepted.
};
} // namespace

TEST_CASE("KittyMediaPolicy.checkPath", "[kitty][media]")
{
    auto const row = GENERATE(
        PathCase { "/tmp/tty-graphics-protocol-x.rgba", Medium::File, std::nullopt },
        PathCase { "/tmp/tty-graphics-protocol-x.rgba", Medium::TemporaryFile, std::nullopt },
        PathCase { "/home/u/.claude/plugins/data/rich-terminal/a.png", Medium::File, std::nullopt },
        PathCase { "/dev/shm/tty-graphics-protocol-y", Medium::TemporaryFile, std::nullopt },
        PathCase { "/home/u/.ssh/id_ed25519", Medium::File, MediaRefusal::OutsideRoots },
        PathCase { "/tmpx/tty-graphics-protocol-x.rgba", Medium::File, MediaRefusal::OutsideRoots },
        PathCase { "/tmp", Medium::File, MediaRefusal::OutsideRoots },
        PathCase { "tmp/tty-graphics-protocol-x.rgba", Medium::File, MediaRefusal::OutsideRoots },
        PathCase {
            "/home/u/.claude/plugins/data/.hidden/a.png", Medium::File, MediaRefusal::HiddenComponent },
        PathCase {
            "/tmp/.x/tty-graphics-protocol-x.rgba", Medium::TemporaryFile, MediaRefusal::HiddenComponent },
        PathCase { "/tmp/../home/u/.ssh/id_ed25519", Medium::File, MediaRefusal::HiddenComponent },
        PathCase { "/proc/self/environ", Medium::File, MediaRefusal::SystemPath },
        PathCase { "/sys/kernel/notes", Medium::File, MediaRefusal::SystemPath },
        PathCase { "/dev/sda", Medium::File, MediaRefusal::SystemPath },
        PathCase { "/tmp/a.png", Medium::TemporaryFile, MediaRefusal::NotTemporaryName },
        PathCase { "/home/u/.claude/plugins/data/tty-graphics-protocol-a",
                   Medium::TemporaryFile,
                   MediaRefusal::OutsideRoots },
        PathCase {
            "/tmp/tty-graphics-protocol-x.rgba", Medium::SharedMemory, MediaRefusal::UnsupportedMedium },
        PathCase { "tty-graphics-protocol-shm", Medium::SharedMemory, MediaRefusal::UnsupportedMedium },
        PathCase { "/tmp/tty-graphics-protocol-x.rgba", Medium::Direct, MediaRefusal::UnsupportedMedium });

    INFO(std::format("{} t={}", row.path, static_cast<char>(row.medium)));
    auto const verdict = checkPath(testPolicy(), std::filesystem::path { row.path }, row.medium);
    if (row.refusal)
    {
        REQUIRE_FALSE(verdict.has_value());
        CHECK(verdict.error() == *row.refusal);
    }
    else
        CHECK(verdict.has_value());
}

TEST_CASE("KittyMediaPolicy.checkPath.disabled_refuses_everything", "[kitty][media]")
{
    auto policy = testPolicy();
    policy.transmission = FileTransmission::Disabled;
    auto const medium = GENERATE(Medium::File, Medium::TemporaryFile, Medium::SharedMemory);
    auto const verdict = checkPath(policy, "/tmp/tty-graphics-protocol-x.rgba", medium);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error() == MediaRefusal::Disabled);
}

TEST_CASE("KittyMediaPolicy.checkPath.temp_root_below_a_hidden_directory", "[kitty][media]")
{
    // A `$TMPDIR` that itself lies below a hidden directory of another root still accepts its own files:
    // the components counted are the ones below the root that matched.
    auto policy = testPolicy();
    policy.temporaryRoots.emplace_back("/tmp/.private/u");
    CHECK(checkPath(policy, "/tmp/.private/u/tty-graphics-protocol-a", Medium::TemporaryFile).has_value());
}

TEST_CASE("KittyMediaPolicy.checkOpenedFile", "[kitty][media]")
{
    auto const accepted =
        OpenedFile { .kind = FileKind::Regular, .ownerId = 1000, .linkCount = 1, .size = 1024 };
    CHECK(checkOpenedFile(testPolicy(), accepted).has_value());

    struct FileCase
    {
        OpenedFile file;
        MediaRefusal refusal;
    };
    auto const row = GENERATE_COPY(
        FileCase { OpenedFile { .kind = FileKind::Other, .ownerId = 1000, .linkCount = 1, .size = 4 },
                   MediaRefusal::NotRegularFile },
        FileCase { OpenedFile { .kind = FileKind::Regular, .ownerId = 0, .linkCount = 1, .size = 4 },
                   MediaRefusal::WrongOwner },
        FileCase { OpenedFile { .kind = FileKind::Regular, .ownerId = 1000, .linkCount = 2, .size = 4 },
                   MediaRefusal::MultipleLinks },
        FileCase { OpenedFile { .kind = FileKind::Regular, .ownerId = 1000, .linkCount = 1, .size = 1025 },
                   MediaRefusal::TooLarge });
    auto const verdict = checkOpenedFile(testPolicy(), row.file);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error() == row.refusal);
}

TEST_CASE("KittyMediaPolicy.deletionAfterRead", "[kitty][media]")
{
    auto const policy = testPolicy();
    CHECK(deletionAfterRead(policy, "/tmp/tty-graphics-protocol-x.rgba", Medium::TemporaryFile)
          == DeletionAfterRead::Delete);
    CHECK(deletionAfterRead(policy, "/tmp/tty-graphics-protocol-x.rgba", Medium::File)
          == DeletionAfterRead::Keep);
    CHECK(deletionAfterRead(policy, "/home/u/.claude/plugins/data/rich-terminal/a.png", Medium::File)
          == DeletionAfterRead::Keep);
    CHECK(deletionAfterRead(
              policy, "/home/u/.claude/plugins/data/tty-graphics-protocol-a", Medium::TemporaryFile)
          == DeletionAfterRead::Keep);
    CHECK(deletionAfterRead(policy, "/tmp/a.png", Medium::TemporaryFile) == DeletionAfterRead::Keep);
}
