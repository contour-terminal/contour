/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
    auto const cmd = cli::Command {
        "contour",
        "help here",
        cli::OptionList { cli::Option { "verbose"sv, cli::Value { false }, "Help text here"sv } },
    };

    SECTION("set")
    {
        auto const args = cli::StringViewList { "contour", "verbose" };
        optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value { true });
    }

    SECTION("set true")
    {
        auto const args = cli::StringViewList { "contour", "verbose", "true" };
        optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value { true });
    }

    SECTION("set true")
    {
        auto const args = cli::StringViewList { "contour", "verbose", "false" };
        optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value { false });
    }

    SECTION("unset")
    {
        auto const args = cli::StringViewList { "contour" };
        optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value { false });
    }
}

TEST_CASE("CLI.contour-full-test")
{
    auto const cmd = cli::Command {
        "contour",
        "help here",
        cli::OptionList {
            cli::Option { "debug"sv, cli::Value { ""s }, "Help text here"sv },
            cli::Option { "config", cli::Value { "~/.config/contour/contour.yml"s }, "Help text there"sv },
            cli::Option { "profile", cli::Value { ""s }, "Help text over here"sv } },
        cli::CommandList { cli::Command { "capture",
                                          "some capture help text",
                                          {
                                              cli::Option { "logical", cli::Value { false }, "help there" },
                                              cli::Option { "timeout", cli::Value { 1.0 }, "help here" },
                                              cli::Option { "output", cli::Value { ""s } },
                                          } } }
    };

    auto const args = cli::StringViewList { "contour", "capture", "logical", "output", "out.vt" };
    optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
    REQUIRE(flagsOpt.has_value());

    cli::FlagStore const& flags = flagsOpt.value();

    CHECK(flags.values.size() == 8);
    CHECK(flags.values.at("contour") == cli::Value { true }); // command
    CHECK(flags.values.at("contour.debug") == cli::Value { ""s });
    CHECK(flags.values.at("contour.config") == cli::Value { "~/.config/contour/contour.yml"s });
    CHECK(flags.values.at("contour.profile") == cli::Value { ""s });
    CHECK(flags.values.at("contour.capture") == cli::Value { true }); // command
    CHECK(flags.values.at("contour.capture.logical") == cli::Value { true });
    CHECK(flags.values.at("contour.capture.output") == cli::Value { "out.vt"s });
    CHECK(flags.values.at("contour.capture.timeout") == cli::Value { 1.0 });
}
