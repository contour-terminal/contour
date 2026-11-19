// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The shapes `contour agent transcript` reads from a session log and writes as format 1; only
/// AgentTranscript.cpp includes this header. Glaze reflects only types with linkage, so these cannot sit in
/// that file's anonymous namespace.

#include <contour/cli/AgentTranscript.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

namespace contour::cli::detail
{

/// The first read of every line: its type, from what was kept of it.
struct TypeField
{
    std::string type;
};

/// The binding read: the directory an entry was written in.
struct CwdField
{
    std::optional<std::string> cwd;
};

struct MessageFields
{
    std::optional<glz::generic> content;
};

/// What a line that may be shown is read into; every other key is skipped.
struct LineFields
{
    std::string type;
    std::optional<std::string> subtype;
    std::optional<std::string> timestamp;
    std::optional<std::string> version;
    std::optional<bool> isMeta;
    std::optional<bool> isSidechain;
    std::optional<bool> isCompactSummary;
    std::optional<MessageFields> message;
};

struct WireEntry
{
    std::string_view kind;
    std::string_view time;
    std::optional<int32_t> offset; ///< Minutes east of UTC the local time had then; absent: no UTC time.
    std::string_view text;
};

/// Format 1, its keys in their contract order.
struct WirePage
{
    uint32_t format = TranscriptFormat;
    std::string_view session;
    std::string_view writtenBy;
    uint32_t page = 1;
    uint32_t pages = 1;
    std::size_t notUnderstood = 0;
    std::vector<WireEntry> entries;
};

} // namespace contour::cli::detail
