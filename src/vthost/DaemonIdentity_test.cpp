// SPDX-License-Identifier: Apache-2.0
#include <core/net/Tls.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vthost/DaemonIdentity.hpp>
#include <vthost/testing/RecordingFilePrivacy.hpp>

using vthost::PrivateEntry;
using vthost::testing::RecordedRestrict;

TEST_CASE("the daemon identity is made once, privately, and read back the same", "[vthost][daemon-identity]")
{
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto const privacy = vthost::testing::RecordingFilePrivacy { files };
    auto const file = std::filesystem::path { "/state/tls/default.pem" };

    auto const first = vthost::loadOrCreateDaemonIdentity(files, privacy, file);
    REQUIRE(first);
    CHECK(first->certPem.contains("BEGIN CERTIFICATE"));
    CHECK(first->keyPem.contains("PRIVATE KEY"));

    // Directory first, then the file while it is still empty: the key is never within anyone else's reach.
    auto const temporary = std::filesystem::path { "/state/tls/default.pem.tmp" };
    REQUIRE(privacy.calls.size() == 2);
    CHECK(privacy.calls[0] == RecordedRestrict { "/state/tls", PrivateEntry::Directory });
    CHECK(privacy.calls[1] == RecordedRestrict { temporary, PrivateEntry::File });
    CHECK(privacy.contentAtRestrict(temporary).empty());

    // It sits in the file, the leftover temporary gone, and the second call returns it unchanged and
    // restricts nothing more.
    auto const stored = files.readFile(file);
    REQUIRE(stored);
    CHECK(*stored == first->certPem + first->keyPem);
    CHECK_FALSE(files.exists(temporary));
    auto const second = vthost::loadOrCreateDaemonIdentity(files, privacy, file);
    REQUIRE(second);
    CHECK(second->certPem == first->certPem);
    CHECK(second->keyPem == first->keyPem);
    CHECK(privacy.calls.size() == 2);

    // A context can be built from it, and states the fingerprint the daemon does.
    auto const context = core::net::makeTlsServerContext(second->certPem, second->keyPem);
    REQUIRE(context);
    auto const stated = vthost::daemonFingerprint(*second);
    REQUIRE(stated);
    auto const& fingerprint = *stated;
    CHECK(fingerprint.starts_with("SHA256:"));
    CHECK_FALSE(fingerprint.contains('='));
    CHECK(fingerprint.size() == 7 + 43);
}

TEST_CASE("a file that is not a daemon identity is refused and left alone", "[vthost][daemon-identity]")
{
    auto const files = core::platform::testing::InMemoryFileSystem { { .path = "/state/tls/default.pem",
                                                                       .content = "not a certificate\n" } };
    auto const privacy = vthost::testing::RecordingFilePrivacy { files };
    auto const loaded = vthost::loadOrCreateDaemonIdentity(
        files, privacy, std::filesystem::path { "/state/tls/default.pem" });
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().contains("/state/tls/default.pem"));
    CHECK(files.readFile("/state/tls/default.pem").value() == "not a certificate\n");
    CHECK(privacy.calls.empty());
}

TEST_CASE("a refused restriction leaves no identity behind", "[vthost][daemon-identity]")
{
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = vthost::testing::RecordingFilePrivacy { files };
    privacy.failure = "no";
    auto const loaded = vthost::loadOrCreateDaemonIdentity(
        files, privacy, std::filesystem::path { "/state/tls/default.pem" });
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().contains("no"));
    CHECK_FALSE(files.exists("/state/tls/default.pem"));
}

TEST_CASE("the identity file is named by the label, which must be a plain name", "[vthost][daemon-identity]")
{
    CHECK(vthost::daemonIdentityPath("/state", "work") == std::filesystem::path { "/state/tls/work.pem" });
    CHECK_FALSE(vthost::daemonIdentityPath("/state", "").has_value());
    CHECK_FALSE(vthost::daemonIdentityPath("/state", "../x").has_value());
    CHECK_FALSE(vthost::daemonIdentityPath("/state", "a/b").has_value());
}

TEST_CASE("the fingerprint of an unreadable certificate is an error that says why",
          "[vthost][daemon-identity]")
{
    auto const fingerprint = vthost::daemonFingerprint({});
    REQUIRE_FALSE(fingerprint);
    CHECK_FALSE(fingerprint.error().empty());
}

TEST_CASE("an identity file the privacy check refuses is not used, nor replaced", "[vthost][daemon-identity]")
{
    auto const files = core::platform::testing::InMemoryFileSystem { { .path = "/state/tls/default.pem",
                                                                       .content = "planted\n" } };
    auto privacy = vthost::testing::RecordingFilePrivacy { files };
    privacy.refusal = "'/state/tls/default.pem' belongs to user 1001, not to this one (1000); it is not used";
    auto const loaded = vthost::loadOrCreateDaemonIdentity(
        files, privacy, std::filesystem::path { "/state/tls/default.pem" });
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().contains("belongs to user 1001"));
    CHECK(files.readFile("/state/tls/default.pem").value() == "planted\n");
    CHECK(privacy.calls.empty());
}
