// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace contour::session
{

/// What a size indicator reports.
enum class SizeIndicatorKind : uint8_t
{
    PageSize = 0, //!< The page's columns and rows, after a resize.
    FontSize,     //!< The font size in points, after a zoom.
};

/// Whether the size indicator is on screen: a burst runs from the first change until the pill has been still
/// for its hold.
enum class SizeIndicatorBurst : uint8_t
{
    Idle = 0, //!< Nothing shows.
    Showing,  //!< The pill shows, and its hold restarts with every change.
};

/// Whether the profile asks for the indicator (`size_indicator_on_resize`), converted from its YAML boolean
/// at the session.
enum class SizeIndicatorSetting : uint8_t
{
    Off = 0, //!< Never shown.
    On,      //!< Shown on every resize and zoom, once the session is past its startup.
};

/// Whether a change may raise the indicator.
enum class SizeIndicatorGate : uint8_t
{
    Quiet = 0, //!< It may not: the session is starting, or the profile turned the indicator off.
    Open,      //!< It may.
};

/// How long after it starts a session raises no indicator: the window settling into its first size is not a
/// change the user made.
inline constexpr auto SizeIndicatorStartupQuiet = std::chrono::seconds { 1 };

/// What the burst's pill reports after @p event.
///
/// A font zoom resizes the page as a consequence; the pill keeps saying the size in points for as long as the
/// burst it started lasts.
/// @param burst Whether a pill shows already.
/// @param showing What it reports, if it shows.
/// @param event What just changed.
/// @return SizeIndicatorKind::FontSize while a zoom's pill shows, else @p event.
[[nodiscard]] constexpr SizeIndicatorKind nextSizeIndicatorKind(SizeIndicatorBurst burst,
                                                                SizeIndicatorKind showing,
                                                                SizeIndicatorKind event) noexcept
{
    if (burst == SizeIndicatorBurst::Showing && showing == SizeIndicatorKind::FontSize)
        return SizeIndicatorKind::FontSize;
    return event;
}

/// Whether a change may raise the indicator.
/// @param uptime How long the session has run.
/// @param setting The profile's `size_indicator_on_resize`.
/// @return SizeIndicatorGate::Open once @p uptime is past SizeIndicatorStartupQuiet and @p setting is on.
[[nodiscard]] constexpr SizeIndicatorGate sizeIndicatorGate(std::chrono::steady_clock::duration uptime,
                                                            SizeIndicatorSetting setting) noexcept
{
    return setting == SizeIndicatorSetting::On && uptime > SizeIndicatorStartupQuiet
               ? SizeIndicatorGate::Open
               : SizeIndicatorGate::Quiet;
}

/// The pill's text.
/// @param kind What it reports.
/// @param page The page, for SizeIndicatorKind::PageSize.
/// @param points The font size, for SizeIndicatorKind::FontSize.
/// @return `"100 × 30"` (columns first, U+00D7 between spaces), or `"13 pt"` / `"13.5 pt"` in the shortest
///         decimal form.
[[nodiscard]] inline std::string sizeIndicatorText(SizeIndicatorKind kind,
                                                   vtbackend::PageSize page,
                                                   double points)
{
    switch (kind)
    {
        case SizeIndicatorKind::PageSize:
            return std::format("{} × {}", unbox(page.columns), unbox(page.lines));
        case SizeIndicatorKind::FontSize: return std::format("{} pt", points);
    }
    return {};
}

namespace detail
{
    /// A unit the announcement counts, in both of its English forms.
    struct SizeIndicatorNoun
    {
        std::string_view singular;
        std::string_view plural;
    };

    /// The units the announcement names. Localisation is a later `tr()` of whole patterns; English needs only
    /// the two forms.
    inline constexpr auto ColumnNoun = SizeIndicatorNoun { .singular = "column", .plural = "columns" };
    inline constexpr auto RowNoun = SizeIndicatorNoun { .singular = "row", .plural = "rows" };
    inline constexpr auto PointNoun = SizeIndicatorNoun { .singular = "point", .plural = "points" };

    /// @return @p count, in its shortest decimal form, followed by @p noun in the form it takes: singular
    ///         where the number reads "1".
    template <typename Number>
    [[nodiscard]] std::string counted(Number count, SizeIndicatorNoun noun)
    {
        auto const number = std::format("{}", count);
        return std::format("{} {}", number, number == "1" ? noun.singular : noun.plural);
    }
} // namespace detail

/// What a screen reader is told once the burst has settled.
/// @param kind What the pill reported.
/// @param page The page, for SizeIndicatorKind::PageSize.
/// @param points The font size, for SizeIndicatorKind::FontSize.
/// @return `"100 columns, 30 rows"` (`"1 column, 1 row"`), or `"13 points"` (`"1 point"`).
[[nodiscard]] inline std::string sizeIndicatorAnnouncement(SizeIndicatorKind kind,
                                                           vtbackend::PageSize page,
                                                           double points)
{
    switch (kind)
    {
        case SizeIndicatorKind::PageSize:
            return std::format("{}, {}",
                               detail::counted(unbox(page.columns), detail::ColumnNoun),
                               detail::counted(unbox(page.lines), detail::RowNoun));
        case SizeIndicatorKind::FontSize: return detail::counted(points, detail::PointNoun);
    }
    return {};
}

} // namespace contour::session
