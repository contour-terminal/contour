// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Capabilities.h>

#include <crispy/utils.h>

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <string>

using namespace std::string_view_literals;
using crispy::fromHexString;

TEST_CASE("Capabilities.codeFromName")
{
    vtbackend::capabilities::StaticDatabase const tcap;
    auto const capName = fromHexString("62656c"sv).value();
    auto const tn = tcap.codeFromName(capName);
    REQUIRE(tn.has_value());
    CHECK(tn.value().code == 0x626c);
    CHECK(tn.value().hex() == "626C");
}

TEST_CASE("Capabilities.get")
{
    vtbackend::capabilities::StaticDatabase const tcap;
    auto const rgb = tcap.stringCapability("RGB");
    REQUIRE(rgb == "8/8/8");

    auto const colors = tcap.numericCapability("colors");
    REQUIRE(colors == std::numeric_limits<int16_t>::max());

    auto const bce = tcap.numericCapability("bce");
    REQUIRE(bce);
}

// Issue #1861: Empty string terminfo capabilities cause input to be swallowed in programs
// like less and bat. When capabilities are defined as empty strings (e.g., "ka1=,"),
// buggy parsers match any input against them.
TEST_CASE("Capabilities.terminfo_no_empty_string_values", "[issue-1861]")
{
    vtbackend::capabilities::StaticDatabase const tcap;
    auto const terminfo = tcap.terminfo();

    // The regex matches sequences like "\n    name=,\n" where the value after '=' is empty.
    // In terminfo format, "name=value," defines a string capability.
    // "name=,\n" means the value is an empty string — this must never appear.
    // Note: We avoid std::regex::multiline because MSVC does not support it.
    auto const emptyValuePattern = std::regex(R"(\n\s+(\w+)=,\n)");

    auto begin = std::sregex_iterator(terminfo.begin(), terminfo.end(), emptyValuePattern);
    auto end = std::sregex_iterator();

    std::string emptyCapNames;
    for (auto it = begin; it != end; ++it)
    {
        if (!emptyCapNames.empty())
            emptyCapNames += ", ";
        emptyCapNames += (*it)[1].str();
    }

    INFO("Capabilities with empty string values: " << emptyCapNames);
    CHECK(emptyCapNames.empty());
}

// Verify that previously-empty keypad capabilities (ka1, ka3, kc1, kc3) are no longer
// present in the terminfo output at all — they should be omitted, not set to empty.
TEST_CASE("Capabilities.keypad_caps_not_in_terminfo", "[issue-1861]")
{
    vtbackend::capabilities::StaticDatabase const tcap;
    auto const terminfo = tcap.terminfo();

    CHECK(terminfo.find("ka1=") == std::string::npos);
    CHECK(terminfo.find("ka3=") == std::string::npos);
    CHECK(terminfo.find("kc1=") == std::string::npos);
    CHECK(terminfo.find("kc3=") == std::string::npos);
}

// Verify that khlp and kund (which were also empty) are omitted.
TEST_CASE("Capabilities.help_undo_caps_not_in_terminfo", "[issue-1861]")
{
    vtbackend::capabilities::StaticDatabase const tcap;
    auto const terminfo = tcap.terminfo();

    CHECK(terminfo.find("khlp=") == std::string::npos);
    CHECK(terminfo.find("kund=") == std::string::npos);
}

// Verify that non-empty string capabilities are still present in terminfo output.
TEST_CASE("Capabilities.non_empty_caps_still_present", "[issue-1861]")
{
    vtbackend::capabilities::StaticDatabase const tcap;
    auto const terminfo = tcap.terminfo();

    // These are well-known capabilities that must still be present.
    CHECK(terminfo.find("bold=") != std::string::npos);
    CHECK(terminfo.find("clear=") != std::string::npos);
    CHECK(terminfo.find("kcub1=") != std::string::npos);
    CHECK(terminfo.find("kcud1=") != std::string::npos);
    CHECK(terminfo.find("kf1=") != std::string::npos);
    CHECK(terminfo.find("smkx=") != std::string::npos);
    CHECK(terminfo.find("rmkx=") != std::string::npos);
}

// Verify that the stringCapability() API returns an empty view for removed capabilities,
// confirming they are no longer part of the static database.
TEST_CASE("Capabilities.removed_caps_return_empty_from_api", "[issue-1861]")
{
    vtbackend::capabilities::StaticDatabase const tcap;

    CHECK(tcap.stringCapability("ka1").empty());
    CHECK(tcap.stringCapability("ka3").empty());
    CHECK(tcap.stringCapability("kc1").empty());
    CHECK(tcap.stringCapability("kc3").empty());
    CHECK(tcap.stringCapability("khlp").empty());
    CHECK(tcap.stringCapability("kund").empty());
}
