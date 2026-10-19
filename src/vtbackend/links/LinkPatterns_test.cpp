// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkPatterns.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <string>
#include <vector>

using namespace vtbackend;

namespace
{
std::vector<std::string> namesOf(LinkPatterns const& table)
{
    auto names = std::vector<std::string> {};
    for (auto const& pattern: table.patterns())
        names.push_back(pattern.name);
    return names;
}
} // namespace

TEST_CASE("LinkPatterns.builtinsInTableOrder", "[links]")
{
    auto const table = LinkPatterns::builtins();
    CHECK(namesOf(*table) == std::vector<std::string> { "url", "filepath", "githash", "ipv4", "ipv6" });
    CHECK(table->find("url")->kind == LinkKind::Url);
    CHECK(table->find("url")->activation == LinkActivation::Clickable);
    CHECK(table->find("filepath")->kind == LinkKind::Path);
    CHECK(table->find("filepath")->activation == LinkActivation::Clickable);
    CHECK(table->find("githash")->activation == LinkActivation::HintOnly);
    CHECK(table->diagnostics().empty());
    CHECK(LinkPatterns::builtins() == table); // compiled once
}

TEST_CASE("LinkPatterns.userPatternIsAppendedAndHintOnly", "[links]")
{
    auto const entries =
        std::array { LinkPatternSpec { .name = "jira", .regex = R"(\b[A-Z]+-\d+\b)", .activation = {} } };
    auto const table = LinkPatterns::compile(entries);
    REQUIRE(table.find("jira") != nullptr);
    CHECK(table.find("jira")->kind == LinkKind::Custom);
    CHECK(table.find("jira")->activation == LinkActivation::HintOnly);
    CHECK(namesOf(table).back() == "jira");
}

TEST_CASE("LinkPatterns.overrideKeepsWhatItDoesNotGive", "[links]")
{
    SECTION("a regex only")
    {
        auto const entries =
            std::array { LinkPatternSpec { .name = "url", .regex = R"(https://\S+)", .activation = {} } };
        auto const table = LinkPatterns::compile(entries);
        CHECK(table.find("url")->source == R"(https://\S+)");
        CHECK(table.find("url")->kind == LinkKind::Url);
        CHECK(table.find("url")->activation == LinkActivation::Clickable);
    }
    SECTION("a name only")
    {
        auto const entries = std::array { LinkPatternSpec {
            .name = "filepath", .regex = {}, .activation = LinkActivation::HintOnly } };
        auto const table = LinkPatterns::compile(entries);
        CHECK(table.find("filepath")->activation == LinkActivation::HintOnly);
        CHECK(table.find("filepath")->source == LinkPatterns::builtins()->find("filepath")->source);
    }
}

TEST_CASE("LinkPatterns.reportsAndDisablesBrokenEntries", "[links]")
{
    auto const entries = std::array {
        LinkPatternSpec { .name = "broken", .regex = "([a-", .activation = {} },
        LinkPatternSpec { .name = "empty", .regex = R"(\b)", .activation = LinkActivation::Clickable },
        LinkPatternSpec { .name = "bytes", .regex = "[äö]+", .activation = {} },
        LinkPatternSpec { .name = "nobody", .regex = {}, .activation = LinkActivation::Clickable },
    };
    auto const table = LinkPatterns::compile(entries);
    CHECK(table.find("broken") == nullptr);
    CHECK(table.find("empty") == nullptr);
    CHECK(table.find("bytes") != nullptr); // a warning only
    CHECK(table.find("nobody") == nullptr);

    REQUIRE(table.diagnostics().size() == 4);
    CHECK(table.diagnostics()[0].problem == LinkPatternProblem::InvalidRegex);
    CHECK(table.diagnostics()[0].severity() == LinkPatternSeverity::Error);
    CHECK(table.diagnostics()[1].problem == LinkPatternProblem::MatchesEmpty);
    CHECK(table.diagnostics()[2].problem == LinkPatternProblem::NonAsciiInBracket);
    CHECK(table.diagnostics()[2].severity() == LinkPatternSeverity::Warning);
    CHECK(table.diagnostics()[3].problem == LinkPatternProblem::UnknownBuiltin);
    CHECK(table.diagnostics()[0].message().contains("broken"));
}

TEST_CASE("LinkPatterns.backReferenceIsRefusedAtLoad", "[links]")
{
    // Patterns run on an engine whose stack does not grow with the line, which has no back-references: the
    // pattern is refused when the table is compiled, on every platform alike, never when it matches.
    auto const entries = std::array {
        LinkPatternSpec { .name = "twice", .regex = R"((a)\1)", .activation = {} },
        LinkPatternSpec { .name = "escaped", .regex = R"(a\\1)", .activation = {} }, // a backslash, then 1
        LinkPatternSpec { .name = "inClass", .regex = R"(x[\]1])", .activation = {} },
    };
    auto const table = LinkPatterns::compile(entries);
    CHECK(table.find("twice") == nullptr);
    CHECK(table.find("escaped") != nullptr);
    CHECK(table.find("inClass") != nullptr);
    REQUIRE(table.diagnostics().size() == 1);
    CHECK(table.diagnostics()[0].pattern == "twice");
    CHECK(table.diagnostics()[0].problem == LinkPatternProblem::BackReference);
    CHECK(table.diagnostics()[0].severity() == LinkPatternSeverity::Error);
    CHECK(table.diagnostics()[0].message().contains("back-reference"));
}

TEST_CASE("LinkPatterns.templateNamingAMissingGroupDisablesThePattern", "[links]")
{
    auto const entries = std::array {
        LinkPatternSpec { .name = "jira",
                          .regex = R"(\b[A-Z]+-\d+\b)",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "https://j/{1}" },
        LinkPatternSpec { .name = "ok",
                          .regex = R"(\b([A-Z]+)-\d+\b)",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "https://j/{1}" },
    };
    auto const table = LinkPatterns::compile(entries);
    CHECK(table.find("jira") == nullptr);
    REQUIRE(table.find("ok") != nullptr);
    CHECK(table.find("ok")->urlTemplate.has_value());
    REQUIRE(table.diagnostics().size() == 1);
    CHECK(table.diagnostics()[0].pattern == "jira");
    CHECK(table.diagnostics()[0].problem == LinkPatternProblem::UnknownTemplateGroup);
    CHECK(table.diagnostics()[0].severity() == LinkPatternSeverity::Error);
    CHECK(table.diagnostics()[0].message().contains("https://j/{1}"));
}

TEST_CASE("LinkPatterns.aTemplateThatIsNoTemplateDisablesThePattern", "[links]")
{
    auto const entries = std::array {
        LinkPatternSpec { .name = "open",
                          .regex = R"(\b[A-Z]+-\d+\b)",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "https://j/{0" },
        LinkPatternSpec { .name = "named",
                          .regex = R"(\b[A-Z]+-\d+\b)",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "https://j/{id}" },
    };
    auto const table = LinkPatterns::compile(entries);
    CHECK(table.find("open") == nullptr);
    CHECK(table.find("named") == nullptr);
    REQUIRE(table.diagnostics().size() == 2);
    CHECK(table.diagnostics()[0].problem == LinkPatternProblem::InvalidUrlTemplate);
    CHECK(table.diagnostics()[1].problem == LinkPatternProblem::InvalidUrlTemplate);
    CHECK(table.diagnostics()[0].severity() == LinkPatternSeverity::Error);
}

TEST_CASE("LinkPatterns.builtinNamesAreKnownByName", "[links]")
{
    for (auto const name: builtinLinkPatternNames())
        CHECK(isBuiltinLinkPatternName(name));
    CHECK_FALSE(isBuiltinLinkPatternName("jira"));
    CHECK_FALSE(isBuiltinLinkPatternName(""));
    CHECK_FALSE(isBuiltinLinkPatternName("URL")); // names are matched exactly
}

TEST_CASE("LinkPatterns.aTemplateOnABuiltinIsIgnoredAndSaid", "[links]")
{
    // A built-in opens what it finds as it is: a url: given to one is left out, and the pattern still works.
    // Every built-in, by its name: githash, ipv4 and ipv6 find custom links, and take no template either.
    auto const name = std::string { GENERATE(from_range(builtinLinkPatternNames())) };
    CAPTURE(name);
    auto const entries = std::array { LinkPatternSpec {
        .name = name, .regex = {}, .activation = {}, .urlTemplate = "https://proxy/{0}" } };
    auto const table = LinkPatterns::compile(entries);
    REQUIRE(table.find(name) != nullptr);
    CHECK_FALSE(table.find(name)->urlTemplate.has_value());
    REQUIRE(table.diagnostics().size() == 1);
    CHECK(table.diagnostics()[0].problem == LinkPatternProblem::TemplateOnBuiltin);
    CHECK(table.diagnostics()[0].severity() == LinkPatternSeverity::Warning);
}

TEST_CASE("LinkPatterns.aPathPatternWithoutItsGroupIsDisabled", "[links]")
{
    // A path pattern names the path with capture group 1 (and its location with groups 2-5): without that
    // group it would find nothing, silently.
    auto const entries =
        std::array { LinkPatternSpec { .name = "filepath", .regex = R"(\S+\.cpp)", .activation = {} } };
    auto const table = LinkPatterns::compile(entries);
    CHECK(table.find("filepath") == nullptr);
    REQUIRE(table.diagnostics().size() == 1);
    CHECK(table.diagnostics()[0].problem == LinkPatternProblem::PathWithoutGroup);
    CHECK(table.diagnostics()[0].severity() == LinkPatternSeverity::Error);
    CHECK(table.diagnostics()[0].message().contains("group 1"));

    auto const grouped =
        std::array { LinkPatternSpec { .name = "filepath", .regex = R"((\S+\.cpp))", .activation = {} } };
    CHECK(LinkPatterns::compile(grouped).find("filepath") != nullptr);
}

TEST_CASE("LinkPatterns.aClickablePatternWhosePrintedTextChoosesTheSchemeIsWarnedAbout", "[links]")
{
    // §7.6: its match is a link only where the printed text names a scheme Contour opens. Said at load, so
    // that a pattern that finds no link is no mystery; a template that writes its scheme, and a hint-only
    // pattern, are not concerned.
    auto const entries = std::array {
        LinkPatternSpec { .name = "plain",
                          .regex = R"(\bTKT-\d+\b)",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = std::nullopt },
        LinkPatternSpec { .name = "printed",
                          .regex = R"(\bTKT-\d+\b)",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "{0}" },
        LinkPatternSpec { .name = "written",
                          .regex = R"(\bTKT-\d+\b)",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "https://t/{0}" },
        LinkPatternSpec { .name = "hinted",
                          .regex = R"(\bTKT-\d+\b)",
                          .activation = LinkActivation::HintOnly,
                          .urlTemplate = std::nullopt },
    };
    auto const table = LinkPatterns::compile(entries);
    REQUIRE(table.diagnostics().size() == 2);
    CHECK(table.diagnostics()[0].pattern == "plain");
    CHECK(table.diagnostics()[1].pattern == "printed");
    for (auto const& diagnostic: table.diagnostics())
    {
        CHECK(diagnostic.severity() == LinkPatternSeverity::Warning);
        CHECK(diagnostic.message().contains("scheme"));
        CHECK(diagnostic.message().contains("at the start of url")); // what a pattern of one's own can do
    }
    CHECK(table.find("plain") != nullptr); // a warning only
}

TEST_CASE("LinkPatterns.aClickableBuiltinWhoseMatchesNameNoSchemeSaysSo", "[links]")
{
    // A built-in takes no url, so advice to write one would give a second warning and still no link. Its
    // clickable has no effect: a pattern of one's own, with a url, links what it finds.
    auto const entries = std::array {
        LinkPatternSpec { .name = "githash", .regex = {}, .activation = LinkActivation::Clickable },
    };
    auto const table = LinkPatterns::compile(entries);
    REQUIRE(table.diagnostics().size() == 1);
    auto const& diagnostic = table.diagnostics()[0];
    CHECK(diagnostic.pattern == "githash");
    CHECK(diagnostic.severity() == LinkPatternSeverity::Warning);
    CHECK(diagnostic.message().contains("no effect"));
    CHECK(diagnostic.message().contains("pattern of your own"));
    CHECK_FALSE(diagnostic.message().contains("at the start of url"));
}

TEST_CASE("LinkPatterns.hintOnlyPatternMayMatchEmpty", "[links]")
{
    auto const entries =
        std::array { LinkPatternSpec { .name = "boundary", .regex = R"(\b)", .activation = {} } };
    CHECK(LinkPatterns::compile(entries).find("boundary") != nullptr);
}

namespace
{
/// @return The text of the first match of the built-in @p name in @p text, or an empty string.
std::string firstMatchOf(std::string_view name, std::string const& text)
{
    auto const table = LinkPatterns::builtins();
    auto match = std::smatch {};
    if (std::regex_search(text, match, table->find(name)->regex))
        return match.str(0);
    return {};
}
} // namespace

TEST_CASE("LinkPatterns.urlKeepsClosingBrackets", "[links]")
{
    // The URL ends at whitespace or a quote, not at `)`: a parenthesised URL is not truncated here;
    // trimming a trailing bracket is the detector's decision.
    CHECK(firstMatchOf("url", "see https://en.wikipedia.org/wiki/Foo_(bar) now")
          == "https://en.wikipedia.org/wiki/Foo_(bar)");
    CHECK(firstMatchOf("url", "write mailto:a@b.org.") == "mailto:a@b.org.");
    CHECK(firstMatchOf("url", R"(<a href="ftp://h/x">)") == "ftp://h/x");
}

TEST_CASE("LinkPatterns.filepathReadsLocationSuffix", "[links]")
{
    auto const table = LinkPatterns::builtins();
    auto match = std::smatch {};
    auto const text = std::string { " src/main.cpp:12:3 " };
    REQUIRE(std::regex_search(text, match, table->find("filepath")->regex));
    CHECK(match.str(1) == "src/main.cpp");
    CHECK(match.str(2) == "12");
    CHECK(match.str(3) == "3");

    auto const paren = std::string { "(a/b.rs(7,9))" };
    REQUIRE(std::regex_search(paren, match, table->find("filepath")->regex));
    CHECK(match.str(1) == "a/b.rs");
    CHECK(match.str(4) == "7");
    CHECK(match.str(5) == "9");
}

TEST_CASE("LinkPatterns.ipv6FullFormTakesNoColonAfterIt", "[links]")
{
    // Followed by `::`, the full form would match the first eight groups where a later alternative goes on
    // through the `::`: ECMAScript takes the first alternative, libstdc++'s breadth-first executor the
    // longest. Refusing the `:` leaves one answer for both engines.
    CHECK(firstMatchOf("ipv6", "1:2:3:4:5:6:7:8") == "1:2:3:4:5:6:7:8");
    CHECK(firstMatchOf("ipv6", "at 1:2:3:4:5:6:7:8, then") == "1:2:3:4:5:6:7:8");
    CHECK(firstMatchOf("ipv6", "1:2:3:4:5:6:7:8::9") == "1:2:3:4:5:6:7:8::9");
    CHECK(firstMatchOf("ipv6", "f:1:2:3:4:5:6:7:8:: x") == "f:1:2:3:4:5:6:7:8::");
}

TEST_CASE("LinkPatterns.filepathSurvivesALongWordRun", "[links]")
{
    // Only that it returns (the quadratic regex took 80 s on 64 KB): no timing is asserted.
    auto const run = std::string(std::size_t { 16 } * 1024, 'a');
    CHECK(firstMatchOf("filepath", run) == run);
    CHECK(firstMatchOf("filepath", run + "!") == run);
}
