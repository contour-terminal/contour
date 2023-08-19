// SPDX-License-Identifier: Apache-2.0
#include <crispy/CLI.h>

#include <fmt/format.h>

#include <catch2/catch.hpp>

// TODO API / impl:
//
// - [ ] int-casts in cli.h are a nightmare. use size_t when applicable then.
// - [ ] Add ValueDef { Value defaultValue; std::string_view placeholder; } and use this where Value{} was
// used.
// - [x] option presence validation (optional, required)
// - [x] option variation parsing: posix
// - [x] usage output
// - [x] help output
// - [x] colorizing the output for usage and detailed help
// - [x] easy accessor for flag values
// - [x] line/word-wrapping; smart indentation at the beginning of the text scope
// - [x] help output: print default, if available (i.e. presence=optional)

// TODO tests:
//
// - [ ] variations of option names and value attachements
//       all of: NAME [VALUE] | --NAME [VALUE] | -NAME [VALUE] | --NAME[=VALUE]
// - [ ] help output printing (colored, non-colored)
// - [ ] help output auto-detecting screen width, via: VT seq, ioctl(TIOCGWINSZ), manual
// - [ ] presence optional vs presence required
// - [x] test option type: BOOL
// - [ ] test option type: INT
// - [ ] test option type: UINT
// - [ ] test option type: FLOAT (also being passed as INT positive / negative)
// - [ ] test option type: STR (can be any arbitrary string)
// - [ ] test option defaults
// - [ ] CONSIDER: supporting positional arguments (free sanding values of single given type)
// - [ ] test command chains up to 3 levels deep (including proper help output, maybe via /bin/ip emul?)
//

using std::optional;
using std::string;

namespace cli = crispy::cli;

using namespace std::string_view_literals;
using namespace std::string_literals;

TEST_CASE("CLI.option.type.bool")
{
    auto const cmd = cli::command {
        "contour",
        "help here",
        cli::option_list { cli::option { "verbose"sv, cli::value { false }, "Help text here"sv } },
    };

    SECTION("set")
    {
        auto const args = cli::string_view_list { "contour", "verbose" };
        optional<cli::flag_store> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::value { true });
    }

    SECTION("set true")
    {
        auto const args = cli::string_view_list { "contour", "verbose", "true" };
        optional<cli::flag_store> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::value { true });
    }

    SECTION("set true")
    {
        auto const args = cli::string_view_list { "contour", "verbose", "false" };
        optional<cli::flag_store> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::value { false });
    }

    SECTION("unset")
    {
        auto const args = cli::string_view_list { "contour" };
        optional<cli::flag_store> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::value { false });
    }
}

TEST_CASE("CLI.contour-full-test")
{
    auto const cmd = cli::command {
        "contour",
        "help here",
        cli::option_list {
            cli::option { "debug"sv, cli::value { ""s }, "Help text here"sv },
            cli::option { "config", cli::value { "~/.config/contour/contour.yml"s }, "Help text there"sv },
            cli::option { "profile", cli::value { ""s }, "Help text over here"sv } },
        cli::command_list { cli::command { "capture",
                                           "some capture help text",
                                           {
                                               cli::option { "logical", cli::value { false }, "help there" },
                                               cli::option { "timeout", cli::value { 1.0 }, "help here" },
                                               cli::option { "output", cli::value { ""s } },
                                           } } }
    };

    auto const args = cli::string_view_list { "contour", "capture", "logical", "output", "out.vt" };
    optional<cli::flag_store> const flagsOpt = cli::parse(cmd, args);
    REQUIRE(flagsOpt.has_value());

    cli::flag_store const& flags = flagsOpt.value();

    CHECK(flags.values.size() == 8);
    CHECK(flags.values.at("contour") == cli::value { true }); // command
    CHECK(flags.values.at("contour.debug") == cli::value { ""s });
    CHECK(flags.values.at("contour.config") == cli::value { "~/.config/contour/contour.yml"s });
    CHECK(flags.values.at("contour.profile") == cli::value { ""s });
    CHECK(flags.values.at("contour.capture") == cli::value { true }); // command
    CHECK(flags.values.at("contour.capture.logical") == cli::value { true });
    CHECK(flags.values.at("contour.capture.output") == cli::value { "out.vt"s });
    CHECK(flags.values.at("contour.capture.timeout") == cli::value { 1.0 });
}
