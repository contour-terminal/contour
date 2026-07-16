// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>
#include <contour/GuiConfigStore.h>

#include <QtCore/QObject>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <string>

namespace contour
{

/// The editable bridge between the GUI settings page and the configuration.
///
/// It follows Windows Terminal's "edit a clone, then Save" model: the controller never mutates the
/// live @ref config::Config, it edits an in-memory *draft* of one profile (or one color scheme) and
/// persists that draft to a GUI-owned side file on Save, then asks the host to reload so the change
/// takes effect. It NEVER writes the hand-maintained `contour.yml`; profiles and schemes defined there
/// are surfaced read-only (see @ref config::SettingsOrigin), and only the GUI's own side-file entities
/// are editable.
///
/// Every collaborator it touches is injected (per the project's DI principle), so the whole
/// create/edit/save/delete workflow is driven headlessly in tests against an in-memory store:
///   - a *config accessor* returning the current, already-merged configuration (re-read on refresh),
///   - a @ref GuiConfigStore for the actual side-file writes/deletes,
///   - an *apply* callback the host wires to a live config reload.
class SettingsController: public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the WindowController")

    /// True when `gui_config_locked` is set in contour.yml: the page opens read-only and no Save,
    /// Save-As or Delete is offered.
    Q_PROPERTY(bool locked READ locked NOTIFY changed)

    /// The profiles the page lists: one QVariantMap per profile
    /// { name, origin ("main"|"side"|"builtin"), editable (bool), isDefault (bool) }.
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY changed)

    /// The color schemes the page lists: one QVariantMap per scheme { name, editable (bool) }.
    Q_PROPERTY(QVariantList colorSchemes READ colorSchemes NOTIFY changed)

    /// The name of the current default profile (settings.yml override, else contour.yml).
    Q_PROPERTY(QString defaultProfile READ defaultProfile NOTIFY changed)

    /// The configured key/mouse bindings, read-only: one QVariantMap per binding
    /// { trigger, action, mode }. Editing bindings stays in contour.yml for now (this is a viewer).
    Q_PROPERTY(QVariantList keybindings READ keybindings NOTIFY changed)

    /// The editable global (application-scope) settings: one QVariantMap per field
    /// { key, label, help, type, value, options, overridden }. Edits are written to settings.yml as
    /// overrides on contour.yml — the same side-file discipline the rest of the page uses.
    Q_PROPERTY(QVariantList globalFields READ globalFields NOTIFY changed)

    /// The scalar fields of the profile currently being edited: one QVariantMap per field
    /// { key, label, help, type ("bool"|"int"|"double"|"string"), value }. Empty when nothing is open.
    Q_PROPERTY(QVariantList profileFields READ profileFields NOTIFY draftChanged)

    /// The name of the profile draft being edited ("" for an unsaved new profile).
    Q_PROPERTY(QString editingProfile READ editingProfile NOTIFY draftChanged)

    /// True when the open profile draft cannot be saved in place (it is a contour.yml/builtin profile,
    /// or the page is locked): the UI offers "Save As" / "Duplicate" instead of "Save".
    Q_PROPERTY(bool editingReadOnly READ editingReadOnly NOTIFY draftChanged)

    /// The color-scheme selection of the profile draft: "simple" (one scheme) or "dual" (light+dark).
    Q_PROPERTY(QString colorSchemeMode READ colorSchemeMode NOTIFY draftChanged)
    /// The scheme name for "simple" mode.
    Q_PROPERTY(QString colorScheme READ colorScheme NOTIFY draftChanged)
    /// The light/dark scheme names for "dual" mode.
    Q_PROPERTY(QString colorSchemeLight READ colorSchemeLight NOTIFY draftChanged)
    Q_PROPERTY(QString colorSchemeDark READ colorSchemeDark NOTIFY draftChanged)

    /// The colors of the color-scheme draft being edited: one QVariantMap per slot
    /// { key, label, color ("#rrggbb") }. Empty when no scheme is open for editing.
    Q_PROPERTY(QVariantList schemeColors READ schemeColors NOTIFY schemeDraftChanged)
    /// The name of the color-scheme draft being edited ("" for an unsaved new scheme).
    Q_PROPERTY(QString editingScheme READ editingScheme NOTIFY editingSchemeChanged)

  public:
    /// Returns the current, already-side-file-merged configuration. Called on every refresh, so a
    /// reload is reflected the next time the page is opened or refreshed.
    using ConfigAccessor = std::function<config::Config const&()>;

    /// Applies persisted changes to the running program (the host wires this to a live config reload).
    using ApplyCallback = std::function<void()>;

    /// @param config Accessor for the live configuration (read-only; edits go through @p store).
    /// @param store  Where side files are written/removed (injected for testability).
    /// @param apply  Invoked after a successful save/delete so the change takes effect immediately.
    /// @param parent Optional QObject parent.
    SettingsController(ConfigAccessor config,
                       std::shared_ptr<GuiConfigStore> store,
                       ApplyCallback apply,
                       QObject* parent = nullptr);

    [[nodiscard]] bool locked() const noexcept { return _locked; }
    [[nodiscard]] QVariantList profiles() const { return _profiles; }
    [[nodiscard]] QVariantList colorSchemes() const { return _colorSchemes; }
    [[nodiscard]] QString defaultProfile() const { return _defaultProfile; }
    [[nodiscard]] QVariantList keybindings() const { return _keybindings; }
    [[nodiscard]] QVariantList globalFields() const;

    [[nodiscard]] QVariantList profileFields() const;
    [[nodiscard]] QString editingProfile() const { return _editingProfile; }
    [[nodiscard]] bool editingReadOnly() const noexcept { return _editingReadOnly; }
    [[nodiscard]] QString colorSchemeMode() const;
    [[nodiscard]] QString colorScheme() const;
    [[nodiscard]] QString colorSchemeLight() const;
    [[nodiscard]] QString colorSchemeDark() const;

    [[nodiscard]] QVariantList schemeColors() const;
    [[nodiscard]] QString editingScheme() const { return _editingScheme; }

    /// Rebuilds the list models (profiles, color schemes, default) from the current configuration.
    /// Called by the host whenever the settings page is shown, so stale rows never linger.
    Q_INVOKABLE void refresh();

    // {{{ Profile draft
    /// Opens @p name for editing (loads its values into the draft). Read-only if it is not a side file.
    Q_INVOKABLE void editProfile(QString const& name);
    /// Starts a new profile draft seeded from @p basedOn (or the default profile if empty/unknown).
    Q_INVOKABLE void newProfile(QString const& basedOn);
    /// Sets a scalar profile field on the draft (see @ref profileFields for keys/types).
    Q_INVOKABLE void setProfileField(QString const& key, QVariant const& value);
    /// Switches the draft's color selection between "simple" and "dual".
    Q_INVOKABLE void setColorSchemeMode(QString const& mode);
    /// Sets the simple-mode scheme name / the dual-mode light/dark scheme names on the draft.
    Q_INVOKABLE void setColorScheme(QString const& name);
    Q_INVOKABLE void setColorSchemeLight(QString const& name);
    Q_INVOKABLE void setColorSchemeDark(QString const& name);
    /// Persists the draft to its own side file (its current name). @return true on success.
    Q_INVOKABLE bool saveProfile();
    /// Persists the draft under @p newName (a new side file), then continues editing it.
    /// @return true on success.
    Q_INVOKABLE bool saveProfileAs(QString const& newName);
    /// Removes profile @p name's side file. @return true on success.
    Q_INVOKABLE bool deleteProfile(QString const& name);
    /// Renames a GUI-created profile's side file from @p oldName to @p newName, moving the default-profile
    /// pointer and the open draft with it. Only side-file profiles can be renamed. @return success.
    Q_INVOKABLE bool renameProfile(QString const& oldName, QString const& newName);
    // }}}

    /// Sets the default profile (persisted to settings.yml, overriding contour.yml). @return success.
    Q_INVOKABLE bool setDefaultProfile(QString const& name);

    /// Overrides a global setting (persisted to settings.yml over contour.yml). @return success.
    Q_INVOKABLE bool setGlobalField(QString const& key, QVariant const& value);
    /// Clears a global override, so the setting falls back to contour.yml/default. @return success.
    Q_INVOKABLE bool resetGlobalField(QString const& key);

    // {{{ Color-scheme draft
    /// Opens color scheme @p name for editing (loads its colors into the scheme draft).
    Q_INVOKABLE void editColorScheme(QString const& name);
    /// Starts a new color-scheme draft seeded from @p basedOn (or a default palette).
    Q_INVOKABLE void newColorScheme(QString const& basedOn);
    /// Sets one color slot on the scheme draft (see @ref schemeColors for keys). @p color is "#rrggbb".
    Q_INVOKABLE void setSchemeColor(QString const& key, QString const& color);
    /// Persists the scheme draft under @p name (a `colorschemes/<name>.yml` side file). @return success.
    Q_INVOKABLE bool saveColorScheme(QString const& name);
    /// Removes color scheme @p name's side file. @return true on success.
    Q_INVOKABLE bool deleteColorScheme(QString const& name);
    /// Renames a GUI-created color scheme's side file from @p oldName to @p newName, moving the open
    /// scheme draft with it. Only side-file schemes can be renamed. @return success.
    Q_INVOKABLE bool renameColorScheme(QString const& oldName, QString const& newName);
    // }}}

  signals:
    /// The list models (profiles, color schemes, default profile, locked) changed.
    void changed();
    /// The profile draft changed (opened, a field edited, saved).
    void draftChanged();
    /// The color-scheme draft changed.
    void schemeDraftChanged();
    /// The IDENTITY of the color scheme being edited changed (a different scheme selected, a new one
    /// started, or the current one saved/deleted) — but NOT a mere color tweak. Kept separate from
    /// @ref schemeDraftChanged so the editor's name field can re-seed on an identity change without being
    /// wiped every time the user adjusts a color while typing a new name.
    void editingSchemeChanged();
    /// A user-facing error occurred (e.g. a write failed); @p message is human-readable.
    void errorOccurred(QString const& message);

  private:
    /// The origin of @p name among the current profiles, defaulting to Builtin if unknown.
    [[nodiscard]] config::SettingsOrigin profileOrigin(std::string const& name) const;
    /// Whether a GUI-owned side file @c colorschemes/<name>.yml exists on disk. This is the editability
    /// ground truth the color-scheme list uses: only such schemes can be edited, renamed or deleted.
    /// @param name The color scheme name. @return true if the side file exists.
    [[nodiscard]] bool colorSchemeSideFileExists(std::string const& name) const;
    /// Whether @p name is a color scheme declared inline in contour.yml with no GUI side file backing it.
    /// Such a name would shadow a @c colorschemes/<name>.yml side file at load time (the inline node wins),
    /// so the GUI must refuse to write one. @param name The color scheme name. @return true if inline.
    [[nodiscard]] bool isInlineColorScheme(std::string const& name) const;
    /// Reports @p error to the UI and returns false, for the `return fail(...)` idiom on write errors.
    bool fail(std::string const& error);

    ConfigAccessor _config;
    std::shared_ptr<GuiConfigStore> _store;
    ApplyCallback _apply;

    bool _locked = false;
    QVariantList _profiles;
    QVariantList _colorSchemes;
    QVariantList _keybindings;
    QString _defaultProfile;

    // The profile draft (Windows-Terminal "edit a clone").
    config::TerminalProfile _draft;
    QString _editingProfile; //!< "" while editing an unsaved new profile.
    bool _hasDraft = false;
    bool _editingReadOnly = false;

    // The color-scheme draft.
    vtbackend::ColorPalette _schemeDraft;
    QString _editingScheme; //!< "" while editing an unsaved new scheme.
    bool _hasSchemeDraft = false;
};

} // namespace contour
