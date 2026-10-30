// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ChromeStyleHost.hpp>

#include <QtQml/QQmlContext>

#include <utility>

namespace contour::window
{

namespace
{
    /// The context property every chrome QML file reads its tokens from.
    [[nodiscard]] QString chromeStylePropertyName()
    {
        return QStringLiteral("chromeStyle");
    }
} // namespace

ChromeStyleHost::ChromeStyleHost(QQmlContext& context, ChromeInputs inputs):
    _context { context }, _provider { std::make_unique<UiStyleProvider>(std::move(inputs)) }
{
    _context.setContextProperty(chromeStylePropertyName(), _provider.get());
}

ChromeStyleHost::~ChromeStyleHost() = default;

void ChromeStyleHost::update(ChromeInputs inputs)
{
    if (inputs == _provider->inputs())
        return;

    auto next = std::make_unique<UiStyleProvider>(std::move(inputs));
    _context.setContextProperty(chromeStylePropertyName(), next.get());

    // Deferred: the change that got us here may be on a stack that still reads the old provider.
    _provider.release()->deleteLater();
    _provider = std::move(next);
}

} // namespace contour::window
