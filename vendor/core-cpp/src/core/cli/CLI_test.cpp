// SPDX-License-Identifier: Apache-2.0
#include <core/cli/CLI.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>

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
// - [ ] variations of option names and value attachments
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

using std::string;

namespace cli = core::cli;

using namespace std::string_view_literals;
using namespace std::string_literals;

TEST_CASE("CLI.option.type.bool")
{
    auto const cmd = cli::Command {
        .name = "contour",
        .helpText = "help here",
        .options = cli::OptionList { cli::Option {
            .name = "verbose"sv, .v = cli::Value { false }, .helpText = "Help text here"sv } },
    };

    SECTION("set")
    {
        auto const args = cli::StringViewList { "contour", "verbose" };
        auto const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value { true });
    }

    SECTION("set true")
    {
        auto const args = cli::StringViewList { "contour", "verbose", "true" };
        auto const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value { true });
    }

    SECTION("set true")
    {
        auto const args = cli::StringViewList { "contour", "verbose", "false" };
        auto const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value { false });
    }

    SECTION("unset")
    {
        auto const args = cli::StringViewList { "contour" };
        auto const flagsOpt = cli::parse(cmd, args);
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
    auto const flagsOpt = cli::parse(cmd, args);
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

namespace
{
cli::HelpDisplayStyle plainStyle()
{
    auto style = cli::HelpDisplayStyle {};
    style.colors.reset();
    style.hyperlink = false;
    return style;
}

cli::Command commandWithOptions()
{
    return cli::Command {
        .name = "contour",
        .helpText = "Terminal emulator.",
        .options =
            cli::OptionList {
                cli::Option { .name = "config"sv,
                              .v = cli::Value { "~/.config/contour/contour.yml"s },
                              .helpText = "Path to configuration file to load at startup."sv },
                cli::Option { .name = "profile"sv,
                              .v = cli::Value { ""s },
                              .helpText = "Overrides the profile to use in the configuration."sv },
            },
    };
}
} // namespace

// printOptions() sets the cursor to the option column and hands it to wordWrapped() as the
// starting position. `margin - cursor + 1` is unsigned: any margin at or below that column --
// a narrow terminal, or a pty that reports no size at all -- wrapped to about 4294967295, the
// `rightMargin <= 0` guard below it is dead for an unsigned type, and the index walked far off
// the end of the help text.
TEST_CASE("CLI.helpText.narrow-margin")
{
    auto const cmd = commandWithOptions();

    for (auto const margin: { 0u, 1u, 8u, 20u, 40u, 79u, 80u })
    {
        INFO("margin " << margin);
        auto const text = cli::helpText(cmd, plainStyle(), margin);
        CHECK(text.contains("config"));
        CHECK(text.contains("profile"));
    }
}

// wordWrapped() computed the position before the line feed as `linefeed - 1` on a size_t, so a
// help text whose first character is a line feed indexed text[SIZE_MAX].
TEST_CASE("CLI.helpText.leading-linefeed")
{
    auto const cmd = cli::Command {
        .name = "contour",
        .helpText = "Terminal emulator."sv,
        .options =
            cli::OptionList { cli::Option { .name = "config"sv,
                                            .v = cli::Value { ""s },
                                            .helpText = "\nIts help text starts on the next line."sv } },
    };

    auto const text = cli::helpText(cmd, plainStyle(), 80);
    CHECK(text.contains("Its help text starts on the next line."));
}

// The verbatim row's left column was measured against a column width computed from the options
// alone: `columnWidth - leftSize` underflowed, the assert above it is compiled out under NDEBUG,
// and spaces(n) became a string of about four billion characters.
TEST_CASE("CLI.helpText.verbatim-longer-than-the-options")
{
    auto const cmd = cli::Command {
        .name = "contour",
        .helpText = "Terminal emulator."sv,
        .verbatim = cli::Verbatim { "A_PLACEHOLDER_LONGER_THAN_ANY_OPTION", "Extra arguments." },
    };

    auto const text = cli::helpText(cmd, plainStyle(), 80);
    CHECK(text.contains("A_PLACEHOLDER_LONGER_THAN_ANY_OPTION"));
    CHECK(text.contains("Extra arguments."));
}

// The hyperlink scan walks back over the scheme with isalpha(), which is undefined for a char
// whose value is negative -- every continuation byte of a UTF-8 sequence, and help text is
// written by a human.
TEST_CASE("CLI.helpText.non-ascii-help-text")
{
    auto const cmd = cli::Command {
        .name = "contour",
        .helpText = "Terminal emulator."sv,
        .options = cli::OptionList { cli::Option {
            .name = "config"sv,
            .v = cli::Value { ""s },
            .helpText = "Grüße — siehe https://contour-terminal.org/ für mehr."sv } },
    };

    auto style = plainStyle();
    style.hyperlink = true;
    auto const text = cli::helpText(cmd, style, 80);
    CHECK(text.contains("https://contour-terminal.org/"));
}

// Every way a command line can be refused is a value -- core-cpp#13 -- and says which, and where.
TEST_CASE("CLI.parse.failure-modes")
{
    auto const cmd = cli::Command {
        .name = "contour",
        .helpText = "Terminal emulator."sv,
        .options =
            cli::OptionList {
                cli::Option { .name = "count"sv, .v = cli::Value { 0 }, .helpText = "A number."sv },
                cli::Option { .name = "size"sv, .v = cli::Value { 0U }, .helpText = "A size."sv },
                cli::Option { .name = "ratio"sv, .v = cli::Value { 0.0 }, .helpText = "A ratio."sv },
                cli::Option { .name = "verbose"sv, .v = cli::Value { false }, .helpText = "Chatty."sv },
                cli::Option { .name = "profile"sv,
                              .v = cli::Value { ""s },
                              .helpText = "Which profile."sv,
                              .placeholder = {},
                              .presence = cli::Presence::Required },
            },
    };

    /// Parses @p args, which must be refused, and returns why.
    auto const refusal = [&cmd](cli::StringViewList const& args) {
        auto result = cli::parse(cmd, args);
        REQUIRE_FALSE(result.has_value());
        return std::move(result).error();
    };

    SECTION("a value of the wrong type")
    {
        auto const error = refusal({ "contour", "profile", "p", "count", "not-a-number" });
        CHECK(error.kind == cli::ParseErrorKind::InvalidValue);
        CHECK(error.tokenIndex == 4);
        CHECK(error.message.contains("count"));
    }

    SECTION("a number with something after it")
    {
        // std::stoi read "12abc" as 12.
        auto const error = refusal({ "contour", "profile", "p", "count", "12abc" });
        CHECK(error.kind == cli::ParseErrorKind::InvalidValue);
        CHECK(error.tokenIndex == 4);
    }

    SECTION("a negative unsigned")
    {
        // std::stoul read "-1" as ULONG_MAX, and the cast to unsigned kept its low half.
        auto const error = refusal({ "contour", "profile", "p", "size", "-1" });
        CHECK(error.kind == cli::ParseErrorKind::InvalidValue);
    }

    SECTION("an integer out of range")
    {
        auto const error = refusal({ "contour", "profile", "p", "count", "99999999999999999999" });
        CHECK(error.kind == cli::ParseErrorKind::InvalidValue);
    }

    SECTION("a floating-point value with something after it")
    {
        auto const error = refusal({ "contour", "profile", "p", "ratio", "1.5x" });
        CHECK(error.kind == cli::ParseErrorKind::InvalidValue);
    }

    SECTION("an explicit empty value for an option that is not a string")
    {
        auto const error = refusal({ "contour", "profile", "p", "--count=" });
        CHECK(error.kind == cli::ParseErrorKind::EmptyValue);
        CHECK(error.tokenIndex == 3);
    }

    SECTION("an option whose value is missing")
    {
        auto const error = refusal({ "contour", "profile", "p", "count" });
        CHECK(error.kind == cli::ParseErrorKind::NotEnoughArguments);
        CHECK(error.tokenIndex == 4);
        CHECK(error.message.contains("count"));
    }

    SECTION("no command name at all")
    {
        auto const error = refusal({});
        CHECK(error.kind == cli::ParseErrorKind::NotEnoughArguments);
        CHECK(error.tokenIndex == 0);
    }

    SECTION("a token nothing takes")
    {
        auto const error = refusal({ "contour", "profile", "p", "bogus" });
        CHECK(error.kind == cli::ParseErrorKind::UnexpectedToken);
        CHECK(error.tokenIndex == 3);
        CHECK(error.message.contains("bogus"));
    }

    SECTION("a missing required option")
    {
        auto const error = refusal({ "contour", "count", "1" });
        CHECK(error.kind == cli::ParseErrorKind::MissingRequiredOption);
        CHECK(error.message.contains("contour.profile"));
    }

    SECTION("every type still reads a well-formed value")
    {
        auto const flags = cli::parse(
            cmd, { "contour", "profile", "p", "count", "-7", "size", "7", "ratio", "0.25", "verbose", "no" });
        REQUIRE(flags.has_value());
        CHECK(flags->get<int>("contour.count") == -7);
        CHECK(flags->get<unsigned>("contour.size") == 7U);
        CHECK(flags->get<double>("contour.ratio") == 0.25);
        CHECK_FALSE(flags->get<bool>("contour.verbose"));
        CHECK(flags->get<std::string>("contour.profile") == "p");
    }
}

// The chunk that ends at a line feed is trimmed of its trailing spaces, but the caller advanced
// its index by the chunk's LENGTH: the trimmed spaces stayed in front of it, the skip loop skips
// line feeds and not spaces, and so the same empty chunk came back for ever. Any help text with a
// space before a line feed hung the renderer -- and `--help` with it.
TEST_CASE("CLI.helpText.space-before-linefeed")
{
    auto const render = [](std::string_view helpText) {
        auto const cmd = cli::Command {
            .name = "contour",
            .helpText = "Terminal emulator."sv,
            .options = cli::OptionList { cli::Option {
                .name = "config"sv, .v = cli::Value { ""s }, .helpText = helpText } },
        };
        return cli::helpText(cmd, plainStyle(), 80);
    };

    SECTION("in the middle of the text")
    {
        auto const text = render("First line. \nSecond line."sv);
        CHECK(text.contains("First line."));
        CHECK(text.contains("Second line."));
    }

    SECTION("at the very start")
    {
        auto const text = render(" \nAfter a space and a line feed."sv);
        CHECK(text.contains("After a space and a line feed."));
    }

    SECTION("at the very end")
    {
        auto const text = render("Ends with a space before its line feed. \n"sv);
        CHECK(text.contains("Ends with a space before its line feed."));
    }

    SECTION("several in a row")
    {
        auto const text = render("One. \n \nTwo.   \n\nThree."sv);
        CHECK(text.contains("One."));
        CHECK(text.contains("Two."));
        CHECK(text.contains("Three."));
    }

    SECTION("nothing but spaces and line feeds")
    {
        // Nothing to emit, and the renderer still has to finish.
        CHECK(!render("  \n  \n \n"sv).empty());
    }
}
