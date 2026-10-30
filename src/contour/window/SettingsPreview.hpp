// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigFwd.hpp>
#include <contour/session/TerminalSession.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace contour::window
{

class SettingsController;

/// The terminal the Settings window previews the profile being edited in: a session over a canned
/// transcript that follows the controller's drafts, so every change shows here before it is written.
///
/// It owns the session, which is in no window and no layout. Its session is created on the first show() and
/// destroyed with it.
class SettingsPreview final: public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("contour/session/TerminalSession.hpp")
    Q_PROPERTY(contour::session::TerminalSession* session READ session NOTIFY sessionChanged)
    /// "Live preview — {family} {size} · {scheme}" of the previewed profile.
    Q_PROPERTY(QString caption READ caption NOTIFY captionChanged)

  public:
    /// Makes the session the preview shows: the configuration to preview and the profile to run under.
    using SessionFactory =
        std::function<std::unique_ptr<session::TerminalSession>(config::Config const&, std::string const&)>;

    /// @param controller The controller whose drafts the preview follows; borrowed, outlives this.
    /// @param factory    Makes the preview session (TerminalSessionManager::createPreviewSession).
    /// @param parent     Qt ownership.
    SettingsPreview(SettingsController& controller, SessionFactory factory, QObject* parent = nullptr);
    ~SettingsPreview() override;

    SettingsPreview(SettingsPreview const&) = delete;
    SettingsPreview& operator=(SettingsPreview const&) = delete;
    SettingsPreview(SettingsPreview&&) = delete;
    SettingsPreview& operator=(SettingsPreview&&) = delete;

    /// @return The preview session; null before the first show().
    [[nodiscard]] session::TerminalSession* session() const noexcept { return _session.get(); }

    /// @return The caption of the profile shown; empty before the first show().
    [[nodiscard]] QString caption() const { return _caption; }

    /// Shows @p profileName as the controller's drafts have it, and keeps it following them.
    /// @param profileName The profile to preview.
    Q_INVOKABLE void show(QString const& profileName);

    /// show() with the indicator status line forced on, whatever the profile says.
    /// @param profileName The profile to preview.
    Q_INVOKABLE void showStatusLine(QString const& profileName);

  signals:
    /// session() changed: the session was created.
    void sessionChanged();
    /// caption() changed.
    void captionChanged();

  private:
    /// Whether the previewed status line is the profile's own or forced to the indicator.
    enum class StatusLine : uint8_t
    {
        AsConfigured = 0,
        Indicator,
    };

    /// Applies the controller's drafts to the session, creating it on the first call.
    void apply();
    void showWith(QString const& profileName, StatusLine statusLine);

    SettingsController& _controller;
    SessionFactory _factory;
    std::unique_ptr<session::TerminalSession> _session;
    std::string _profileName;
    StatusLine _statusLine = StatusLine::AsConfigured;
    QString _caption;
};

} // namespace contour::window
