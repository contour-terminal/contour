// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <core/log/LogStore.hpp>

#include <yaml-cpp/yaml.h>

#include <format>
#include <string>
#include <string_view>

namespace contour::config::detail
{

/// Where @p node stands in its file, as yaml-cpp's own errors say it: one-based.
/// @param node A node parsed from a file.
/// @return `line L, column C`.
[[nodiscard]] inline std::string placeOf(YAML::Node const& node)
{
    auto const mark = node.Mark();
    return std::format("line {}, column {}", mark.line + 1, mark.column + 1);
}

/// Reads a configuration enum spelled as one of the tokens its parser accepts.
///
/// Case-insensitive, and an unrecognized value is reported rather than silently accepted -- a typo
/// in a visible appearance setting should not pass unnoticed. It still leaves @p where at its
/// default, so a bad value costs a log line rather than a failed startup. Mirrors the GuiTheme
/// reader in Config.cpp.
///
/// It lives in a header, not in Config.cpp, because YAMLConfigReader's reader of a ConfigEnumConcept
/// enum is a member template that every translation unit instantiates for the enums it reads.
/// Defined in Config.cpp alone, the instantiations other files need exist only as long as the
/// optimizer keeps an out-of-line copy there, which GCC at -O3 does not.
///
/// @param node   The mapping to read from.
/// @param entry  The key within @p node.
/// @param shownAs What a diagnostic calls @p entry: its key path from the document's root.
/// @param where  Receives the value, and is left untouched when it is not recognized.
/// @param logger The reader's trace category, passed in because it is a member of the reader.
/// @param parse  Maps a spelling to a value. Defaults to the ConfigEnum.hpp token table; the
///               text_shaper enums pass their own `make*()`, since a `contour::config` table
///               cannot be hung off a type owned by a lower layer.
/// @param accepted The spellings to name in the diagnostic of an unknown value, already
///               formatted as a list; empty when the set is too large or too open to enumerate.
/// @return Whether @p entry was present and recognized, so a caller can add a fallback (a
///         deprecated spelling, say) without duplicating the reading and the reporting.
template <typename Enum, typename Parser = decltype(&configEnumFromToken<Enum>)>
bool loadConfigEnum(YAML::Node const& node,
                    std::string const& entry,
                    std::string const& shownAs,
                    Enum& where,
                    core::log::Category const& logger,
                    Parser parse = configEnumFromToken<Enum>,
                    std::string_view accepted = {})
{
    auto const child = node[entry];
    if (!child)
        return false;

    // Diagnosed here rather than left to as<std::string>() to throw, so that only this value is
    // lost and not the siblings read after it.
    if (child.IsSequence() || child.IsMap())
    {
        errorLog()("{} ({}) takes a single word, not a {}; keeping {}.",
                   shownAs,
                   placeOf(child),
                   child.IsMap() ? "map" : "list",
                   where);
        return false;
    }

    auto const rawValue = child.as<std::string>();
    logger()("Loading entry: {}, value {}", entry, rawValue);
    if (auto const value = parse(rawValue))
    {
        where = *value;
        return true;
    }
    errorLog()("Unknown value for {} ({}): '{}'; keeping {}.{}{}",
               shownAs,
               placeOf(child),
               rawValue,
               where,
               accepted.empty() ? "" : " Accepted values: ",
               accepted);
    return false;
}

} // namespace contour::config::detail
