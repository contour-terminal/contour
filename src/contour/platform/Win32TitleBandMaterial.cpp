// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Win32TitleBandMaterial.hpp>

#ifdef _WIN32

    #include <contour/platform/Win32SystemBackdrop.hpp>

    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

namespace contour::platform
{

namespace
{
    /// @return Whether the running platform plugin is the native Windows one, the only one with an HWND.
    ///         Asked on every call rather than at construction, which precedes the application.
    [[nodiscard]] bool onWindows()
    {
        return QGuiApplication::platformName() == QStringLiteral("windows");
    }

    class Win32TitleBandMaterial final: public ITitleBandMaterial
    {
      public:
        explicit Win32TitleBandMaterial(DwmApi const& api): _api { api } {}

        [[nodiscard]] std::expected<void, TitleBandMaterialError> apply(
            QWindow& window, TitleBandRequest const& request) override
        {
            // Deliberately not winId() first, which would create a platform window: one with no handle yet
            // is left alone, as the frame leaves it, and dressed on its first expose.
            if (!onWindows() || window.handle() == nullptr)
                return {};
            auto* hwnd = reinterpret_cast<HWND>(window.winId());
            if (hwnd == nullptr)
                return {};
            recordBandBackdrop(window, request.backdrop, request.kind);
            return applySystemBackdrop(window, hwnd, _api);
        }

        /// Zeros: the band replaces the title bar; there is no native one to measure.
        [[nodiscard]] NativeTitleBarMetrics metrics(QWindow& /*window*/) const override { return {}; }

      private:
        DwmApi const& _api;
    };
} // namespace

std::unique_ptr<ITitleBandMaterial> makeWin32TitleBandMaterial(DwmApi const& api)
{
    if (api.setWindowAttribute == nullptr || api.extendFrameIntoClientArea == nullptr)
        return nullptr;
    return std::make_unique<Win32TitleBandMaterial>(api);
}

} // namespace contour::platform

#endif
