// SPDX-License-Identifier: Apache-2.0
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <span>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <vthost/KnownDaemons.hpp>
#include <vthost/testing/CountingRandom.hpp>
#include <vthost/testing/ForwardingFileSystem.hpp>
#include <vthost/testing/RecordingDurableFiles.hpp>

namespace
{
constexpr auto FirstPin = "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU";
constexpr auto SecondPin = "SHA256:Zm9vYmFyZm9vYmFyZm9vYmFyZm9vYmFyZm9vYmFyZm8";
} // namespace

TEST_CASE("KnownDaemons pins, reads back and forgets", "[vthost][known-daemons]")
{
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto daemons = vthost::KnownDaemons { files, "/state/known_daemons", random };

    CHECK(daemons.pinOf("studio:7297") == std::nullopt);
    REQUIRE(daemons.pin("studio:7297", FirstPin));
    REQUIRE(daemons.pin("build:7297", SecondPin));
    CHECK(daemons.pinOf("studio:7297") == FirstPin);
    CHECK(daemons.pinOf("build:7297") == SecondPin);

    // A new object over the same file sees the same pins: they are kept, not cached.
    auto const again = vthost::KnownDaemons { files, "/state/known_daemons", random };
    CHECK(again.pinOf("studio:7297") == FirstPin);

    // Pinning again replaces rather than adds.
    REQUIRE(daemons.pin("studio:7297", SecondPin));
    CHECK(daemons.pinOf("studio:7297") == SecondPin);
    CHECK(files.readFile("/state/known_daemons")->contains("studio:7297"));

    auto const removed = daemons.forget("studio:7297");
    REQUIRE(removed);
    CHECK(*removed);
    CHECK(daemons.pinOf("studio:7297") == std::nullopt);
    CHECK(daemons.pinOf("build:7297") == SecondPin);

    auto const none = daemons.forget("studio:7297");
    REQUIRE(none);
    CHECK_FALSE(*none);
}

TEST_CASE("KnownDaemons skips a corrupt line", "[vthost][known-daemons]")
{
    auto const files = core::platform::testing::InMemoryFileSystem {
        { .path = "/state/known_daemons",
          .content = std::string { "garbage\n# a comment\nstudio:7297 not-a-fingerprint\nbuild:7297 " }
                     + SecondPin + "\n\nstudio:7297 " + FirstPin + " extra\n" }
    };
    auto random = vthost::testing::CountingRandom {};
    auto daemons = vthost::KnownDaemons { files, "/state/known_daemons", random };

    CHECK(daemons.pinOf("build:7297") == SecondPin);
    CHECK(daemons.pinOf("studio:7297") == std::nullopt);

    // The damage does not stop a new pin, and the good line survives it.
    REQUIRE(daemons.pin("studio:7297", FirstPin));
    CHECK(daemons.pinOf("studio:7297") == FirstPin);
    CHECK(daemons.pinOf("build:7297") == SecondPin);
}

TEST_CASE("KnownDaemons writes the file whole, synced, and renamed over the old one",
          "[vthost][known-daemons]")
{
    auto const files = vthost::testing::RecordingDurableFiles {};
    auto random = vthost::testing::CountingRandom {};
    auto daemons = vthost::KnownDaemons { files, "/state/known_daemons", random };
    REQUIRE(daemons.pin("studio:7297", FirstPin));
    REQUIRE(daemons.pin("build:7297", SecondPin));

    auto const expected = std::string { "studio:7297 " } + FirstPin + "\nbuild:7297 " + SecondPin + "\n";
    // The second write: made beside the file, synced with everything in it, then renamed over it.
    REQUIRE(files.calls.size() == 6);
    CHECK(files.calls[3].op == vthost::testing::DurableOp::CreateExclusive);
    // A name of its own for each write: the first took bytes 1..8, this one 9..16.
    CHECK(files.calls[0].path == "/state/known_daemons.0102030405060708.tmp");
    CHECK(files.calls[3].path == "/state/known_daemons.090A0B0C0D0E0F10.tmp");
    CHECK(files.calls[4]
          == vthost::testing::RecordedDurableCall { .op = vthost::testing::DurableOp::SyncFile,
                                                    .path = "/state/known_daemons.090A0B0C0D0E0F10.tmp",
                                                    .content = expected });
    CHECK(files.calls[5].op == vthost::testing::DurableOp::Replace);
    CHECK(files.files.readFile("/state/known_daemons") == expected);
    CHECK_FALSE(files.files.exists("/state/known_daemons.090A0B0C0D0E0F10.tmp"));

    // A write that fails leaves the pins as they were.
    auto failing = vthost::testing::RecordingDurableFiles {};
    auto other = vthost::KnownDaemons { failing, "/state/known_daemons", random };
    REQUIRE(other.pin("studio:7297", FirstPin));
    failing.failing = vthost::testing::DurableOp::Replace;
    CHECK_FALSE(other.pin("studio:7297", SecondPin));
    CHECK(other.pinOf("studio:7297") == FirstPin);
}

namespace
{
/// Files that exist but cannot be read.
class UnreadableFiles final: public vthost::testing::ForwardingFileSystem
{
  public:
    [[nodiscard]] std::expected<std::string, std::string> readFile(
        std::filesystem::path const& path) const override
    {
        return std::unexpected("Permission denied: " + path.string());
    }
};
} // namespace

TEST_CASE("KnownDaemons does not rewrite a file it cannot read", "[vthost][known-daemons]")
{
    auto files = UnreadableFiles {};
    REQUIRE(files.files.writeFile("/state/known_daemons", std::string { "studio:7297 " } + FirstPin + "\n"));
    auto random = vthost::testing::CountingRandom {};
    auto daemons = vthost::KnownDaemons { files, "/state/known_daemons", random };

    // Nor reads it as "no pin": that would have every daemon, a changed one too, asked about anew.
    auto const pinned = daemons.pinOf("studio:7297");
    REQUIRE_FALSE(pinned);
    CHECK(pinned.error().contains("/state/known_daemons"));
    CHECK_FALSE(daemons.pin("build:7297", SecondPin));
    CHECK_FALSE(daemons.forget("studio:7297"));
    CHECK(*files.files.readFile("/state/known_daemons") == std::string { "studio:7297 " } + FirstPin + "\n");
}

TEST_CASE("KnownDaemons refuses a directory where its file should be", "[vthost][known-daemons]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    REQUIRE(files.createDirectories("/state/known_daemons"));
    auto random = vthost::testing::CountingRandom {};
    auto daemons = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const pinned = daemons.pinOf("studio:7297");
    REQUIRE_FALSE(pinned);
    CHECK(pinned.error().contains("is a directory"));
    CHECK_FALSE(daemons.pin("studio:7297", FirstPin));
}

TEST_CASE("KnownDaemons reads a line split by any whitespace, CRLF ended too", "[vthost][known-daemons]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    REQUIRE(files.writeFile("/state/known_daemons",
                            std::string { "studio:7297 " } + FirstPin + "\r\n" + "build:7297\t \t" + SecondPin
                                + "  \r\n"));
    auto random = vthost::testing::CountingRandom {};
    auto const daemons = vthost::KnownDaemons { files, "/state/known_daemons", random };
    CHECK(daemons.pinOf("studio:7297") == FirstPin);
    CHECK(daemons.pinOf("build:7297") == SecondPin);
}

TEST_CASE("every spelling of one daemon names one pin key", "[vthost][known-daemons]")
{
    CHECK(vthost::pinKey("studio", 7297) == "studio:7297");
    CHECK(vthost::pinKey("192.0.2.1", 7297) == "192.0.2.1:7297");
    CHECK(vthost::pinKey("::1", 7297) == "[::1]:7297");
    CHECK(vthost::pinKey("fe80::1%eth0", 7297) == "[fe80::1%eth0]:7297");

    CHECK(vthost::pinKeyOf("studio:7297") == "studio:7297");
    CHECK(vthost::pinKeyOf("studio:07297") == "studio:7297");
    CHECK(vthost::pinKeyOf("[::1]:7297") == "[::1]:7297");
    CHECK(vthost::pinKeyOf("::1:7297") == "[::1]:7297");
    CHECK(vthost::pinKeyOf("[studio]:7297") == "studio:7297");
    CHECK(vthost::pinKeyOf("studio") == std::nullopt);
    CHECK(vthost::pinKeyOf("studio:0") == std::nullopt);

    // What pinKey() writes, pinKeyOf() reads back unchanged.
    for (auto const* host: { "studio", "192.0.2.1", "::1", "2001:db8::7" })
        CHECK(vthost::pinKeyOf(vthost::pinKey(host, 7297)) == vthost::pinKey(host, 7297));
}

TEST_CASE("KnownDaemons reads no pins only where the file is shown to be absent", "[vthost][known-daemons]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-known-daemons" };
    auto random = vthost::testing::CountingRandom {};

    // Its directory not made yet: a first run, with no pins.
    auto const fresh = vthost::KnownDaemons { core::platform::NativeFileSystem::instance(),
                                              dir.path() / "config" / "contour" / "known_daemons",
                                              random };
    CHECK(fresh.pinOf("studio:7297") == std::nullopt);

#ifndef _WIN32
    // A directory this user may not search: the file may well be there, and is not taken for absent.
    if (::geteuid() != 0)
    {
        auto const locked = dir.path() / "locked";
        std::filesystem::create_directory(locked);
        std::ofstream { locked / "known_daemons" } << "studio:7297 " << FirstPin << "\n";
        std::filesystem::permissions(
            locked, std::filesystem::perms::none, std::filesystem::perm_options::replace);
        auto const hidden = vthost::KnownDaemons { core::platform::NativeFileSystem::instance(),
                                                   locked / "known_daemons",
                                                   random };
        auto const pinned = hidden.pinOf("studio:7297");
        std::filesystem::permissions(
            locked, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
        REQUIRE_FALSE(pinned);
        CHECK(pinned.error().contains("known_daemons"));
    }
#endif
}
