// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/FileUrl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using vtbackend::extractPathFromFileUrl;
using vtbackend::isLocalHost;
using vtbackend::localWorkingDirectory;

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

TEST_CASE("extractPathFromFileUrl.NonFileUrl", "[fileurl]")
{
    CHECK(extractPathFromFileUrl("https://example.com") == "https://example.com");
    CHECK(extractPathFromFileUrl("ftp://server/file") == "ftp://server/file");
    CHECK(extractPathFromFileUrl("").empty());
    CHECK(extractPathFromFileUrl("/plain/path") == "/plain/path");
}

TEST_CASE("extractPathFromFileUrl.FileUrlWithLocalPath", "[fileurl]")
{
    CHECK(extractPathFromFileUrl("file:///home/user/file.txt") == "/home/user/file.txt");
    CHECK(extractPathFromFileUrl("file:///") == "/");
}

TEST_CASE("extractPathFromFileUrl.FileUrlWithHost", "[fileurl]")
{
    CHECK(extractPathFromFileUrl("file://hostname/home/user/file.txt") == "/home/user/file.txt");
    CHECK(extractPathFromFileUrl("file://hostname").empty());
}

TEST_CASE("extractPathFromFileUrl.WindowsDriveLetter", "[fileurl]")
{
    // A Windows drive-letter authority must not be mistaken for a host and stripped.
    CHECK(extractPathFromFileUrl("file://C:/Users/user/file.txt") == "C:/Users/user/file.txt");
    // The standards-conformant form has an empty authority and a leading slash before the drive.
    CHECK(extractPathFromFileUrl("file:///C:/Users/user/file.txt") == "C:/Users/user/file.txt");
    // Lower-case drive letters are equally valid.
    CHECK(extractPathFromFileUrl("file://d:/temp/x") == "d:/temp/x");
}

TEST_CASE("extractPathFromFileUrl.WindowsDriveLetterWithHost", "[fileurl]")
{
    // OSC 7 on Windows commonly reports a real hostname *and* a drive-letter path
    // (e.g. "file://MYPC/C:/Users/user"). The leading slash before the drive letter
    // must still be stripped, otherwise callers get an invalid "/C:/..." path that
    // Windows CreateProcess() rejects with ERROR_DIRECTORY ("directory name is invalid").
    CHECK(extractPathFromFileUrl("file://hostname/C:/Users/user") == "C:/Users/user");
    CHECK(extractPathFromFileUrl("file://MYPC/d:/temp/x") == "d:/temp/x");
}

TEST_CASE("localWorkingDirectory.localHostIsOpenable", "[fileurl]")
{
    // The pane's own host: strip the scheme and authority down to a plain local path.
    CHECK(localWorkingDirectory("file://fedora/home/user/proj", "fedora") == "/home/user/proj");
    // Case-insensitively, and tolerant of a fully-qualified name on either side (same machine).
    CHECK(localWorkingDirectory("file://FEDORA/home/user", "fedora") == "/home/user");
    CHECK(localWorkingDirectory("file://fedora.corp.example/home/user", "fedora") == "/home/user");
    CHECK(localWorkingDirectory("file://fedora/home/user", "fedora.corp.example") == "/home/user");
    // An empty authority (file:///path) and an explicit "localhost" are this machine too.
    CHECK(localWorkingDirectory("file:///home/user", "fedora") == "/home/user");
    CHECK(localWorkingDirectory("file://localhost/home/user", "fedora") == "/home/user");
    // A bare path (a shell that emits OSC 7 without the file:// wrapper) is local as-is.
    CHECK(localWorkingDirectory("/home/user", "fedora") == "/home/user");
}

TEST_CASE("localWorkingDirectory.remoteHostIsRejected", "[fileurl]")
{
    // A different host is a remote (e.g. SSH) working directory: its path does not exist here.
    CHECK(localWorkingDirectory("file://remotehost/home/user", "fedora") == std::nullopt);
    CHECK(localWorkingDirectory("file://remotehost/C:/Users/user", "fedora") == std::nullopt);
    // A host with no path at all has nothing to open.
    CHECK(localWorkingDirectory("file://fedora", "fedora") == std::nullopt);
    // Nothing reported yet.
    CHECK(localWorkingDirectory("", "fedora") == std::nullopt);
}

TEST_CASE("localWorkingDirectory.windowsDriveLetterIsLocal", "[fileurl]")
{
    // A drive-letter authority is a path, not a host, so it is this machine's.
    CHECK(localWorkingDirectory("file://C:/Users/user", "laptop") == "C:/Users/user");
    CHECK(localWorkingDirectory("file:///C:/Users/user", "laptop") == "C:/Users/user");
    // A real host in front of a drive path is still local only when the host matches.
    CHECK(localWorkingDirectory("file://laptop/C:/Users/user", "laptop") == "C:/Users/user");
    CHECK(localWorkingDirectory("file://desktop/C:/Users/user", "laptop") == std::nullopt);
}
