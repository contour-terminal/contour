// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <net/Tls.h>
#include <vthost/Token.h>

using vthost::readTokenFile;
using vthost::resolveToken;

namespace
{
/// A token file that removes itself when the test ends.
///
/// Declared FIRST in the test so its destructor runs after every reader above it has closed:
/// Windows refuses to delete a file any handle still holds open.
class ScratchTokenFile
{
  public:
    ScratchTokenFile(std::string_view stem, std::string_view contents):
        _path(std::filesystem::temp_directory_path() / ("contour-token-" + std::string { stem }))
    {
        // Binary, so a "\r\n" under test reaches the file as written rather than being translated.
        auto out = std::ofstream { _path, std::ios::out | std::ios::binary | std::ios::trunc };
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    ~ScratchTokenFile()
    {
        auto ec = std::error_code {};
        std::filesystem::remove(_path, ec);
    }

    ScratchTokenFile(ScratchTokenFile const&) = delete;
    ScratchTokenFile& operator=(ScratchTokenFile const&) = delete;
    ScratchTokenFile(ScratchTokenFile&&) = delete;
    ScratchTokenFile& operator=(ScratchTokenFile&&) = delete;

    [[nodiscard]] std::string path() const { return _path.string(); }

  private:
    std::filesystem::path _path;
};
} // namespace

TEST_CASE("a token file yields its contents", "[vthost][token]")
{
    auto const file = ScratchTokenFile { "plain", "swordfish" };
    auto const token = readTokenFile(file.path());
    REQUIRE(token.has_value());
    CHECK(*token == "swordfish");
}

TEST_CASE("a token file's trailing whitespace is not part of the secret", "[vthost][token]")
{
    // `echo secret > token` is how everyone writes one of these, and nobody expects the newline
    // it appends to become part of the token.
    SECTION("a trailing newline")
    {
        auto const file = ScratchTokenFile { "newline", "swordfish\n" };
        CHECK(readTokenFile(file.path()).value() == "swordfish");
    }

    SECTION("a CRLF, as a Windows editor writes it")
    {
        auto const file = ScratchTokenFile { "crlf", "swordfish\r\n" };
        CHECK(readTokenFile(file.path()).value() == "swordfish");
    }

    SECTION("an interior carriage return goes too, wherever it sits")
    {
        auto const file = ScratchTokenFile { "cr",
                                             "sword\r"
                                             "fish\r\n" };
        CHECK(readTokenFile(file.path()).value() == "swordfish");
    }

    SECTION("trailing spaces and tabs")
    {
        auto const file = ScratchTokenFile { "spaces", "swordfish \t \n" };
        CHECK(readTokenFile(file.path()).value() == "swordfish");
    }
}

TEST_CASE("a token may contain interior spaces", "[vthost][token]")
{
    // A passphrase is a legitimate token; only the TRAILING whitespace is noise.
    auto const file = ScratchTokenFile { "passphrase", "correct horse battery staple\n" };
    CHECK(readTokenFile(file.path()).value() == "correct horse battery staple");
}

TEST_CASE("an empty or unreadable token file is an error, not an empty token", "[vthost][token]")
{
    // Silently yielding "" would disable authentication, which is the one outcome a token file
    // must never produce.
    SECTION("empty")
    {
        auto const file = ScratchTokenFile { "empty", "\n" };
        auto const token = readTokenFile(file.path());
        REQUIRE_FALSE(token.has_value());
        CHECK(token.error().contains("is empty"));
    }

    SECTION("missing")
    {
        auto const token = readTokenFile(std::filesystem::temp_directory_path() / "contour-token-absent");
        REQUIRE_FALSE(token.has_value());
        CHECK(token.error().contains("cannot open token file"));
    }
}

TEST_CASE("resolveToken picks the spelling that was given", "[vthost][token]")
{
    SECTION("neither: an empty token, meaning no authentication")
    {
        auto const token = resolveToken("", "");
        REQUIRE(token.has_value());
        CHECK(token->empty());
    }

    SECTION("--token, verbatim")
    {
        CHECK(resolveToken("inline-secret", "").value() == "inline-secret");
    }

    SECTION("--token-file, read from disk")
    {
        auto const file = ScratchTokenFile { "resolve", "from-file\n" };
        CHECK(resolveToken("", file.path()).value() == "from-file");
    }
}

TEST_CASE("resolveToken refuses both spellings at once", "[vthost][token]")
{
    // Two different secrets is a bug in the invocation; picking a winner would hide it until an
    // authentication failure much later, somewhere unrelated.
    auto const file = ScratchTokenFile { "conflict", "from-file\n" };
    auto const token = resolveToken("inline-secret", file.path());
    REQUIRE_FALSE(token.has_value());
    CHECK(token.error().contains("not both"));
}

TEST_CASE("constantTimeEquals answers what == would", "[vthost][token]")
{
    // The timing property cannot be asserted portably; what a test CAN pin is that the
    // constant-time path did not change the ANSWER — a comparison that is secure and wrong is
    // worse than the fast one it replaced.
    auto const secret = std::string { "swordfish" };

    // The mismatches are DERIVED from the secret rather than spelled out, so each case says which
    // property it covers instead of leaving the reader to diff two similar literals.
    auto lastByteDiffers = secret;
    lastByteDiffers.back() = 'X';
    auto firstByteDiffers = secret;
    firstByteDiffers.front() = 'X';
    auto const oneByteShorter = secret.substr(0, secret.size() - 1);
    auto const oneByteLonger = secret + " ";

    CHECK(net::constantTimeEquals(secret, secret));
    CHECK(net::constantTimeEquals("", ""));
    CHECK_FALSE(net::constantTimeEquals(secret, lastByteDiffers));
    CHECK_FALSE(net::constantTimeEquals(secret, firstByteDiffers));
    CHECK_FALSE(net::constantTimeEquals(secret, oneByteShorter)); // a prefix is not a match
    CHECK_FALSE(net::constantTimeEquals(secret, oneByteLonger));  // nor is a longer string
    CHECK_FALSE(net::constantTimeEquals("", secret));
    // A zero byte inside the token is compared, not treated as a terminator.
    CHECK(net::constantTimeEquals(std::string_view { "a\0b", 3 }, std::string_view { "a\0b", 3 }));
    CHECK_FALSE(net::constantTimeEquals(std::string_view { "a\0b", 3 }, std::string_view { "a\0c", 3 }));
}
