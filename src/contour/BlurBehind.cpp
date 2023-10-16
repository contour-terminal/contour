// SPDX-License-Identifier: Apache-2.0
#include <contour/BlurBehind.h>
#include <contour/ContourGuiApp.h>

#include <crispy/utils.h>

#include <QtCore/QDebug>
#include <QtGui/QWindow>

#if defined(_WIN32)
    #include <Windows.h>
#endif

#if !defined(Q_OS_WINDOWS) && !defined(Q_OS_DARWIN)
    #define CONTOUR_FRONTEND_XCB
    #include <xcb/xproto.h>
#endif

#if defined(CONTOUR_FRONTEND_XCB)
    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        #include <QtGui/QGuiApplication>
    #else
        #include <QtX11Extras/QX11Info>
    #endif
#endif

namespace BlurBehind
{

using std::nullopt;
using std::optional;
using std::string;

#if defined(CONTOUR_FRONTEND_XCB)
namespace
{
    struct XcbPropertyInfo
    {
        xcb_connection_t* connection;
        xcb_window_t window;
        xcb_atom_t propertyAtom;
    };

    xcb_connection_t* x11Connection()
    {
    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (!qApp)
            return nullptr;

        auto* native = qApp->nativeInterface<QNativeInterface::QX11Application>();
        if (!native)
            return nullptr;

        return native->connection();
    #else
        return QX11Info::connection();
    #endif
    }

    optional<XcbPropertyInfo> queryXcbPropertyInfo(QWindow* window, string const& name)
    {
        auto const winId = static_cast<xcb_window_t>(window->winId());

        xcb_connection_t* xcbConnection = x11Connection();
        if (!xcbConnection)
            return nullopt;

        auto const atomNameCookie =
            xcb_intern_atom(xcbConnection, 0, static_cast<uint16_t>(name.size()), name.c_str());
        xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(xcbConnection, atomNameCookie, nullptr);
        if (!reply)
            return nullopt;
        auto const atomName = reply->atom;

        return XcbPropertyInfo { xcbConnection, winId, atomName };
    }

    void setPropertyX11(QWindow* window, string const& name, uint32_t value)
    {
        if (auto infoOpt = queryXcbPropertyInfo(window, name))
        {
            xcb_change_property(infoOpt.value().connection,
                                XCB_PROP_MODE_REPLACE,
                                infoOpt.value().window,
                                infoOpt.value().propertyAtom,
                                XCB_ATOM_CARDINAL,
                                32,
                                1,
                                &value);
            xcb_flush(infoOpt.value().connection);
        }
        else
            errorLog()(R"(Could not set X11 property info for "{}" to {}.)", name, value);
    }

    void setPropertyX11(QWindow* window, string const& name, string const& value)
    {
        if (auto infoOpt = queryXcbPropertyInfo(window, name))
        {
            xcb_change_property(infoOpt.value().connection,
                                XCB_PROP_MODE_REPLACE,
                                infoOpt.value().window,
                                infoOpt.value().propertyAtom,
                                XCB_ATOM_STRING,
                                8,
                                static_cast<uint32_t>(value.size()),
                                value.data());

            xcb_flush(infoOpt.value().connection);
        }
        else
            errorLog()(R"(Could not set X11 property info for "{}" to "{}".)", name, value);
    }

    void unsetPropertyX11(QWindow* window, string const& name)
    {
        if (auto infoOpt = queryXcbPropertyInfo(window, name))
        {
            xcb_delete_property(
                infoOpt.value().connection, infoOpt.value().window, infoOpt.value().propertyAtom);
        }
    }

} // namespace
#endif

void setEnabled(QWindow* window, bool enable, QRegion const& region)
{
    crispy::ignore_unused(region);

#if !defined(Q_OS_WINDOWS) && !defined(Q_OS_DARWIN)
    // This #if should catch UNIX in general but not Mac, so we have not just Linux but also the BSDs and
    // maybe others if one wants to.
    //
    // I was looking into the kwin source code and it's all in fact just a one-liner, so easy to get rid of
    // the dependency and still support nice looking semi transparent blurred backgrounds.
    if (enable)
    {
        setPropertyX11(window, "_KDE_NET_WM_BLUR_BEHIND_REGION", uint32_t { 0 });
        setPropertyX11(window, "_MUTTER_HINTS", "blur-provider=sigma:60,brightness:0.9");
    }
    else
    {
        unsetPropertyX11(window, "kwin_blur");
        unsetPropertyX11(window, "_MUTTER_HINTS");
    }
#elif defined(_WIN32) // {{{
    // Awesome hack with the noteworty links:
    // * https://gist.github.com/ethanhs/0e157e4003812e99bf5bc7cb6f73459f (used as code template)
    // * https://github.com/riverar/sample-win32-acrylicblur/blob/master/MainWindow.xaml.cs
    // * https://stackoverflow.com/questions/44000217/mimicking-acrylic-in-a-win32-app
    // p.s.: if you find a more official way to do it, please PR me. :)

    if (HWND hwnd = (HWND) window->winId(); hwnd != nullptr)
    {
        const HINSTANCE hModule = LoadLibrary(TEXT("user32.dll"));
        if (hModule)
        {
            enum WindowCompositionAttribute : int
            {
                // ...
                WCA_ACCENT_POLICY = 19,
                // ...
            };
            enum AcceptState : int
            {
                ACCENT_DISABLED = 0,
                ACCENT_ENABLE_GRADIENT = 1,
                ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
                ACCENT_ENABLE_BLURBEHIND = 3,
                ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
                ACCENT_ENABLE_HOSTBACKDROP = 5,
            };
            struct ACCENTPOLICY
            {
                AcceptState nAccentState;
                int nFlags;
                int nColor;
                int nAnimationId;
            };
            struct WINCOMPATTRDATA
            {
                WindowCompositionAttribute nAttribute;
                void const* pData;
                ULONG ulDataSize;
            };
            typedef BOOL(WINAPI * pSetWindowCompositionAttribute)(HWND, WINCOMPATTRDATA const*);
            const pSetWindowCompositionAttribute SetWindowCompositionAttribute =
                (pSetWindowCompositionAttribute) GetProcAddress(hModule, "SetWindowCompositionAttribute");
            if (SetWindowCompositionAttribute)
            {
                auto const policy = enable ? ACCENTPOLICY { ACCENT_ENABLE_BLURBEHIND, 0, 0, 0 }
                                           : ACCENTPOLICY { ACCENT_DISABLED, 0, 0, 0 };
                auto const data = WINCOMPATTRDATA { WCA_ACCENT_POLICY, &policy, sizeof(ACCENTPOLICY) };
                BOOL rs = SetWindowCompositionAttribute(hwnd, &data);
                if (!rs)
                    qDebug() << "SetWindowCompositionAttribute" << rs;
            }
            FreeLibrary(hModule);
        }
    }
    // }}}
#else
    // Get me working on other platforms/compositors (such as OSX, Gnome, ...), please.
    crispy::ignore_unused(window, enable);
#endif
}

} // namespace BlurBehind
