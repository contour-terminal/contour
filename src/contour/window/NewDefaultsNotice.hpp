// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigFwd.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/config/NewDefaultsOffer.hpp>

#include <QtCore/QObject>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contour::window
{

/// What the new-defaults banner shows: whether to offer the newest defaults, and the two answers.
///
/// The decision is config::newDefaultsOffer() over the editions the loaded configuration's documents
/// stated (none while the GUI may not write `settings.yml`), minus the offers the user already declined
/// (config::isOfferShown()). "Use New" records the newest edition in `settings.yml`, which wins over
/// `contour.yml`, through the app's one Settings save model -- so Settings never reads its own app's write
/// as a change made outside it -- and that reloads; "Keep Mine" records the declined offer in the GUI state
/// file, so the same reason is never offered twice.
class NewDefaultsNotice: public QObject
{
    Q_OBJECT

    /// Whether the banner is shown. A plain `bool` because QML reads it as a predicate: a documented
    /// exception to the enum-class rule (QML-facing Q_PROPERTY).
    Q_PROPERTY(bool offered READ offered NOTIFY offeredChanged)

  public:
    /// Records one global override in `settings.yml` now -- written and reloaded before it returns --
    /// keeping every other key. The app's is SettingsController::writeGlobalOverride().
    /// @return Nothing, or why it was not written or did not reload.
    using RecordOverride =
        std::function<std::expected<void, std::string>(std::string_view key, std::string_view value)>;

    /// @param currentConfig The configuration in effect, read anew on every decision (a reload replaces it).
    /// @param recordOverride How "Use New" records the newest edition.
    /// @param state Where "Keep Mine" records the declined offer.
    /// @param parent The QObject parent.
    NewDefaultsNotice(std::function<config::Config const&()> currentConfig,
                      RecordOverride recordOverride,
                      std::shared_ptr<config::GuiStateStore> state,
                      QObject* parent = nullptr);

    /// @return True while there is an offer the user has not declined.
    [[nodiscard]] bool offered() const noexcept;

    /// Switches to the newest defaults: records them in `settings.yml`, which reloads.
    /// @return Nothing on success, or the write's or the reload's error, with offered() as it was. Refused
    ///         while the GUI may not write `settings.yml` (see offerFor()).
    [[nodiscard]] std::expected<void, std::string> switchToNewDefaults();

    /// The banner's action: switchToNewDefaults(), logging a failure.
    Q_INVOKABLE void useNew();

    /// The banner's dismissal: records the current offer as declined and hides the banner.
    Q_INVOKABLE void keepMine();

    /// Decides again, from the configuration in effect and the stored state. Emits offeredChanged()
    /// only when offered() changes. Called after every configuration reload.
    void refresh();

  signals:
    /// offered() changed.
    void offeredChanged();

  private:
    /// Why the GUI may not write `settings.yml` under @p loaded.
    /// @param loaded The configuration in effect.
    /// @return The reason, or nothing when it may.
    [[nodiscard]] static std::optional<std::string_view> settingsWriteBlocker(config::Config const& loaded);

    /// The offer @p loaded raises: none while the GUI may not write `settings.yml`, that is under
    /// `gui_config_locked` or over a `settings.yml` that did not parse; else config::newDefaultsOffer().
    /// @param loaded The configuration in effect.
    /// @return The offer.
    [[nodiscard]] static config::NewDefaultsOffer offerFor(config::Config const& loaded);

    /// The dismissals @p store holds.
    /// @param store The GUI state to read.
    /// @param fallback What to answer when @p store cannot be read: the dismissals known so far.
    /// @return The stored dismissals, or @p fallback, logged.
    [[nodiscard]] static std::vector<config::NewDefaultsOffer> dismissedIn(
        config::GuiStateStore const& store, std::vector<config::NewDefaultsOffer> fallback);

    /// Applies a new decision, emitting offeredChanged() when it changes offered().
    /// @param offer The offer the configuration raises.
    /// @param dismissed The offers declined.
    void apply(config::NewDefaultsOffer offer, std::vector<config::NewDefaultsOffer> dismissed);

    std::function<config::Config const&()> _config;
    RecordOverride _recordOverride;
    std::shared_ptr<config::GuiStateStore> _state;
    config::NewDefaultsOffer _offer = config::NewDefaultsOffer::None;
    std::vector<config::NewDefaultsOffer> _dismissed {};
};

} // namespace contour::window
