// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/StateHandover.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <string>
#include <string_view>

#include <vtconformance/VtTestLog.hpp>

using namespace std::string_view_literals;

namespace
{
/// One recorded vttest transcript (test/cmd/*.cmd), compiled in.
struct VtTestTranscript
{
    std::string_view name; ///< Its file name.
    std::string_view text; ///< Its contents.
};

constexpr auto Transcripts = std::array {
#include <VtTestTranscripts.inc>
};

/// @return Every byte @p transcript records vttest sending the terminal, in order.
std::string bytesSentIn(std::string_view transcript)
{
    auto bytes = std::string {};
    for (auto const& record: vtconformance::parseVtTestLog(transcript))
        if (record.kind == vtconformance::VtTestRecordKind::Send
            || record.kind == vtconformance::VtTestRecordKind::Data)
            bytes += record.payload;
    return bytes;
}
} // namespace

TEST_CASE("round-trip at every byte offset of the vtconformance corpus", "[InFlightTail][StateRoundTrip]")
{
    REQUIRE_FALSE(Transcripts.empty());
    for (auto const& transcript: Transcripts)
    {
        DYNAMIC_SECTION(transcript.name)
        {
            auto const corpus = bytesSentIn(transcript.text);
            REQUIRE_FALSE(corpus.empty());
            auto const mismatch = vtbackend::test::findHandoverMismatch(
                corpus,
                vtbackend::test::HandoverSetup {
                    .pageSize =
                        vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) } });
            if (mismatch)
                FAIL(std::format("handed over after {} of {} bytes (\"{}\" next), compared after {}: {}",
                                 mismatch->offset,
                                 corpus.size(),
                                 core::escape(corpus.substr(mismatch->offset, 12)),
                                 mismatch->checkpoint,
                                 mismatch->what));
        }
    }
}
