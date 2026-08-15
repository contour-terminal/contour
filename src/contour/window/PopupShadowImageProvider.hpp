// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/PopupShadow.hpp>

#include <QtQuick/QQuickImageProvider>

#include <map>

namespace contour::window
{

/// Serves the popups' drop shadow to QML as a nine-patch image.
///
/// QML reaches it as `image://popupshadow/<blur>/<offsetY>/<cornerRadius>/<#aarrggbb>`, which is
/// how the parameters travel: an image provider is addressed by URL, and encoding them there is
/// what lets one provider answer for both chrome styles and for whatever colour the palette
/// currently gives, without the provider knowing anything about either.
///
/// The results are cached by those parameters. There are at most a handful of distinct shadows in a
/// session -- one per style, times a light and a dark palette -- while there are many popups, and
/// the blur costs a few hundred microseconds.
class PopupShadowImageProvider final: public QQuickImageProvider
{
  public:
    PopupShadowImageProvider(): QQuickImageProvider(QQuickImageProvider::Image) {}

    /// @param id            The parameters, slash-separated, as described above.
    /// @param size          Out: the image's natural size, which QML needs before it loads.
    /// @param requestedSize Ignored: a nine-patch must not be scaled, or its corners would be too.
    /// @return The shadow, or a null image when @p id does not parse.
    QImage requestImage(QString const& id, QSize* size, QSize const& requestedSize) override;

    /// The unstretched corner width for the shadow @p id names, i.e. BorderImage's `border`.
    ///
    /// QML needs it alongside the image and cannot read it out of one, so it is computed from the
    /// same parameters rather than guessed. @see borderFor in the QML.
    [[nodiscard]] static int cornerFor(platform::PopupShadowParams const& params);

  private:
    std::map<QString, platform::PopupShadowImage> _cache;
};

} // namespace contour::window
