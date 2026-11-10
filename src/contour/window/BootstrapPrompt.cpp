// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/window/BootstrapPrompt.hpp>

#include <QtCore/QVariant>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>

#include <utility>

namespace contour::window
{

std::unique_ptr<BootstrapPrompt> BootstrapPrompt::create(std::shared_ptr<askpass::AskpassPromptRouter> router,
                                                         QQmlEngine& engine)
{
    // The host first: it takes the prompts that wait, and the window then shows them as it opens.
    auto prompt = std::unique_ptr<BootstrapPrompt> { new BootstrapPrompt { std::move(router) } };
    auto component = QQmlComponent(&engine, "Contour.Ui", "PromptWindow");
    auto* const created = component.createWithInitialProperties(
        { { QStringLiteral("host"), QVariant::fromValue(static_cast<QObject*>(&prompt->_host)) } });
    if (created == nullptr)
    {
        errorLog()("askpass: cannot open a window for ssh's prompts: {}",
                   component.errorString().toStdString());
        return nullptr;
    }
    // Owned by the engine, so it goes with it, before the host it shows.
    created->setParent(&engine);
    prompt->_window = created;
    return prompt;
}

BootstrapPrompt::~BootstrapPrompt()
{
    // Before the host: the window shows it.
    delete _window.data();
}

} // namespace contour::window
