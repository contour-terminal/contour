// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Win32SystemBackdrop.hpp>

#ifdef _WIN32

    #include <contour/platform/NativeWindowFrame.hpp>

    #include <QtCore/QVariant>
    #include <QtGui/QWindow>

namespace contour::platform
{

namespace
{
    /// The window properties the three wishes are recorded under, in the @ref needsFrameApplied manner:
    /// the enumerator + 1, so "never recorded" (an absent property, which converts to 0) is distinct.
    constexpr auto BandBackdropKey = "contour_band_backdrop";
    constexpr auto BandKindKey = "contour_band_kind";
    constexpr auto BlurBehindKey = "contour_blur_behind";

    template <typename E>
    void record(QWindow& window, char const* key, E value)
    {
        window.setProperty(key, static_cast<int>(value) + 1);
    }

    /// @return The value recorded under @p key, or @p fallback when none was.
    template <typename E>
    [[nodiscard]] E recorded(QWindow const& window, char const* key, E fallback)
    {
        auto const stored = window.property(key).toInt();
        return stored > 0 ? static_cast<E>(stored - 1) : fallback;
    }

    /// Extends @p hwnd's frame into its client area by @p margins.
    void extendFrame(DwmApi const& api, HWND hwnd, FrameMargins const& margins)
    {
        auto const native = MARGINS { .cxLeftWidth = margins.left,
                                      .cxRightWidth = margins.right,
                                      .cyTopHeight = margins.top,
                                      .cyBottomHeight = margins.bottom };
        api.extendFrameIntoClientArea(hwnd, &native);
    }

    /// Writes DWMWA_SYSTEMBACKDROP_TYPE.
    [[nodiscard]] HRESULT writeBackdrop(DwmApi const& api, HWND hwnd, DwmBackdrop backdrop)
    {
        auto value = static_cast<DWORD>(backdrop);
        return api.setWindowAttribute(
            hwnd, static_cast<DWORD>(DwmwaSystemBackdropType), &value, static_cast<DWORD>(sizeof(value)));
    }
} // namespace

void recordBandBackdrop(QWindow& window, config::BandBackdrop band, config::BackdropKind kind)
{
    record(window, BandBackdropKey, band);
    record(window, BandKindKey, kind);
}

void recordBlurBehind(QWindow& window, BlurBehindWish blur)
{
    record(window, BlurBehindKey, blur);
}

BlurBehindWish recordedBlurBehind(QWindow const& window)
{
    return recorded(window, BlurBehindKey, BlurBehindWish::Off);
}

std::expected<void, TitleBandMaterialError> applySystemBackdrop(QWindow& window, HWND hwnd, DwmApi const& api)
{
    if (api.setWindowAttribute == nullptr || api.extendFrameIntoClientArea == nullptr)
        return std::unexpected { TitleBandMaterialError::Unsupported };

    auto const band = recorded(window, BandBackdropKey, config::BandBackdrop::Client);
    auto const kind = recorded(window, BandKindKey, config::BackdropKind::Tabbed);
    auto const blur = recordedBlurBehind(window);
    // A window no frame was applied to yet is the kept client frame's to be: WindowController applies one
    // on its first expose, and the client decoration is the default.
    auto const decoration = appliedDecoration(window).value_or(WindowDecoration::Client);

    if (auto const hr = writeBackdrop(api, hwnd, systemBackdropFor(band, kind, blur)); FAILED(hr))
    {
        // No backdrop then, and the margins of a window without one: sheet-of-glass margins over no
        // backdrop paint the client area black. Writing none fails as well where the attribute is unknown,
        // which leaves it at none all the same.
        (void) writeBackdrop(api, hwnd, DwmBackdrop::None);
        extendFrame(
            api, hwnd, frameMarginsFor(config::BandBackdrop::Client, BlurBehindWish::Off, decoration));
        return std::unexpected { hr == E_INVALIDARG ? TitleBandMaterialError::Unsupported
                                                    : TitleBandMaterialError::Rejected };
    }

    extendFrame(api, hwnd, frameMarginsFor(band, blur, decoration));
    return {};
}

} // namespace contour::platform

#endif
