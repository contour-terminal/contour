/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "BackgroundBlur.h"

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
    #include <KWindowEffects>

    #include <kwindowsystem_version.h>
    #define KDE_MAKE_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#endif

#include <QtCore/QDebug>
#include <QtGui/QWindow>

#if defined(_WIN32)
    #include <Windows.h>
#endif

namespace BlurBehind
{
void setEnabled(QWindow* window, bool enable)
{
#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
    #if KWINDOWSYSTEM_VERSION >= KDE_MAKE_VERSION(5, 82, 0)
    KWindowEffects::enableBlurBehind(window, enable);
    KWindowEffects::enableBackgroundContrast(window, enable);
    #else
    KWindowEffects::enableBlurBehind(window->winId(), enable);
    KWindowEffects::enableBackgroundContrast(window->winId(), enable);
    #endif
#elif defined(_WIN32)
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
#else
    // Get me working on other platforms/compositors (such as OSX, Gnome, ...), please.
    (void) window;
    (void) enable;
#endif
}

} // namespace BlurBehind
