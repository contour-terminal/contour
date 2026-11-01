// SPDX-License-Identifier: Apache-2.0
//
// The Win32 title-band material on a real HWND: Mica Alt behind the band, one owner for the backdrop with
// blur-behind, and margins that survive a composition restart. Native Windows platform only; the decisions
// themselves are SystemBackdrop_test.cpp's, on every OS.

#ifdef _WIN32

    #include <contour/platform/BlurBehind.hpp>
    #include <contour/platform/Dwmapi.hpp>
    #include <contour/platform/QtPlatformPlugin.hpp>
    #include <contour/platform/SystemBackdrop.hpp>
    #include <contour/platform/TitleBandMaterial.hpp>
    #include <contour/platform/Win32TitleBandMaterial.hpp>
    #include <contour/platform/Win32WindowFrame.hpp>
    #include <contour/platform/WindowsSettings.hpp>

    #include <QtQuick/QQuickWindow>

    #include <catch2/catch_test_macros.hpp>

    #include <memory>
    #include <optional>
    #include <vector>

    #include <Windows.h>

    #include <QtTest/QTest>

using contour::config::BackdropKind;
using contour::config::BandBackdrop;
using contour::platform::BlurBehindWish;
using contour::platform::DwmApi;
using contour::platform::DwmBackdrop;
using contour::platform::DwmwaSystemBackdropType;
using contour::platform::FrameMargins;
using contour::platform::TitleBandMaterialError;
using contour::platform::TitleBandRequest;

namespace
{

/// The window's logical size.
constexpr auto WindowWidth = 800;
constexpr auto WindowHeight = 500;

/// @return The running Windows' build number, from RtlGetVersion (GetVersionEx lies without a manifest).
[[nodiscard]] DWORD osBuild()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto const getVersion =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    auto info = OSVERSIONINFOW {};
    info.dwOSVersionInfoSize = static_cast<DWORD>(sizeof(info));
    return getVersion != nullptr && getVersion(&info) == 0 ? info.dwBuildNumber : 0;
}

/// @return Whether this Windows has the system backdrop at all.
[[nodiscard]] bool hasSystemBackdrop()
{
    return osBuild() >= contour::platform::SystemBackdropBuild;
}

/// @return The DWORD window attribute @p attribute of @p hwnd as DWM holds it, or nothing if unreadable.
[[nodiscard]] std::optional<DWORD> attributeOf(HWND hwnd, DWORD attribute)
{
    auto const& api = contour::platform::dwmApi();
    if (api.getWindowAttribute == nullptr)
        return std::nullopt;
    auto value = DWORD { 0 };
    if (FAILED(api.getWindowAttribute(hwnd, attribute, &value, static_cast<DWORD>(sizeof(value)))))
        return std::nullopt;
    return value;
}

/// @return DWMWA_SYSTEMBACKDROP_TYPE of @p hwnd.
[[nodiscard]] std::optional<DWORD> backdropOf(HWND hwnd)
{
    return attributeOf(hwnd, static_cast<DWORD>(DwmwaSystemBackdropType));
}

/// @return @p backdrop as DWM reports it.
[[nodiscard]] std::optional<DWORD> dword(DwmBackdrop backdrop)
{
    return static_cast<DWORD>(backdrop);
}

/// A DwmApi whose backdrop attribute answers what a case sets, and which records every margin extended.
/// Captureless functions over static state, because DwmApi holds raw function pointers; reset per case.
struct StubDwm
{
    static inline auto backdropResult = HRESULT { S_OK };
    static inline auto margins = std::vector<FrameMargins> {};

    static HRESULT WINAPI setWindowAttribute(HWND /*hwnd*/,
                                             DWORD attribute,
                                             LPCVOID /*value*/,
                                             DWORD /*size*/)
    {
        return attribute == static_cast<DWORD>(DwmwaSystemBackdropType) ? backdropResult : S_OK;
    }

    static HRESULT WINAPI extendFrameIntoClientArea(HWND /*hwnd*/, MARGINS const* native)
    {
        margins.push_back(FrameMargins { .left = native->cxLeftWidth,
                                         .right = native->cxRightWidth,
                                         .top = native->cyTopHeight,
                                         .bottom = native->cyBottomHeight });
        return S_OK;
    }

    /// Resets the recorders and answers @p result for the backdrop attribute.
    /// @return The stub's entry points, with the system's DPI ones, so a frame can be kept over them.
    [[nodiscard]] static DwmApi make(HRESULT result)
    {
        backdropResult = result;
        margins.clear();
        auto const& system = contour::platform::dwmApi();
        return DwmApi { .setWindowAttribute = &setWindowAttribute,
                        .extendFrameIntoClientArea = &extendFrameIntoClientArea,
                        .getWindowAttribute = nullptr,
                        .getDpiForWindow = system.getDpiForWindow,
                        .adjustWindowRectExForDpi = system.adjustWindowRectExForDpi };
    }
};

/// A shown, transparent QQuickWindow with the kept Win32 frame, speaking through @p api.
struct MaterialWindow
{
    std::unique_ptr<contour::platform::NativeWindowFrame> frame;
    QQuickWindow window {};

    explicit MaterialWindow(DwmApi const& api = contour::platform::dwmApi()):
        frame { contour::platform::makeWin32WindowFrame([] { return Qt::ColorScheme::Dark; }, api) }
    {
        window.setColor(Qt::transparent);
        window.resize(WindowWidth, WindowHeight);
        window.create();
        REQUIRE(frame != nullptr);
        frame->apply(window, contour::platform::WindowDecoration::Client);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
    }

    /// @return The platform window.
    [[nodiscard]] HWND hwnd() const { return reinterpret_cast<HWND>(window.winId()); }
};

constexpr auto MicaAlt = TitleBandRequest { .backdrop = BandBackdrop::NativeMaterial,
                                            .kind = BackdropKind::Tabbed,
                                            .growth = contour::config::TitleBarGrowth::None,
                                            .scheme = Qt::ColorScheme::Unknown };

} // namespace

TEST_CASE("Mica Alt on a tabbed window", "[native][win32][mica]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    // Built before the window exists, as ContourGuiApp builds it before the application (F1): the shared
    // factory hands out the Win32 material, which still acts once a window does.
    auto const material = contour::platform::makeTitleBandMaterial();
    auto fixture = MaterialWindow {};

    auto const applied = material->apply(fixture.window, MicaAlt);
    if (!hasSystemBackdrop())
    {
        REQUIRE_FALSE(applied.has_value());
        CHECK(applied.error() == TitleBandMaterialError::Unsupported);
        return;
    }
    CHECK(applied.has_value());
    CHECK(backdropOf(fixture.hwnd()) == dword(DwmBackdrop::TabbedWindow));

    SECTION("Client restores none and the 1-px margin")
    {
        CHECK(material->apply(fixture.window, TitleBandRequest {}).has_value());
        CHECK(backdropOf(fixture.hwnd()) == dword(DwmBackdrop::None));
    }
}

TEST_CASE("a rejected backdrop attribute reports Unsupported and restores the margins",
          "[native][win32][mica]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    auto const api = StubDwm::make(E_INVALIDARG);
    auto const material = contour::platform::makeWin32TitleBandMaterial(api);
    REQUIRE(material != nullptr);
    auto fixture = MaterialWindow {};

    auto const applied = material->apply(fixture.window, MicaAlt);
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error() == TitleBandMaterialError::Unsupported);
    REQUIRE_FALSE(StubDwm::margins.empty());
    CHECK(StubDwm::margins.back() == contour::platform::ClientFrameMargins);

    SECTION("any other failure is Rejected")
    {
        auto const rejecting = StubDwm::make(E_FAIL);
        auto const other = contour::platform::makeWin32TitleBandMaterial(rejecting);
        auto const result = other->apply(fixture.window, MicaAlt);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == TitleBandMaterialError::Rejected);
    }

    SECTION("an accepted one extends the sheet of glass")
    {
        auto const accepting = StubDwm::make(S_OK);
        auto const other = contour::platform::makeWin32TitleBandMaterial(accepting);
        CHECK(other->apply(fixture.window, MicaAlt).has_value());
        REQUIRE_FALSE(StubDwm::margins.empty());
        CHECK(StubDwm::margins.back() == contour::platform::SheetOfGlassMargins);
    }
}

TEST_CASE("a client band where DWM lacks the backdrop attribute is no failure", "[native][win32][mica]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    // Before Windows 11 22H2 even DWMSBT_NONE fails with E_INVALIDARG; no material was asked for.
    auto const api = StubDwm::make(E_INVALIDARG);
    auto const material = contour::platform::makeWin32TitleBandMaterial(api);
    auto fixture = MaterialWindow {};

    CHECK(material->apply(fixture.window, TitleBandRequest {}).has_value());
    REQUIRE_FALSE(StubDwm::margins.empty());
    CHECK(StubDwm::margins.back() == contour::platform::ClientFrameMargins);
}

// [win11-22h2]: SKIPs on an older Windows, so CI's "[win32] cases actually ran" guard leaves it out.
TEST_CASE("blur wins over Mica, and off restores it", "[native][win32][mica][win11-22h2]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    if (!hasSystemBackdrop())
        SKIP("Requires Windows 11 22H2 or later.");
    auto const material = contour::platform::makeTitleBandMaterial();
    auto fixture = MaterialWindow {};
    REQUIRE(material->apply(fixture.window, MicaAlt).has_value());

    contour::platform::setBlurBehind(&fixture.window, BlurBehindWish::On);
    CHECK(backdropOf(fixture.hwnd()) == dword(DwmBackdrop::TransientWindow));

    contour::platform::setBlurBehind(&fixture.window, BlurBehindWish::Off);
    CHECK(backdropOf(fixture.hwnd()) == dword(DwmBackdrop::TabbedWindow));
}

TEST_CASE("a composition restart extends the window's frame again", "[native][win32][mica]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    auto const api = StubDwm::make(S_OK);
    auto const material = contour::platform::makeWin32TitleBandMaterial(api);
    auto fixture = MaterialWindow { api };
    REQUIRE(material->apply(fixture.window, MicaAlt).has_value());
    StubDwm::margins.clear();

    // A restart drops what DwmExtendFrameIntoClientArea established. The frame's filter extends it again
    // from the window's records: the sheet of glass under Mica, not a fixed one-pixel margin.
    SendMessageW(fixture.hwnd(), WM_DWMCOMPOSITIONCHANGED, 0, 0);
    REQUIRE_FALSE(StubDwm::margins.empty());
    CHECK(StubDwm::margins.back() == contour::platform::SheetOfGlassMargins);
}

TEST_CASE("the corner preference stays round under Mica", "[native][win32][mica]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    auto const material = contour::platform::makeTitleBandMaterial();
    auto fixture = MaterialWindow {};
    // Unsupported below 22H2 is fine here: the frame owns the corner either way (R-h).
    (void) material->apply(fixture.window, MicaAlt);

    CHECK(attributeOf(fixture.hwnd(), contour::platform::DwmwaWindowCornerPreference)
          == std::optional { contour::platform::DwmwcpRound });
}

TEST_CASE("the Settings window keeps the OS's title bar, dark with the chrome and Mica behind it",
          "[native][win32][settings-frame]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    auto const frame = contour::platform::makeWin32WindowFrame([] { return Qt::ColorScheme::Dark; });
    REQUIRE(frame != nullptr);
    auto const material = contour::platform::makeTitleBandMaterial();
    auto window = QQuickWindow {};
    window.resize(WindowWidth, WindowHeight);
    window.create();
    frame->apply(window, contour::platform::WindowDecoration::Server);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    auto* const hwnd = reinterpret_cast<HWND>(window.winId());

    // The OS's own title bar follows the chrome's dark scheme (R-m).
    CHECK(attributeOf(hwnd, contour::platform::DwmwaUseImmersiveDarkMode)
          == std::optional { DWORD { TRUE } });

    auto const request = TitleBandRequest { .backdrop = BandBackdrop::NativeMaterial,
                                            .kind = BackdropKind::Main,
                                            .growth = contour::config::TitleBarGrowth::None,
                                            .scheme = Qt::ColorScheme::Dark };
    auto const applied = material->apply(window, request);
    if (!hasSystemBackdrop())
    {
        // Below Windows 11 22H2 there is no Mica to ask for; the window keeps the solid title bar.
        REQUIRE_FALSE(applied.has_value());
        CHECK(applied.error() == TitleBandMaterialError::Unsupported);
        return;
    }
    CHECK(applied.has_value());
    CHECK(backdropOf(hwnd) == dword(DwmBackdrop::MainWindow));
}

TEST_CASE("the kept frame leaves DWM no system buttons to draw under our own", "[native][win32][frame]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    auto fixture = MaterialWindow {};
    auto const style = [&] {
        return static_cast<DWORD>(GetWindowLongPtrW(fixture.hwnd(), GWL_STYLE));
    };

    // What DWM keys its shadow, rounded corners and Snap Layouts off stays; what makes it paint its own
    // minimize/maximize/close buttons next to ours -- every button twice -- goes.
    CHECK((style() & WS_CAPTION) == WS_CAPTION);
    CHECK((style() & WS_THICKFRAME) != 0);
    CHECK((style() & WS_MAXIMIZEBOX) != 0);
    CHECK((style() & WS_SYSMENU) == 0);

    SECTION("handing the window back to the OS's title bar restores its buttons")
    {
        fixture.frame->apply(fixture.window, contour::platform::WindowDecoration::Server);
        CHECK((style() & WS_SYSMENU) != 0);
    }
}

#endif
