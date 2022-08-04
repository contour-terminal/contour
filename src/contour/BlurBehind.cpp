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
#include "BlurBehind.h"

#include <QtCore/QDebug>
#include <QtGui/QWindow>

#if defined(_WIN32)
    #include <Windows.h>
#endif

namespace BlurBehind
{
void setEnabled(QWindow* window, bool enable, QRegion region)
{
#if !defined(__APPLE__) && !defined(_WIN32)
    // This #if should catch UNIX in general but not Mac, so we have not just Linux but also the BSDs and
    // maybe others if one wants to.
    //
    // I was looking into the kwin source code and it's all in fact just a one-liner, so easy to get rid of
    // the dependency and still support nice looking semi transparent blurred backgrounds.
    if (enable)
    {
        window->setProperty("kwin_blur", region);
        window->setProperty("kwin_background_region", region);
        window->setProperty("kwin_background_contrast", 1);
        window->setProperty("kwin_background_intensity", 1);
        window->setProperty("kwin_background_saturation", 1);
    }
    else
    {
        window->setProperty("kwin_blur", {});
        window->setProperty("kwin_background_region", {});
        window->setProperty("kwin_background_contrast", {});
        window->setProperty("kwin_background_intensity", {});
        window->setProperty("kwin_background_saturation", {});
    }
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
