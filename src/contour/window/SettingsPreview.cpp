// SPDX-License-Identifier: Apache-2.0
#include <contour/session/PreviewTranscript.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsPreview.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <utility>

namespace contour::window
{

SettingsPreview::SettingsPreview(SettingsController& controller, SessionFactory factory, QObject* parent):
    QObject(parent), _controller { controller }, _factory { std::move(factory) }
{
    // A draft edit shows before it is staged; a reload shows what was written.
    connect(&_controller, &SettingsController::draftChanged, this, &SettingsPreview::apply);
    connect(&_controller, &SettingsController::schemeDraftChanged, this, &SettingsPreview::apply);
    connect(&_controller, &SettingsController::changed, this, &SettingsPreview::apply);
}

SettingsPreview::~SettingsPreview() = default;

void SettingsPreview::show(QString const& profileName)
{
    showWith(profileName, StatusLine::AsConfigured);
}

void SettingsPreview::showStatusLine(QString const& profileName)
{
    showWith(profileName, StatusLine::Indicator);
}

void SettingsPreview::showWith(QString const& profileName, StatusLine statusLine)
{
    _profileName = profileName.toStdString();
    _statusLine = statusLine;
    apply();
}

void SettingsPreview::apply()
{
    // Before the first show() there is nothing to preview; a profile that no longer exists keeps the
    // session as it was (TerminalSession::applyReloadedConfig).
    if (_profileName.empty())
        return;
    auto preview = _controller.previewConfig();
    auto* const profile = preview.profiles.value().contains(_profileName)
                              ? &preview.profiles.value().at(_profileName)
                              : nullptr;
    if (profile == nullptr)
        return;
    if (_statusLine == StatusLine::Indicator)
        profile->statusLine.value().initialType = vtbackend::StatusDisplayType::Indicator;

    auto const caption =
        QString::fromStdString(session::previewCaption(profile->fonts.value().regular.familyName,
                                                       profile->fonts.value().size.pt,
                                                       _controller.previewSchemeNameOf(*profile)));

    if (_session)
        _session->applyReloadedConfig(preview, _profileName);
    else if (_factory)
    {
        _session = _factory(preview, _profileName);
        emit sessionChanged();
    }
    if (_caption != caption)
    {
        _caption = caption;
        emit captionChanged();
    }
}

} // namespace contour::window
