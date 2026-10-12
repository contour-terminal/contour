// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/Functions.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{
// purely for proper diagnostic printing in Catch2
static inline std::ostream& operator<<(std::ostream& os, Function const& f)
{
    return os << std::format("{}", f);
}
} // namespace vtbackend

using namespace std;
using namespace vtbackend;

TEST_CASE("Functions.SCOSC", "[Functions]")
{
    SupportedSequences availableSequences;
    // The problem with SCOSC vs DECSLRM is, that the former is a subset of the latter
    // when no arguments are given.
    availableSequences.disableSequence(DECSLRM);
    Function const* f = vtbackend::selectControl(0, 0, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == SCOSC);
}

TEST_CASE("Functions.SETMARK.removed", "[Functions]")
{
    // CSI > M was a Contour-only extension for marking a prompt line, deprecated in favour of
    // OSC 133;A and removed in 0.7.1. It must no longer resolve to anything -- a leftover row here
    // would keep it working while the docs and the shipped shell integration say it is gone.
    SupportedSequences const availableSequences;
    CHECK(vtbackend::selectControl('>', 0, 0, 'M', availableSequences.activeSequences()) == nullptr);

    // DL is `CSI ... M` with NO leader and must be unaffected by the removal.
    Function const* dl = vtbackend::selectControl(0, 0, 0, 'M', availableSequences.activeSequences());
    REQUIRE(dl);
    CHECK(*dl == DL);
}

TEST_CASE("Functions.DECSLRM", "[Functions]")
{
    // Maybe it is okay to not care about 0 and 1 arguments? Who's doing that?
    SupportedSequences const availableSequences;
    Function const* f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.OSC1", "[Functions]")
{
    SupportedSequences const availableSequences;
    Function const* osc = vtbackend::selectOSCommand(1, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == SETICON);
}

TEST_CASE("Functions.OSC2", "[Functions]")
{
    SupportedSequences const availableSequences;
    Function const* osc = vtbackend::selectOSCommand(2, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == SETWINTITLE);
}

TEST_CASE("Functions.OSC8", "[Functions]")
{
    SupportedSequences const availableSequences;
    Function const* osc = vtbackend::selectOSCommand(8, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == HYPERLINK);
}

TEST_CASE("Functions.OSC777", "[Functions]")
{
    SupportedSequences const availableSequences;
    Function const* osc = vtbackend::selectOSCommand(777, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == NOTIFY);
}

TEST_CASE("Functions.VTLevelConstrain", "[Functions]")
{
    SupportedSequences availableSequences;
    availableSequences.reset(VTType::VT100);
    Function const* f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(!f);
    availableSequences.reset(VTType::VT420);
    f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.DCS_GIP", "[Functions]")
{
    // GIP is DCS with intermediate='!' and final='g' (0 args).
    // This tests the binary search leftward-scan fix for ambiguous DCS selectors.
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::DCS,
                                        .leader = 0,
                                        .argc = 0,
                                        .intermediate = '!',
                                        .finalSymbol = 'g' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == GIP);
}

TEST_CASE("Functions.DOCS", "[Functions]")
{
    // ESC % @ (ISO 8859-1 default) and ESC % G (UTF-8) are the Designate Other Coding System sequences
    // vttest emits at startup. Contour is UTF-8-native and accepts both as no-ops, but they must resolve
    // to a registered function so they are not logged as "Unknown VT sequence".
    SupportedSequences const availableSequences;
    auto const* def = vtbackend::selectEscape('%', '@', availableSequences.activeSequences());
    REQUIRE(def);
    CHECK(*def == DOCS_DEFAULT);
    auto const* utf8 = vtbackend::selectEscape('%', 'G', availableSequences.activeSequences());
    REQUIRE(utf8);
    CHECK(*utf8 == DOCS_UTF8);
}

TEST_CASE("Functions.EnableAndDisable", "[Functions]")
{
    SupportedSequences availableSequences;
    availableSequences.disableSequence(DECSLRM);
    Function const* f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(!f);
    availableSequences.enableSequence(DECSLRM);
    f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.CSI_DECAC", "[Functions]")
{
    // DECAC is CSI with intermediate=',' and final='|' (item ; fg ; bg).
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::CSI,
                                        .leader = 0,
                                        .argc = 3,
                                        .intermediate = ',',
                                        .finalSymbol = '|' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECAC);
}

TEST_CASE("Functions.CSI_DECATC", "[Functions]")
{
    // DECATC is CSI with intermediate=',' and final='}' (attribute ; fg ; bg).
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::CSI,
                                        .leader = 0,
                                        .argc = 3,
                                        .intermediate = ',',
                                        .finalSymbol = '}' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECATC);
}

TEST_CASE("Functions.DECAC_VTLevelConstrain", "[Functions]")
{
    // DECAC/DECATC are VT525; they must not resolve below that conformance level.
    SupportedSequences availableSequences;
    auto const selector = FunctionSelector {
        .category = FunctionCategory::CSI, .leader = 0, .argc = 3, .intermediate = ',', .finalSymbol = '|'
    };
    availableSequences.reset(VTType::VT510);
    REQUIRE(!vtbackend::select(selector, availableSequences.activeSequences()));
    availableSequences.reset(VTType::VT525);
    auto const* f = vtbackend::select(selector, availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECAC);
}

TEST_CASE("Functions.DECATC_VTLevelConstrain", "[Functions]")
{
    // DECATC is VT525; it must not resolve below that conformance level.
    SupportedSequences availableSequences;
    auto const selector = FunctionSelector {
        .category = FunctionCategory::CSI, .leader = 0, .argc = 3, .intermediate = ',', .finalSymbol = '}'
    };
    availableSequences.reset(VTType::VT510);
    REQUIRE(!vtbackend::select(selector, availableSequences.activeSequences()));
    availableSequences.reset(VTType::VT525);
    auto const* f = vtbackend::select(selector, availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECATC);
}

TEST_CASE("Functions.CSI_DECATC_ZeroParameterForm", "[Functions]")
{
    // DECATC must resolve with no parameters at all: a lone '0' attribute collapses to zero parameters
    // under the VT convention, and `CSI 0 , }` is the only spelling of "reset the normal-text entry".
    // Registering DECATC with minArgs=1 would drop that sequence before it ever reaches the handler.
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::CSI,
                                        .leader = 0,
                                        .argc = 0,
                                        .intermediate = ',',
                                        .finalSymbol = '}' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECATC);
}

TEST_CASE("Functions.CSI_DECSTGLT", "[Functions]")
{
    // DECSTGLT is CSI with intermediate=')' and final='{' (VT525).
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::CSI,
                                        .leader = 0,
                                        .argc = 1,
                                        .intermediate = ')',
                                        .finalSymbol = '{' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSTGLT);
}

TEST_CASE("Functions.DECSTGLT_VTLevelConstrain", "[Functions]")
{
    // DECSTGLT is VT525; it must not resolve below that conformance level.
    SupportedSequences availableSequences;
    auto const selector = FunctionSelector {
        .category = FunctionCategory::CSI, .leader = 0, .argc = 1, .intermediate = ')', .finalSymbol = '{'
    };
    availableSequences.reset(VTType::VT510);
    REQUIRE(!vtbackend::select(selector, availableSequences.activeSequences()));
    availableSequences.reset(VTType::VT525);
    auto const* f = vtbackend::select(selector, availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSTGLT);
}

namespace
{
/// Every selector that names a known definition: each one's own category, leader, intermediate and
/// final byte, with every argument count from 0 to one past its maximum.
std::vector<FunctionSelector> selectorsForEveryDefinition(SupportedSequences const& sequences)
{
    auto selectors = std::vector<FunctionSelector> {};
    for (Function const& definition: sequences.allSequences())
        for (auto const argc: std::views::iota(0, static_cast<int>(definition.maximumParameters) + 2))
            selectors.push_back({ .category = definition.category,
                                  .leader = definition.leader,
                                  .argc = argc,
                                  .intermediate = definition.intermediate,
                                  .finalSymbol = definition.finalSymbol });
    return selectors;
}

/// @p count selectors drawn from everything a sequence could name, including final bytes at 0x80 and
/// above, which no definition uses.
std::vector<FunctionSelector> randomSelectors(uint32_t seed, size_t count)
{
    static constexpr auto Categories =
        std::array { FunctionCategory::C0,  FunctionCategory::ESC, FunctionCategory::CSI,
                     FunctionCategory::OSC, FunctionCategory::DCS, FunctionCategory::VT52 };
    static constexpr auto Leaders = std::string_view { "\0<=>?", 5 };
    static constexpr auto Intermediates = std::string_view { "\0 !\"#$%&'()*+,-./", 17 };

    auto rng = std::mt19937 { seed };
    auto const below = [&](size_t n) {
        return static_cast<size_t>(rng() % n);
    };
    auto selectors = std::vector<FunctionSelector> {};
    for ([[maybe_unused]] auto const _: std::views::iota(size_t { 0 }, count))
    {
        auto const category = Categories[below(Categories.size())];
        selectors.push_back({ .category = category,
                              .leader = Leaders[below(Leaders.size())],
                              .argc = static_cast<int>(below(category == FunctionCategory::OSC ? 6000 : 140)),
                              .intermediate = Intermediates[below(Intermediates.size())],
                              .finalSymbol = static_cast<char>(below(0x100)) });
    }
    return selectors;
}

/// Where the index and select() over the whole active table disagree: the count and the first ten,
/// or an empty string when they agree on every selector.
std::string disagreements(SupportedSequences const& sequences, std::vector<FunctionSelector> const& selectors)
{
    auto const nameOf = [](Function const* f) {
        return f ? std::string { f->documentation.mnemonic } : std::string { "none" };
    };
    auto lines = std::vector<std::string> {};
    for (auto const& selector: selectors)
    {
        auto const* const expected = vtbackend::select(selector, sequences.activeSequences());
        auto const* const actual = sequences.select(selector);
        if (expected != actual)
            lines.push_back(std::format("category {} leader {:02X} intermediate {:02X} final {:02X} argc {}: "
                                        "full table {}, index {}",
                                        static_cast<int>(selector.category),
                                        static_cast<unsigned char>(selector.leader),
                                        static_cast<unsigned char>(selector.intermediate),
                                        static_cast<unsigned char>(selector.finalSymbol),
                                        selector.argc,
                                        nameOf(expected),
                                        nameOf(actual)));
    }
    if (lines.empty())
        return {};
    auto report = std::format("{} disagreements, the first of them:\n", lines.size());
    for (auto const& line: lines | std::views::take(10))
        report += line + '\n';
    return report;
}
} // namespace

TEST_CASE("Functions.SupportedSequences.Select", "[Functions]")
{
    auto const sequences = SupportedSequences {};
    auto const* const sgr = sequences.select(
        { .category = FunctionCategory::CSI, .leader = 0, .argc = 1, .intermediate = 0, .finalSymbol = 'm' });
    REQUIRE(sgr);
    CHECK(*sgr == SGR);

    auto const* const hyperlink = sequences.select(
        { .category = FunctionCategory::OSC, .leader = 0, .argc = 8, .intermediate = 0, .finalSymbol = 0 });
    REQUIRE(hyperlink);
    CHECK(*hyperlink == HYPERLINK);

    // No definition ends in DEL, none in a byte at 0x80 or above.
    CHECK(sequences.select({ .category = FunctionCategory::CSI,
                             .leader = 0,
                             .argc = 0,
                             .intermediate = 0,
                             .finalSymbol = '\x7F' })
          == nullptr);
    CHECK(sequences.select({ .category = FunctionCategory::CSI,
                             .leader = 0,
                             .argc = 0,
                             .intermediate = 0,
                             .finalSymbol = static_cast<char>(0x80) })
          == nullptr);
}

TEST_CASE("Functions.EveryFinalByteIsSevenBit", "[Functions]")
{
    // The index has one bucket per final byte below 0x80. A definition outside that range would need a
    // bigger index, not a silently aliased bucket.
    auto const sequences = SupportedSequences {};
    for (Function const& definition: sequences.allSequences())
        CHECK(static_cast<unsigned char>(definition.finalSymbol) < 0x80);
}

TEST_CASE("Functions.SupportedSequences.IndexAgreesWithFullTableSelect", "[Functions]")
{
    auto sequences = SupportedSequences {};
    auto const everyDefinition = selectorsForEveryDefinition(sequences);
    auto const random = randomSelectors(20261012, 20000);
    auto const check = [&](std::string_view activeSet) {
        auto const forDefinitions = disagreements(sequences, everyDefinition);
        auto const forRandom = disagreements(sequences, random);
        INFO(std::format("active set: {}\n{}{}", activeSet, forDefinitions, forRandom));
        CHECK(forDefinitions.empty());
        CHECK(forRandom.empty());
    };

    check("as constructed");
    for (auto const level: { VTType::VT100,
                             VTType::VT220,
                             VTType::VT240,
                             VTType::VT320,
                             VTType::VT330,
                             VTType::VT340,
                             VTType::VT420,
                             VTType::VT510,
                             VTType::VT520,
                             VTType::VT525 })
    {
        sequences.reset(level);
        check(std::format("reset to level {}", static_cast<int>(level)));
    }
    // DECLRMM enables one of DECSLRM and SCOSC, then disables the other. Checking in between as well holds
    // enableSequence() to its own rebuild, which the disableSequence() right after it would otherwise mask.
    auto const swap = [&](Function const& enabled, Function const& disabled, std::string_view state) {
        sequences.enableSequence(enabled);
        check(std::format("{}: {} enabled", state, enabled.documentation.mnemonic));
        sequences.disableSequence(disabled);
        check(std::format("{}: {} enabled, {} disabled",
                          state,
                          enabled.documentation.mnemonic,
                          disabled.documentation.mnemonic));
    };
    swap(DECSLRM, SCOSC, "DECLRMM set");
    swap(SCOSC, DECSLRM, "DECLRMM reset");

    // Below DECSLRM's own level the swap pulls it from among the gated-out definitions, not from the end
    // of the active table.
    sequences.reset(VTType::VT100);
    swap(DECSLRM, SCOSC, "level VT100, DECLRMM set");
    swap(SCOSC, DECSLRM, "level VT100, DECLRMM reset");
}
