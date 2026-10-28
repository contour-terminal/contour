// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Win32Settings.hpp>

#ifdef _WIN32

    #include <QtCore/QByteArray>
    #include <QtGui/QGuiApplication>

    #include <algorithm>
    #include <array>
    #include <cwchar>
    #include <optional>
    #include <string_view>

    #include <Windows.h>

namespace contour::platform
{

namespace
{
    /// A DWORD value under HKEY_CURRENT_USER, and the reading it fills.
    struct RegistryDword
    {
        wchar_t const* key = nullptr;                                         ///< The subkey.
        wchar_t const* name = nullptr;                                        ///< The value's name.
        std::optional<uint32_t> WindowsSettingsReadings::* reading = nullptr; ///< Where it goes.
    };

    constexpr auto RegistryDwords = std::array {
        RegistryDword { .key = L"Software\\Microsoft\\Windows\\DWM",
                        .name = L"AccentColor",
                        .reading = &WindowsSettingsReadings::accentColor },
        RegistryDword { .key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        .name = L"EnableTransparency",
                        .reading = &WindowsSettingsReadings::enableTransparency },
        RegistryDword { .key = L"Control Panel\\Accessibility",
                        .name = L"DynamicScrollbars",
                        .reading = &WindowsSettingsReadings::dynamicScrollbars },
    };

    /// @return The DWORD @p dword names, or nothing when it is absent or not a DWORD.
    [[nodiscard]] std::optional<uint32_t> readDword(RegistryDword const& dword)
    {
        auto value = DWORD { 0 };
        auto size = static_cast<DWORD>(sizeof(value));
        if (RegGetValueW(HKEY_CURRENT_USER, dword.key, dword.name, RRF_RT_REG_DWORD, nullptr, &value, &size)
            != ERROR_SUCCESS)
            return std::nullopt;
        return static_cast<uint32_t>(value);
    }

    [[nodiscard]] HighContrast readHighContrast()
    {
        auto info = HIGHCONTRASTW {};
        info.cbSize = static_cast<UINT>(sizeof(info));
        if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, info.cbSize, &info, 0))
            return HighContrast::Off;
        return (info.dwFlags & HCF_HIGHCONTRASTON) != 0 ? HighContrast::On : HighContrast::Off;
    }

    [[nodiscard]] ClientAreaAnimation readClientAreaAnimation()
    {
        auto enabled = BOOL { TRUE };
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
            return ClientAreaAnimation::On;
        return enabled ? ClientAreaAnimation::On : ClientAreaAnimation::Off;
    }

    /// @return RtlGetVersion's build, resolved at run time: GetVersionEx reports Windows 8 without a
    /// manifest.
    [[nodiscard]] std::optional<uint32_t> readOsBuild()
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto* ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
            return std::nullopt;
        auto const getVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        auto info = OSVERSIONINFOW {};
        info.dwOSVersionInfoSize = static_cast<DWORD>(sizeof(info));
        if (getVersion == nullptr || getVersion(&info) != 0)
            return std::nullopt;
        return static_cast<uint32_t>(info.dwBuildNumber);
    }

    /// A broadcast that carries nothing but its kind.
    struct BareMessage
    {
        UINT message = 0;                                          ///< The Win32 message.
        SettingMessage kind = SettingMessage::ColorizationChanged; ///< What topicOf calls it.
    };

    constexpr auto BareMessages = std::array {
        BareMessage { .message = WM_DWMCOLORIZATIONCOLORCHANGED,
                      .kind = SettingMessage::ColorizationChanged },
        BareMessage { .message = WM_THEMECHANGED, .kind = SettingMessage::ThemeChanged },
    };

    /// @return @p msg as a settings broadcast, or nothing when it is none.
    [[nodiscard]] std::optional<SettingChange> settingChangeOf(MSG const& msg)
    {
        if (msg.message == WM_SETTINGCHANGE)
        {
            // lParam names the area, or is null. wchar_t is UTF-16 on Windows.
            auto const* area = reinterpret_cast<wchar_t const*>(msg.lParam);
            return SettingChange {
                .message = SettingMessage::SettingChange,
                .action = static_cast<uint32_t>(msg.wParam),
                .area = area != nullptr ? std::u16string_view { reinterpret_cast<char16_t const*>(area),
                                                                std::wcslen(area) }
                                        : std::u16string_view {},
            };
        }
        auto const bare = std::ranges::find(BareMessages, msg.message, &BareMessage::message);
        if (bare == BareMessages.end())
            return std::nullopt;
        return SettingChange { .message = bare->kind, .action = 0, .area = {} };
    }
} // namespace

WindowsSettingsReadings Win32SettingsReader::read() const
{
    auto readings = WindowsSettingsReadings {
        .accentColor = std::nullopt,
        .enableTransparency = std::nullopt,
        .dynamicScrollbars = std::nullopt,
        .highContrast = readHighContrast(),
        .animation = readClientAreaAnimation(),
        .osBuild = readOsBuild(),
    };
    for (auto const& dword: RegistryDwords)
        readings.*dword.reading = readDword(dword);
    return readings;
}

bool onNativeWindowsPlatform()
{
    return QGuiApplication::platformName() == QStringLiteral("windows");
}

Win32SettingsWatcher::Win32SettingsWatcher(std::function<void(SettingTopic)> onTopic):
    _onTopic { std::move(onTopic) }
{
    installIfPossible();
}

Win32SettingsWatcher::~Win32SettingsWatcher()
{
    if (_installedOn)
        _installedOn->removeNativeEventFilter(this);
}

void Win32SettingsWatcher::installIfPossible()
{
    if (_installedOn || QCoreApplication::instance() == nullptr || !onNativeWindowsPlatform())
        return;
    _installedOn = QCoreApplication::instance();
    _installedOn->installNativeEventFilter(this);
}

bool Win32SettingsWatcher::nativeEventFilter(QByteArray const& eventType, void* message, qintptr* /*result*/)
{
    if (eventType != "windows_generic_MSG" || message == nullptr)
        return false;
    auto const change = settingChangeOf(*static_cast<MSG const*>(message));
    if (!change)
        return false;
    if (auto const topic = topicOf(*change); topic != SettingTopic::None && _onTopic)
        _onTopic(topic);
    return false;
}

std::unique_ptr<IAccessibilityPreferenceSource> makeAccessibilityPreferenceSource(
    AccessibilityPreferencesChanged onChange)
{
    // Unconditional, though built before the application exists: the source asks the platform on each call
    // and reports the defaults off the native Windows one (F1).
    return std::make_unique<Win32AccessibilityPreferenceSource>(std::make_shared<Win32SettingsReader>(),
                                                                std::move(onChange));
}

std::unique_ptr<IPlatformAppearance> makePlatformAppearance(PlatformAppearanceChanged onChange)
{
    return std::make_unique<Win32PlatformAppearance>(std::make_shared<Win32SettingsReader>(),
                                                     std::move(onChange));
}

} // namespace contour::platform

#endif
