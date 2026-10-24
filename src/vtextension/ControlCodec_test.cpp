// SPDX-License-Identifier: Apache-2.0
//
// The Canonical ABI of the control vocabulary: its layouts against what the Canonical ABI computes, its
// round trips through a fake linear memory, and its refusal of what a guest could forge.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/Json.hpp>
#include <vtextension/ControlCodec.hpp>
#include <vtextension/testing/FakeGuestMemory.hpp>

using namespace vtextension;
namespace abi = vtextension::abi;
using FakeMemory = vtextension::testing::FakeGuestMemory;

namespace
{

/// A record whose float member does not flatten to an i32; the control vocabulary must never hold one.
struct FloatProbe
{
    using ApiRecordTag = void;

    std::string name {};
    float ratio = 0;
};

/// Stores @p value into a fresh memory and loads it back.
template <typename T>
[[nodiscard]] T roundTrip(T const& value)
{
    auto memory = FakeMemory {};
    auto const address = abi::storeNew(memory, value);
    REQUIRE(address.has_value());
    auto loaded = abi::Codec<T>::load(memory, *address);
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
}

/// Lowers @p value to flat values and lifts it back, checking the count of flat values on the way.
template <typename T>
[[nodiscard]] T flatRoundTrip(T const& value)
{
    auto memory = FakeMemory {};
    auto flat = abi::FlatValues {};
    REQUIRE(abi::Codec<T>::lowerFlat(memory, value, flat).has_value());
    REQUIRE(flat.size() == abi::Codec<T>::FlatCount);
    auto reader = abi::FlatReader(flat);
    auto lifted = abi::Codec<T>::liftFlat(memory, reader);
    REQUIRE(lifted.has_value());
    return std::move(*lifted);
}

[[nodiscard]] vtcontrol::SplitRequest splitRequest()
{
    return vtcontrol::SplitRequest { .pane = "pane:x",
                                     .direction = vtcontrol::SplitDirection::Below,
                                     .ratioPercent = 30,
                                     .cwd = "/tmp",
                                     .command = { "a", "b" },
                                     .label = "tail",
                                     .focus = vtcontrol::FocusPolicy::Keep };
}

[[nodiscard]] vtcontrol::ListReply listReply()
{
    auto const pane = [](std::string id, bool isActive) {
        return vtcontrol::PaneInfo {
            .id = std::move(id),
            .label = "",
            .title = "zsh",
            .cwd = "/home/me",
            .isActive = isActive,
            .size = { .columns = 80, .lines = 24 },
            .programStatus = { vtcontrol::ProgramStatusInfo { .id = "s1",
                                                              .state = vtcontrol::ProgramState::Blocked,
                                                              .kind = vtcontrol::BlockedKind::Question,
                                                              .progress = 42,
                                                              .app = "agent",
                                                              .title = "t",
                                                              .message = "m" } },
        };
    };
    auto const tab = [&](std::string const& id, uint32_t position) {
        return vtcontrol::TabInfo { .id = id,
                                    .label = "build",
                                    .title = "make",
                                    .position = position,
                                    .isActive = position == 1,
                                    .panes = { pane(id + "/p1", true), pane(id + "/p2", false) } };
    };
    return vtcontrol::ListReply {
        .windows = { vtcontrol::WindowInfo {
                         .id = "window:1", .label = "", .position = 1, .tabs = { tab("tab:a", 1) } },
                     vtcontrol::WindowInfo { .id = "window:2",
                                             .label = "w",
                                             .position = 2,
                                             .tabs = { tab("tab:b", 1), tab("tab:c", 2) } } },
    };
}

} // namespace

TEST_CASE("control records round-trip through guest memory", "[vtextension][control]")
{
    SECTION("a request with an enum, an option and a list")
    {
        auto const request = splitRequest();
        CHECK(vtcontrol::encodeJson(roundTrip(request)) == vtcontrol::encodeJson(request));
        CHECK(vtcontrol::encodeJson(flatRoundTrip(request)) == vtcontrol::encodeJson(request));

        auto withoutRatio = request;
        withoutRatio.ratioPercent = std::nullopt;
        CHECK_FALSE(roundTrip(withoutRatio).ratioPercent.has_value());
        CHECK_FALSE(flatRoundTrip(withoutRatio).ratioPercent.has_value());
    }

    SECTION("a reply of nested lists of records, with bools")
    {
        auto const reply = listReply();
        CHECK(vtcontrol::encodeJson(roundTrip(reply)) == vtcontrol::encodeJson(reply));
        CHECK(vtcontrol::encodeJson(flatRoundTrip(reply)) == vtcontrol::encodeJson(reply));
    }

    SECTION("a reply with optional records and a signed integer")
    {
        auto const reply = vtcontrol::SendReply { .block = vtcontrol::BlockInfo {
                                                      .id = "block:p/7",
                                                      .pane = "pane:p",
                                                      .state = vtcontrol::BlockState::Finished,
                                                      .command = "make",
                                                      .exitCode = -2,
                                                      .cwd = std::nullopt,
                                                      .promptAt = "2026-10-24T10:00:00Z",
                                                      .startedAt = std::nullopt,
                                                      .durationMs = 1200,
                                                  } };
        CHECK(vtcontrol::encodeJson(roundTrip(reply)) == vtcontrol::encodeJson(reply));
        CHECK(vtcontrol::encodeJson(flatRoundTrip(reply)) == vtcontrol::encodeJson(reply));
    }

    SECTION("a raw JSON field is a string")
    {
        auto const description =
            vtcontrol::ApiDescription { .name = "pane.split",
                                        .summary = "Split a pane.",
                                        .inputSchema = { .text = R"({"type":"object"})" },
                                        .outputSchema = { .text = "{}" },
                                        .requestExample = { .text = "{}" },
                                        .responseExample = { .text = "[1,2]" } };
        CHECK(roundTrip(description).inputSchema == description.inputSchema);
        CHECK(flatRoundTrip(description).responseExample == description.responseExample);
    }

    SECTION("a result: the reply")
    {
        using Result = std::expected<vtcontrol::SplitReply, vtcontrol::ControlError>;
        auto const ok = Result { vtcontrol::SplitReply { .tab = "tab:t", .pane = "pane:n" } };
        auto const loaded = roundTrip(ok);
        REQUIRE(loaded.has_value());
        CHECK(loaded->tab == "tab:t");
        CHECK(loaded->pane == "pane:n");
    }

    SECTION("a result: the error")
    {
        using Result = std::expected<vtcontrol::SplitReply, vtcontrol::ControlError>;
        auto const failed =
            Result { std::unexpected(vtcontrol::ControlError { .code = vtcontrol::ControlErrorCode::NotFound,
                                                               .message = "no pane 'x'",
                                                               .candidates = { "a", "b" } }) };
        for (auto const& loaded: { roundTrip(failed), flatRoundTrip(failed) })
        {
            REQUIRE_FALSE(loaded.has_value());
            CHECK(loaded.error().code == vtcontrol::ControlErrorCode::NotFound);
            CHECK(loaded.error().message == "no pane 'x'");
            CHECK(loaded.error().candidates == std::vector<std::string> { "a", "b" });
        }
    }

    SECTION("a result without a reply")
    {
        using Result = std::expected<vtcontrol::EmptyReply, vtcontrol::ControlError>;
        CHECK(abi::Codec<vtcontrol::EmptyReply>::Size == 0);
        CHECK(abi::Codec<vtcontrol::EmptyReply>::FlatCount == 0);
        CHECK(roundTrip(Result { vtcontrol::EmptyReply {} }).has_value());
    }
}

TEST_CASE("a control record's flat count matches the Canonical ABI", "[vtextension][control]")
{
    // pane 2, direction 1, ratioPercent 2, cwd 2, command 2, label 2, focus 1.
    STATIC_CHECK(abi::Codec<vtcontrol::SplitRequest>::FlatCount == 12);
    // pane 0..8, direction 8, ratioPercent 9..11, cwd 12..20, command 20..28, label 28..36, focus 36.
    STATIC_CHECK(abi::Codec<vtcontrol::SplitRequest>::Offsets
                 == std::array<uint32_t, 7> { 0, 8, 9, 12, 20, 28, 36 });
    STATIC_CHECK(abi::Codec<vtcontrol::SplitRequest>::Size == 40);
    STATIC_CHECK(abi::Codec<vtcontrol::SplitRequest>::Alignment == 4);
    // An enum of seven cases is a u8; the error is code 1, message 2, candidates 2.
    STATIC_CHECK(abi::Codec<vtcontrol::ControlErrorCode>::Size == 1);
    STATIC_CHECK(abi::Codec<vtcontrol::ControlError>::FlatCount == 5);
    STATIC_CHECK(abi::Codec<bool>::Size == 1);
    STATIC_CHECK(abi::Codec<std::optional<uint32_t>>::Size == 8);
    STATIC_CHECK(abi::Codec<std::optional<uint32_t>>::FlatCount == 2);
}

TEST_CASE("the control vocabulary flattens to i32 values only", "[vtextension][control]")
{
    STATIC_CHECK(abi::ControlVocabularyFlattensToI32);
    STATIC_CHECK_FALSE(abi::detail::flattensToI32<FloatProbe>());
    STATIC_CHECK_FALSE(abi::detail::flattensToI32<std::vector<uint64_t>>());
}

TEST_CASE("an out-of-bounds string in a control record traps", "[vtextension][control]")
{
    auto memory = FakeMemory {};
    auto flat = abi::FlatValues {};
    REQUIRE(abi::Codec<vtcontrol::SplitRequest>::lowerFlat(memory, splitRequest(), flat).has_value());

    SECTION("a pointer past the end of memory")
    {
        flat[0] = FakeMemory::Size - 2; // pane's pointer; its length is 6
    }
    SECTION("a length past the end of memory")
    {
        flat[1] = FakeMemory::Size; // pane's length
    }
    SECTION("a list whose element points outside memory")
    {
        // command's list: its first element's string pointer, forged in memory.
        memory.poke(flat[7], FakeMemory::Size + 16);
    }
    auto reader = abi::FlatReader(flat);
    auto const lifted = abi::Codec<vtcontrol::SplitRequest>::liftFlat(memory, reader);
    REQUIRE_FALSE(lifted.has_value());
    CHECK(lifted.error() == ExtensionError::Trapped);
}

TEST_CASE("an unknown enum case in a control record traps", "[vtextension][control]")
{
    auto memory = FakeMemory {};
    auto flat = abi::FlatValues {};
    REQUIRE(abi::Codec<vtcontrol::SplitRequest>::lowerFlat(memory, splitRequest(), flat).has_value());
    flat[2] = 2; // direction has the cases right and below
    auto reader = abi::FlatReader(flat);
    auto const lifted = abi::Codec<vtcontrol::SplitRequest>::liftFlat(memory, reader);
    REQUIRE_FALSE(lifted.has_value());
    CHECK(lifted.error() == ExtensionError::Trapped);
}
