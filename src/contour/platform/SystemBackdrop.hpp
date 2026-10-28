// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/platform/WindowDecoration.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Which DWM system backdrop a window asks for, and the frame margins it extends into its client area,
// Qt-free: the Win32 material and blur-behind ask these, so the one attribute both write is decided once,
// here, from the band's backdrop and the user's blur wish.
namespace contour::platform
{

/// DWMWA_SYSTEMBACKDROP_TYPE values, spelled out because the SDK may predate them. The adapter widens one to
/// the DWORD the attribute takes.
enum class DwmBackdrop : uint8_t
{
    Auto = 0,            ///< DWMSBT_AUTO: DWM decides.
    None = 1,            ///< DWMSBT_NONE: no system backdrop.
    MainWindow = 2,      ///< DWMSBT_MAINWINDOW: Mica.
    TransientWindow = 3, ///< DWMSBT_TRANSIENTWINDOW: Acrylic.
    TabbedWindow = 4,    ///< DWMSBT_TABBEDWINDOW: Mica Alt.
};

/// DWMWA_SYSTEMBACKDROP_TYPE, the DWM window attribute (Windows 11 22H2, build 22621, and later).
inline constexpr uint32_t DwmwaSystemBackdropType = 38;

/// Whether the user asked for blur behind the window (`background.blur`).
enum class BlurBehindWish : uint8_t
{
    Off = 0, ///< No blur: the band's backdrop decides.
    On,      ///< Blur behind the whole window.
};

/// DwmExtendFrameIntoClientArea's margins, in device pixels; -1 on every side is the sheet of glass.
struct FrameMargins
{
    int left = 0;   ///< Left margin.
    int right = 0;  ///< Right margin.
    int top = 0;    ///< Top margin.
    int bottom = 0; ///< Bottom margin.

    [[nodiscard]] bool operator==(FrameMargins const&) const = default;
};

/// One row of the backdrop table.
struct SystemBackdropRow
{
    config::BandBackdrop band {}; ///< The band's backdrop.
    config::BackdropKind kind {}; ///< Which window asks.
    DwmBackdrop backdrop {};      ///< What DWM draws behind it, without blur.
};

/// What DWM draws behind a window without blur: the native material is Mica Alt behind a tabbed window and
/// Mica behind Settings; a band of the client's own paint needs none.
inline constexpr auto SystemBackdropRows = std::array {
    SystemBackdropRow { .band = config::BandBackdrop::NativeMaterial,
                        .kind = config::BackdropKind::Tabbed,
                        .backdrop = DwmBackdrop::TabbedWindow },
    SystemBackdropRow { .band = config::BandBackdrop::NativeMaterial,
                        .kind = config::BackdropKind::Main,
                        .backdrop = DwmBackdrop::MainWindow },
};

/// Decides the DWM system backdrop.
/// @param band What the band shows behind it (window::resolveTitleBandSurface).
/// @param kind Which window asks: a tabbed terminal window, or Settings.
/// @param blur Whether the user asked for blur behind the window.
/// @return Acrylic over the whole window when blur is on -- the user's explicit choice, which replaces Mica
///         (R-i); else the matching @c SystemBackdropRows row; else @c DwmBackdrop::None.
[[nodiscard]] constexpr DwmBackdrop systemBackdropFor(config::BandBackdrop band,
                                                      config::BackdropKind kind,
                                                      BlurBehindWish blur) noexcept
{
    if (blur == BlurBehindWish::On)
        return DwmBackdrop::TransientWindow;
    auto const row = std::ranges::find_if(SystemBackdropRows, [&](SystemBackdropRow const& candidate) {
        return candidate.band == band && candidate.kind == kind;
    });
    return row != SystemBackdropRows.end() ? row->backdrop : DwmBackdrop::None;
}

/// How a re-apply reports DWM not knowing the backdrop it was asked for (E_INVALIDARG, before Windows 11
/// 22H2).
enum class UnsupportedBackdropReport : uint8_t
{
    Log = 0, ///< Worth a log line: the look the backdrop was to give is gone.
    Quiet,   ///< Expected: the wish reaches its look another way, and every re-apply would say so again.
};

/// One row of the unsupported-backdrop table.
struct UnsupportedBackdropRow
{
    BlurBehindWish blur {};              ///< The user's blur wish.
    UnsupportedBackdropReport report {}; ///< How DWM not knowing the backdrop is reported.
};

/// Blur on Windows 10 is Acrylic DWM does not know, and the accent-blur fallback (BlurBehind.cpp) draws it
/// instead; without blur the unknown backdrop is the band's material, which nothing replaces.
inline constexpr auto UnsupportedBackdropRows = std::array {
    UnsupportedBackdropRow { .blur = BlurBehindWish::Off, .report = UnsupportedBackdropReport::Log },
    UnsupportedBackdropRow { .blur = BlurBehindWish::On, .report = UnsupportedBackdropReport::Quiet },
};
static_assert(UnsupportedBackdropRows.size() == static_cast<std::size_t>(BlurBehindWish::On) + 1,
              "Every BlurBehindWish needs exactly one unsupported-backdrop row.");

/// Decides how DWM not knowing the decided backdrop is reported.
/// @param blur Whether the user asked for blur behind the window.
/// @return The matching @c UnsupportedBackdropRows row's report; @c Log for a wish without one.
[[nodiscard]] constexpr UnsupportedBackdropReport unsupportedBackdropReportFor(BlurBehindWish blur) noexcept
{
    auto const row = std::ranges::find(UnsupportedBackdropRows, blur, &UnsupportedBackdropRow::blur);
    return row != UnsupportedBackdropRows.end() ? row->report : UnsupportedBackdropReport::Log;
}

/// The margins a window with a system backdrop extends its frame by: all of its client area, so the
/// backdrop shows wherever the client paints nothing.
inline constexpr auto SheetOfGlassMargins = FrameMargins { .left = -1, .right = -1, .top = -1, .bottom = -1 };

/// The margins a window without one keeps: one pixel at the top, not zero. A zero margin disables the
/// extension outright, and a window DWM is not extending into is one it does not shadow. One pixel is the
/// smallest that keeps it on, and it sits under our own title bar where nothing shows.
inline constexpr auto ClientFrameMargins = FrameMargins { .left = 0, .right = 0, .top = 1, .bottom = 0 };

/// The margins a server-framed window keeps: none. Its frame is the OS's own, which never extended into the
/// client area; leaving the kept client frame takes back what that frame established (F32).
inline constexpr auto ServerFrameMargins = FrameMargins { .left = 0, .right = 0, .top = 0, .bottom = 0 };

/// Decides DwmExtendFrameIntoClientArea's margins.
/// @param band What the band shows behind it.
/// @param blur Whether the user asked for blur behind the window.
/// @param decoration Who draws the window's frame now.
/// @return @c ServerFrameMargins on the OS's own frame; else @c SheetOfGlassMargins under the native material
///         or blur, else @c ClientFrameMargins.
[[nodiscard]] constexpr FrameMargins frameMarginsFor(config::BandBackdrop band,
                                                     BlurBehindWish blur,
                                                     WindowDecoration decoration) noexcept
{
    if (decoration == WindowDecoration::Server)
        return ServerFrameMargins;
    if (band == config::BandBackdrop::NativeMaterial || blur == BlurBehindWish::On)
        return SheetOfGlassMargins;
    return ClientFrameMargins;
}

} // namespace contour::platform
