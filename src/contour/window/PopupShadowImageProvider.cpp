// SPDX-License-Identifier: Apache-2.0
#include <contour/window/PopupShadowImageProvider.hpp>

#include <QtCore/QStringList>

namespace contour::window
{

namespace
{
    /// Parses `<blur>/<offsetY>/<cornerRadius>/<#aarrggbb>` back into parameters.
    [[nodiscard]] std::optional<platform::PopupShadowParams> parse(QString const& id)
    {
        auto const fields = id.split(u'/');
        if (fields.size() != 4)
            return std::nullopt;

        auto blurOk = false;
        auto offsetOk = false;
        auto radiusOk = false;
        auto const params = platform::PopupShadowParams {
            .blur = fields[0].toInt(&blurOk),
            .offsetY = fields[1].toInt(&offsetOk),
            .cornerRadius = fields[2].toInt(&radiusOk),
            .color = QColor::fromString(u"#" + fields[3]),
        };
        if (!blurOk || !offsetOk || !radiusOk || !params.color.isValid())
            return std::nullopt;

        return params;
    }
} // namespace

int PopupShadowImageProvider::cornerFor(platform::PopupShadowParams const& params)
{
    return renderPopupShadow(params).corner;
}

QImage PopupShadowImageProvider::requestImage(QString const& id, QSize* size, QSize const& /*requestedSize*/)
{
    auto const cached = _cache.find(id);
    if (cached == _cache.end())
    {
        auto const params = parse(id);
        if (!params)
            return {};
        _cache.emplace(id, platform::renderPopupShadow(*params));
    }

    auto const& shadow = _cache.at(id);
    if (size != nullptr)
        *size = shadow.image.size();
    return shadow.image;
}

} // namespace contour::window
