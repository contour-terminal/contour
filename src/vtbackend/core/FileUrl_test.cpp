// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/FileUrl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using vtbackend::FileUrl;
using vtbackend::isLocalHost;
using vtbackend::isLoopbackHost;
using vtbackend::localPathOfFileUrl;
using vtbackend::parseFileUrl;

namespace
{

/// @return The path parseFileUrl() reads from @p url; empty when it refuses @p url.
[[nodiscard]] std::string pathOf(std::string url)
{
    return parseFileUrl(std::move(url))
        .transform([](FileUrl parsed) { return std::move(parsed.path); })
        .value_or(std::string {});
}

} // namespace

TEST_CASE("isLocalHost.thisMachine", "[fileurl]")
{
    // An absent authority (file:///tmp) is this machine by definition, and so is the loopback name.
    CHECK(isLocalHost("", "fedora"));
    CHECK(isLocalHost("localhost", "fedora"));
    CHECK(isLocalHost("LocalHost", "fedora"));
    CHECK(isLocalHost("localhost.localdomain", "fedora"));

    // The plain host name, however it is cased.
    CHECK(isLocalHost("fedora", "fedora"));
    CHECK(isLocalHost("FEDORA", "fedora"));
    CHECK(isLocalHost("Fedora", "FEDORA"));

    // Qualified on either side: applications build the URL from gethostname(2), which reports the short
    // name on one machine and the fully-qualified one on another -- issue #2057.
    CHECK(isLocalHost("fedora.corp.example", "fedora"));
    CHECK(isLocalHost("fedora", "fedora.corp.example"));
    CHECK(isLocalHost("fedora.corp.example", "fedora.corp.example"));
    CHECK(isLocalHost("FEDORA.CORP.EXAMPLE", "fedora.lan"));
}

TEST_CASE("isLocalHost.anotherMachine", "[fileurl]")
{
    CHECK_FALSE(isLocalHost("remotehost", "fedora"));
    CHECK_FALSE(isLocalHost("remotehost.corp.example", "fedora"));
    // A shared domain is not a shared machine.
    CHECK_FALSE(isLocalHost("other.corp.example", "fedora.corp.example"));
    // The first label must match in full: a prefix of it is a different machine.
    CHECK_FALSE(isLocalHost("fedora2", "fedora"));
    CHECK_FALSE(isLocalHost("fed", "fedora"));
    // "localhost" only counts as the whole first label, not as part of a longer name.
    CHECK_FALSE(isLocalHost("localhostess", "fedora"));
}

TEST_CASE("isLoopbackHost", "[fileurl]")
{
    CHECK(isLoopbackHost("localhost"));
    CHECK(isLoopbackHost("LocalHost"));
    CHECK(isLoopbackHost("localhost.localdomain"));

    // A machine's own name, and an absent one, are not the loopback name.
    CHECK_FALSE(isLoopbackHost("fedora"));
    CHECK_FALSE(isLoopbackHost(""));
    // The whole first label, as isLocalHost() reads it.
    CHECK_FALSE(isLoopbackHost("localhostess"));
    CHECK_FALSE(isLoopbackHost("my.localhost"));
}

TEST_CASE("parseFileUrl.NonFileUrl", "[fileurl]")
{
    CHECK(pathOf("https://example.com") == "https://example.com");
    CHECK(pathOf("ftp://server/file") == "ftp://server/file");
    CHECK(pathOf("").empty());
    CHECK(pathOf("/plain/path") == "/plain/path");
}

TEST_CASE("parseFileUrl.FileUrlWithLocalPath", "[fileurl]")
{
    CHECK(pathOf("file:///home/user/file.txt") == "/home/user/file.txt");
    CHECK(pathOf("file:///") == "/");
}

TEST_CASE("parseFileUrl.FileUrlWithHost", "[fileurl]")
{
    CHECK(pathOf("file://hostname/home/user/file.txt") == "/home/user/file.txt");
    CHECK(pathOf("file://hostname").empty());
}

TEST_CASE("parseFileUrl.PercentDecodesThePath", "[fileurl]")
{
    // Byte by byte, either hex case: how a shell sends a space, a '%', UTF-8, and the ESC and BEL that
    // would otherwise end OSC 7 early.
    CHECK(pathOf("file://host/tmp/a%20b") == "/tmp/a b");
    CHECK(pathOf("file:///tmp/50%2541") == "/tmp/50%41");
    CHECK(pathOf("file:///tmp/%C3%bc%E2%82%AC%F0%9F%98%80") == "/tmp/\xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80");
    CHECK(pathOf("file:///tmp/%1B%5D2%3Bx%07") == "/tmp/\x1b]2;x\x07");
    CHECK(pathOf("file:///C:/My%20Documents") == "C:/My Documents");
    CHECK(pathOf("file://host/C:/My%20Documents") == "C:/My Documents");
    // A malformed escape is kept as written.
    CHECK(pathOf("file:///tmp/a%zz%4") == "/tmp/a%zz%4");
    // A bare path is no URL, and nothing in it is an escape.
    CHECK(pathOf("/tmp/a%20b") == "/tmp/a%20b");
    // What a local working directory resolves to, too; another host's stays another host's.
    CHECK(localPathOfFileUrl("file://fedora/tmp/a%20b", "fedora") == "/tmp/a b");
    CHECK(localPathOfFileUrl("file://elsewhere/tmp/a%20b", "fedora") == std::nullopt);
}

TEST_CASE("parseFileUrl.AQueryOrFragmentIsNoPartOfThePath", "[fileurl]")
{
    // RFC 3986 §3: the path ends at '?' or '#'. Every reader -- the hover tooltip, the click, the working
    // directory -- reads it so, and a '?' or '#' that is part of a name travels escaped.
    CHECK(pathOf("file:///src/x.cpp#L10") == "/src/x.cpp");
    CHECK(pathOf("file://host/src/x.cpp?plain=1#L10") == "/src/x.cpp");
    CHECK(parseFileUrl("file://host#frag")->host == "host");
    CHECK(pathOf("file:///tmp/a%23b%3Fc") == "/tmp/a#b?c");
    CHECK(localPathOfFileUrl("file://fedora/src/x.cpp#L10", "fedora") == "/src/x.cpp");
    // The URL itself is kept as reported, and a bare path is no URL: its '#' is part of the name.
    CHECK(parseFileUrl("file:///src/x.cpp#L10")->url == "file:///src/x.cpp#L10");
    CHECK(pathOf("/tmp/a#b?c") == "/tmp/a#b?c");
}

TEST_CASE("parseFileUrl.APathHoldingNulIsNoPath", "[fileurl]")
{
    // Every filesystem call would read it cut short at the NUL, as a different directory.
    CHECK_FALSE(parseFileUrl("file:///srv%00/etc").has_value());
    CHECK_FALSE(parseFileUrl(std::string { "/srv\0/etc", 9 }).has_value());
    CHECK(localPathOfFileUrl("file://fedora/srv%00", "fedora") == std::nullopt);

    CHECK(parseFileUrl("file:///srv/etc").has_value());
    CHECK(pathOf("file:///srv%0/etc") == "/srv%0/etc"); // malformed, so kept as written
    CHECK(parseFileUrl("").has_value());
}

TEST_CASE("parseFileUrl.WindowsDriveLetter", "[fileurl]")
{
    // A Windows drive-letter authority must not be mistaken for a host and stripped.
    CHECK(pathOf("file://C:/Users/user/file.txt") == "C:/Users/user/file.txt");
    // The standards-conformant form has an empty authority and a leading slash before the drive.
    CHECK(pathOf("file:///C:/Users/user/file.txt") == "C:/Users/user/file.txt");
    // Lower-case drive letters are equally valid.
    CHECK(pathOf("file://d:/temp/x") == "d:/temp/x");
}

TEST_CASE("parseFileUrl.WindowsDriveLetterWithHost", "[fileurl]")
{
    // OSC 7 on Windows commonly reports a real hostname *and* a drive-letter path
    // (e.g. "file://MYPC/C:/Users/user"). The leading slash before the drive letter
    // must still be stripped, otherwise callers get an invalid "/C:/..." path that
    // Windows CreateProcess() rejects with ERROR_DIRECTORY ("directory name is invalid").
    CHECK(pathOf("file://hostname/C:/Users/user") == "C:/Users/user");
    CHECK(pathOf("file://MYPC/d:/temp/x") == "d:/temp/x");
}

TEST_CASE("localPathOfFileUrl.localHostIsOpenable", "[fileurl]")
{
    // The pane's own host: strip the scheme and authority down to a plain local path.
    CHECK(localPathOfFileUrl("file://fedora/home/user/proj", "fedora") == "/home/user/proj");
    // Case-insensitively, and tolerant of a fully-qualified name on either side (same machine).
    CHECK(localPathOfFileUrl("file://FEDORA/home/user", "fedora") == "/home/user");
    CHECK(localPathOfFileUrl("file://fedora.corp.example/home/user", "fedora") == "/home/user");
    CHECK(localPathOfFileUrl("file://fedora/home/user", "fedora.corp.example") == "/home/user");
    // An empty authority (file:///path) and an explicit "localhost" are this machine too.
    CHECK(localPathOfFileUrl("file:///home/user", "fedora") == "/home/user");
    CHECK(localPathOfFileUrl("file://localhost/home/user", "fedora") == "/home/user");
    // A bare path (a shell that emits OSC 7 without the file:// wrapper) is local as-is.
    CHECK(localPathOfFileUrl("/home/user", "fedora") == "/home/user");
}

TEST_CASE("localPathOfFileUrl.remoteHostIsRejected", "[fileurl]")
{
    // A different host is a remote (e.g. SSH) working directory: its path does not exist here.
    CHECK(localPathOfFileUrl("file://remotehost/home/user", "fedora") == std::nullopt);
    CHECK(localPathOfFileUrl("file://remotehost/C:/Users/user", "fedora") == std::nullopt);
    // A host with no path at all has nothing to open.
    CHECK(localPathOfFileUrl("file://fedora", "fedora") == std::nullopt);
    // Nothing reported yet.
    CHECK(localPathOfFileUrl("", "fedora") == std::nullopt);
}

TEST_CASE("localPathOfFileUrl.aDriveLetterPathIsLocalOnlyUnderTheWindowsConvention", "[fileurl]")
{
    // A drive-letter authority is a path, not a host. It names a file on this machine only where paths take
    // drive letters: under POSIX "C:/Users/user" is not even an absolute path, and opening it would open
    // some other file than the one named.
    using vtbackend::PathConvention;
    for (auto const* const url:
         { "file://C:/Users/user", "file:///C:/Users/user", "file://laptop/C:/Users/user" })
    {
        INFO("URL: " << url);
        CHECK(localPathOfFileUrl(url, "laptop", PathConvention::Windows) == "C:/Users/user");
        CHECK(localPathOfFileUrl(url, "laptop", PathConvention::Posix) == std::nullopt);
    }
    // A real host in front of a drive path is local only when the host matches.
    CHECK(localPathOfFileUrl("file://desktop/C:/Users/user", "laptop", PathConvention::Windows)
          == std::nullopt);
    // POSIX paths are paths under either convention.
    CHECK(localPathOfFileUrl("file:///home/user", "laptop", PathConvention::Windows) == "/home/user");
    CHECK(localPathOfFileUrl("file:///home/user", "laptop", PathConvention::Posix) == "/home/user");
}
