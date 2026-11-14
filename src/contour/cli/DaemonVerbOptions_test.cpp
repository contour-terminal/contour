// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/DaemonVerbOptions.hpp>

#include <core/codec/Stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>
#include <vector>

using contour::cli::DaemonLaunch;
using contour::cli::DaemonOptionsError;
using contour::cli::DaemonOptionsVersion;
using contour::cli::DaemonVerbOptions;
using contour::cli::decodeDaemonLaunch;
using contour::cli::encodeDaemonLaunch;
using contour::cli::OptionFlag;

namespace
{
/// A record of the given version whose one frame holds @p fields.
std::vector<uint8_t> recordOf(uint32_t version, std::vector<uint8_t> const& fields)
{
    auto writer = core::codec::Writer {};
    writer.varint(version);
    writer.bytes(fields);
    return std::move(writer).take();
}

/// A launch of @p options on a socket of its own.
DaemonLaunch launchOf(DaemonVerbOptions options)
{
    return DaemonLaunch::restore(std::move(options), "/tmp/contour-test/launch.sock");
}

DaemonLaunch roundTripped(DaemonLaunch const& launch)
{
    auto const record = encodeDaemonLaunch(launch);
    auto const decoded = decodeDaemonLaunch(record);
    REQUIRE(decoded.has_value());
    return *decoded;
}

/// @return The options @p record holds.
DaemonVerbOptions givenIn(std::vector<uint8_t> const& record)
{
    auto const decoded = decodeDaemonLaunch(record);
    REQUIRE(decoded.has_value());
    return decoded->given();
}
} // namespace

// Adding a member to DaemonVerbOptions stops the header compiling: the structured binding in
// requireEveryDaemonOptionListed names every member. It is fixed by giving the member a row in
// DaemonVerbOptions::Fields, and a SECTION below.

TEST_CASE("every daemon option round-trips through the options record", "[cli][daemon]")
{
    auto options = DaemonVerbOptions {};

    SECTION("the defaults")
    {
    }
    SECTION("socket")
    {
        options.socket = "/run/user/1000/contour/x.sock";
    }
    SECTION("label")
    {
        options.label = "work";
    }
    SECTION("config")
    {
        options.config = "/etc/contour/contour.yml";
    }
    SECTION("profile")
    {
        options.profile = "daemon-profile";
    }
    SECTION("background")
    {
        options.background = OptionFlag::Given;
    }
    SECTION("stdio")
    {
        options.stdio = OptionFlag::Given;
    }
    SECTION("in-place")
    {
        options.inPlace = OptionFlag::Given;
    }
    SECTION("handover")
    {
        options.handover = "fd=7";
    }
    SECTION("lifecycle")
    {
        options.lifecycle = "exit-when-idle";
    }
    SECTION("exit-with-last-session")
    {
        options.exitWithLastSession = OptionFlag::Given;
    }
    SECTION("size-policy")
    {
        options.sizePolicy = "smallest";
    }
    SECTION("tmux-compat-socket")
    {
        options.tmuxCompatSocket = "tmuxlabel";
    }
    SECTION("listen-tcp")
    {
        options.listenTcp = "127.0.0.1:9090";
    }
    SECTION("token")
    {
        options.token = "s3cret";
    }
    SECTION("token-file")
    {
        options.tokenFile = "/home/u/token";
    }
    SECTION("access-file")
    {
        options.accessFile = "/home/u/access";
    }
    SECTION("tls-cert")
    {
        options.tlsCert = "/home/u/cert.pem";
    }
    SECTION("tls-key")
    {
        options.tlsKey = "/home/u/key.pem";
    }
    SECTION("log")
    {
        options.log = "vthost.*,vtbackend";
    }
    SECTION("log-file")
    {
        options.logFile = "-";
    }
    SECTION("authorized-keys")
    {
        options.authorizedKeys = "/home/u/authorized_keys";
    }
    SECTION("auth")
    {
        options.auth = "key";
    }
    SECTION("an empty value in a non-empty record")
    {
        options.socket = "x";
        options.lifecycle = "";
    }

    CHECK(roundTripped(launchOf(options)) == launchOf(options));
}

TEST_CASE("the resolved socket round-trips through the options record", "[cli][daemon]")
{
    auto const launch = DaemonLaunch::restore(DaemonVerbOptions {}, "/tmp/contour/\xC3\xA4.sock");
    CHECK(roundTripped(launch).socketPath() == launch.socketPath());
}

TEST_CASE("every daemon option round-trips through the options record, all at once", "[cli][daemon]")
{
    auto const options = DaemonVerbOptions {
        .socket = "a",
        .label = "b",
        .config = "c",
        .profile = "d",
        .background = OptionFlag::Given,
        .stdio = OptionFlag::Given,
        .inPlace = OptionFlag::Given,
        .handover = "e",
        .lifecycle = "exit-with-last-session",
        .exitWithLastSession = OptionFlag::Given,
        .sizePolicy = "f",
        .tmuxCompatSocket = "g",
        .listenTcp = "h",
        .token = "i",
        .tokenFile = "j",
        .accessFile = "k",
        .tlsCert = "l",
        .tlsKey = "m",
        .log = "n",
        .logFile = "o",
        .authorizedKeys = "p",
        .auth = "q",
    };
    CHECK(roundTripped(launchOf(options)) == launchOf(options));
}

TEST_CASE("a newer or truncated or malformed options record is refused", "[cli][daemon]")
{
    auto record = encodeDaemonLaunch(launchOf(DaemonVerbOptions {}));

    SECTION("a version above the reader's")
    {
        auto const newer = recordOf(DaemonOptionsVersion + 1, {});
        auto const decoded = decodeDaemonLaunch(newer);
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == DaemonOptionsError::NewerVersion);
    }
    SECTION("the version this release writes is read")
    {
        CHECK(decodeDaemonLaunch(record).has_value());
    }
    SECTION("a truncated record")
    {
        auto const empty = decodeDaemonLaunch({});
        REQUIRE(!empty.has_value());
        CHECK(empty.error() == DaemonOptionsError::Truncated);

        record.pop_back();
        auto const cut = decodeDaemonLaunch(record);
        REQUIRE(!cut.has_value());
        CHECK(cut.error() == DaemonOptionsError::Truncated);
    }
    SECTION("a value out of range")
    {
        auto fields = core::codec::Writer {};
        fields.varint((6U << 1U) | 0U); // stdio, a varint
        fields.varint(9);               // no such OptionFlag
        auto const decoded = decodeDaemonLaunch(recordOf(1, std::move(fields).take()));
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == DaemonOptionsError::Malformed);
    }
    SECTION("bytes after the record")
    {
        record.push_back(0);
        auto const decoded = decodeDaemonLaunch(record);
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == DaemonOptionsError::Malformed);
    }
}

TEST_CASE("an options record decodes across additions", "[cli][daemon]")
{
    SECTION("a record that lacks later fields decodes them with their defaults")
    {
        auto fields = core::codec::Writer {};
        fields.varint((1U << 1U) | 1U); // socket, framed
        fields.string("only-this");
        auto const decoded = givenIn(recordOf(1, std::move(fields).take()));
        CHECK(decoded.socket == "only-this");
        auto expected = DaemonVerbOptions {};
        expected.socket = "only-this";
        CHECK(decoded == expected);
        CHECK(decoded.lifecycle == "persistent");
    }
    SECTION("a field a later release added is skipped")
    {
        auto fields = core::codec::Writer {};
        fields.varint((99U << 1U) | 0U); // an id this release does not know, a varint
        fields.varint(5);
        fields.varint((2U << 1U) | 1U); // label, framed
        fields.string("kept");
        auto const decoded = givenIn(recordOf(1, std::move(fields).take()));
        CHECK(decoded.label == "kept");
    }
}

TEST_CASE("the socket path resolves from the configuration when no option names it", "[cli][daemon]")
{
    SECTION("the configuration's socket")
    {
        CHECK(DaemonLaunch::resolve(DaemonVerbOptions {}, "/tmp/contour-test/configured.sock").socketPath()
              == "/tmp/contour-test/configured.sock");
    }
    SECTION("--socket beats the configuration")
    {
        auto options = DaemonVerbOptions {};
        options.socket = "/tmp/contour-test/option.sock";
        CHECK(DaemonLaunch::resolve(options, "/tmp/contour-test/configured.sock").socketPath()
              == "/tmp/contour-test/option.sock");
    }
    SECTION("--label alone names a path of its own")
    {
        auto options = DaemonVerbOptions {};
        options.label = "workbench";
        CHECK(DaemonLaunch::resolve(options, "").socketPath().filename() == "workbench");
    }
}

TEST_CASE("the daemon's options are read from the parsed command line", "[cli][daemon]")
{
    auto parameters = core::cli::FlagStore {};
    for (auto const* name: { "socket",
                             "label",
                             "config",
                             "profile",
                             "handover",
                             "lifecycle",
                             "size-policy",
                             "tmux-compat-socket",
                             "listen-tcp",
                             "token",
                             "token-file",
                             "access-file",
                             "tls-cert",
                             "tls-key",
                             "log",
                             "log-file",
                             "authorized-keys",
                             "auth" })
        parameters.values[std::string { "contour.daemon." } + name] = std::string {};
    for (auto const* name: { "background", "stdio", "in-place", "exit-with-last-session" })
        parameters.values[std::string { "contour.daemon." } + name] = false;
    parameters.values["contour.daemon.label"] = std::string { "lab" };
    parameters.values["contour.daemon.lifecycle"] = std::string { "persistent" };
    parameters.values["contour.daemon.in-place"] = true;

    auto const options = contour::cli::readDaemonVerbOptions(parameters);
    auto expected = DaemonVerbOptions {};
    expected.label = "lab";
    expected.inPlace = OptionFlag::Given;
    CHECK(options == expected);
}

TEST_CASE("a relaunched daemon repeats no option meant for one process", "[cli][daemon]")
{
    auto given = DaemonVerbOptions {};
    given.label = "work";
    given.token = "secret";
    given.handover = "fd=9";
    given.background = OptionFlag::Given;
    given.stdio = OptionFlag::Given;
    given.inPlace = OptionFlag::Given;
    auto const launch = DaemonLaunch::restore(given, "/tmp/contour-test/relaunch.sock");

    auto const relaunched = launch.relaunched();
    CHECK(relaunched.given().handover.empty());
    CHECK(relaunched.given().background == OptionFlag::Absent);
    CHECK(relaunched.given().stdio == OptionFlag::Absent);
    // Everything else is the launch's, unchanged.
    auto expected = given;
    expected.handover.clear();
    expected.background = OptionFlag::Absent;
    expected.stdio = OptionFlag::Absent;
    CHECK(relaunched.given() == expected);
    CHECK(relaunched.socketPath() == launch.socketPath());
    // The launch itself is left as it was.
    CHECK(launch.given() == given);
}
