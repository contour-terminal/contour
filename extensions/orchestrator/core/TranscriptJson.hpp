// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// Format 1 of `contour agent transcript --json`, as Transcript.cpp reads it; nothing else includes this
/// header. Glaze reflects only types with linkage, so these cannot sit in Transcript.cpp's anonymous
/// namespace.
namespace orchestrator::transcript::detail
{

/// What is read of a page first: its format, which decides whether the rest may be read at all.
struct JsonFormat
{
    uint32_t format = 0;
};

/// One entry; its kind is read as text so that an unknown one is counted, not fatal.
struct JsonEntry
{
    std::string kind;
    std::string time;
    std::optional<int32_t> offset;
    std::string text;
};

/// One page. `format` is read first and checked before anything else is trusted.
struct JsonPage
{
    uint32_t format = 0;
    std::string session;
    std::string writtenBy;
    uint32_t page = 0;
    uint32_t pages = 0;
    uint32_t notUnderstood = 0;
    std::vector<JsonEntry> entries;
};

} // namespace orchestrator::transcript::detail
