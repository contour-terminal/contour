// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/display/GpuInventory.hpp>

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace contour::window
{

/// The editable bridge between the GUI settings page and the configuration.
///
/// It follows Windows Terminal's "edit a clone, then Save" model: the controller never mutates the
/// live @ref Config, it edits an in-memory *draft* of one profile (or one color scheme) and
/// persists that draft to a GUI-owned side file on Save, then asks the host to reload so the change
/// takes effect. It NEVER writes the hand-maintained `contour.yml`; profiles and schemes defined there
/// are surfaced read-only (see @ref SettingsOrigin), and only the GUI's own side-file entities
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

    /// How links are opened, spelled for the page: "Ctrl+Click (Shift+Ctrl+Click inside applications that use
    /// the mouse)". It comes from input_mapping, not from a profile.
    Q_PROPERTY(QString linkGesture READ linkGesture NOTIFY changed)

    /// The profiles the page lists: one QVariantMap per profile
    /// { name, origin ("main"|"side"|"builtin"), editable (bool), isDefault (bool) }.
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY changed)

    /// The color schemes the page lists: one QVariantMap per scheme { name, editable (bool) }.
    Q_PROPERTY(QVariantList colorSchemes READ colorSchemes NOTIFY changed)

    /// The name of the current default profile (settings.yml override, else contour.yml).
    Q_PROPERTY(QString defaultProfile READ defaultProfile NOTIFY changed)

    /// The key/mouse bindings, read-only: the configured ones, then each enabled built-in fallback that no
    /// configured binding shadows. One QVariantMap per binding { trigger, action (every action it runs,
    /// comma-separated), mode, builtIn }. Editing bindings stays in contour.yml for now (this is a viewer).
    Q_PROPERTY(QVariantList keybindings READ keybindings NOTIFY changed)

    /// The editable global (application-scope) settings: one QVariantMap per field
    /// { key, label, help, type, value, options, overridden }. Edits are written to settings.yml as
    /// overrides on contour.yml — the same side-file discipline the rest of the page uses. A dotted
    /// key ("gutter.line_numbers") is a path to one leaf of a contour.yml section, and overrides only
    /// that leaf. A value the loader would reject is refused with errorOccurred and not written. `overridden`
    /// says that settings.yml holds an override of the key and that it took.
    Q_PROPERTY(QVariantList globalFields READ globalFields NOTIFY changed)

    /// The scalar fields of the profile currently being edited: one QVariantMap per field
    /// { key, label, help, type ("bool"|"int"|"double"|"string"), value }. Empty when nothing is open.
    Q_PROPERTY(QVariantList profileFields READ profileFields NOTIFY draftChanged)

    /// The link patterns of the profile draft: the built-ins first, in table order, then the profile's own,
    /// in its order. One QVariantMap per pattern { name, regex, clickable, url, builtin, kind
    /// ("url"|"path"|"custom"), problem }. `clickable` and `builtin` are QML-facing bools (the page reads
    /// them as such); `problem` says what the reader or the pattern compiler finds wrong, empty when nothing
    /// is. A built-in's row shows the regex in effect, which the page does not edit. Empty when nothing is
    /// open.
    Q_PROPERTY(QVariantList linkPatterns READ linkPatterns NOTIFY draftChanged)

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
    /// { key, label, color ("#rrggbb"), colorSource }. `colorSource` says where the colour shown comes from:
    /// "scheme" for a slot every scheme sets, "derived" for one the scheme leaves unset so that it follows
    /// the colours it derives from, and "pinned" for such a slot the scheme sets after all. Empty when no
    /// scheme is open for editing.
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
    /// @param gpuInventory The machine's GPUs for the "Rendering GPU" row; null lists none.
    /// @param appearance The appearance (light or dark) the terminals are drawn in now: a light/dark
    ///                   profile's preview takes the scheme of this mode, as the terminal does.
    /// @param parent Optional QObject parent.
    SettingsController(ConfigAccessor config,
                       std::shared_ptr<config::GuiConfigStore> store,
                       ApplyCallback apply,
                       std::shared_ptr<display::IGpuInventory const> gpuInventory,
                       vtbackend::ColorPreference appearance,
                       QObject* parent = nullptr);

    /// Follows a change of the OS appearance, as the sessions do (TerminalSessionManager::
    /// updateColorPreference). Live state driven from outside, not configuration: the light/dark switch
    /// happens while the page is open, and its previews must repaint. Emits draftChanged on a change.
    /// @param appearance The appearance the terminals are drawn in from now on.
    void setColorPreference(vtbackend::ColorPreference appearance);

    [[nodiscard]] bool locked() const noexcept { return _locked; }
    [[nodiscard]] QString linkGesture() const { return _linkGesture; }
    [[nodiscard]] QVariantList profiles() const { return _profiles; }
    [[nodiscard]] QVariantList colorSchemes() const { return _colorSchemes; }
    [[nodiscard]] QString defaultProfile() const { return _defaultProfile; }
    [[nodiscard]] QVariantList keybindings() const { return _keybindings; }
    [[nodiscard]] QVariantList globalFields() const;

    [[nodiscard]] QVariantList profileFields() const;
    [[nodiscard]] QVariantList linkPatterns() const;
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

    // {{{ Link patterns of the profile draft
    /// Appends a pattern of the profile's own, clickable, under a name no pattern has; its regex is still to
    /// be given. A no-op without a draft or on a read-only one.
    Q_INVOKABLE void addLinkPattern();
    /// Edits row @p index of @ref linkPatterns. A built-in's row takes `clickable` only, kept in the draft
    /// as a name-only override, which goes again once it holds nothing the built-in does not; a user row
    /// takes `name`, `regex`, `clickable` and `url` (empty: none). A name another pattern has is refused
    /// with errorOccurred. A no-op without a draft, on a read-only one, or for an index out of range.
    /// @param index The row.
    /// @param row   The row as @ref linkPatterns spells it, with the keys edited.
    Q_INVOKABLE void setLinkPattern(int index, QVariantMap const& row);
    /// Removes row @p index of @ref linkPatterns, a pattern of the profile's own; a built-in's row stays.
    /// A no-op without a draft, on a read-only one, or for an index out of range.
    /// @param index The row.
    Q_INVOKABLE void removeLinkPattern(int index);
    /// What @p row, as @ref linkPatterns spells it, finds in @p sample: through the validation and the
    /// matcher the terminal uses, without the disk, so a path is shown whether or not it exists.
    /// @param row    The pattern, edited or not.
    /// @param sample The text to match.
    /// @return { spans: [{ start, end, target }] with UTF-16 offsets into @p sample, problem, note }; the
    ///         note says, for a path pattern, when the terminal shows what it finds.
    [[nodiscard]] Q_INVOKABLE QVariantMap previewLinkPattern(QVariantMap const& row,
                                                             QString const& sample) const;
    // }}}

    /// Sets the default profile (persisted to settings.yml, overriding contour.yml). @return success.
    Q_INVOKABLE bool setDefaultProfile(QString const& name);

    /// Parses an indicator status line template into the item list the visual editor edits.
    ///
    /// Each returned map holds: `type` (the template's placeholder name), `label` and `sample` from
    /// vtbackend::StatusLineDefinitions::ItemTraits, `displayName` (the label, or the item's own text
    /// where it has some), `hasColor`/`color` and `hasBgColor`/`bgColor`, `textLeft`/`textRight`, and
    /// `flags` -- a map from each template flag name to whether it is set. A colour is `"#RRGGBB"` or a
    /// role's name (@ref indicatorColorRoles); `colorPreview`/`bgColorPreview` give what it paints as,
    /// `"#rrggbb"` with a role resolved against the draft profile's scheme, empty while unset. Items
    /// carrying more than styles add their own keys: `text`, `command`, `interval`, `name`, Tabs'
    /// `hasActiveColor`/`activeColor`, `hasActiveBackground`/`activeBackground` and `separator`, Context's
    /// `verbosity` and `separator`, InputMode's `hide` (a mode's name, or empty) and `chip` (whether it
    /// draws as a chip), and `maxWidth` on every item with a column budget.
    ///
    /// Everything the template can express is present, so @ref serializeIndicatorSegment can put all of
    /// it back: a segment that goes through this pair unedited comes out unchanged.
    [[nodiscard]] Q_INVOKABLE QVariantList parseIndicatorSegment(QString const& templateStr) const;

    /// Serializes an item list (as returned by @ref parseIndicatorSegment) back to a template string.
    /// An item whose `type` names no known placeholder is skipped rather than guessed at.
    [[nodiscard]] Q_INVOKABLE QString serializeIndicatorSegment(QVariantList const& items) const;

    /// Every placeholder the indicator editor can offer, as `{ type, label, sample }`.
    /// Read from vtbackend, so the picker cannot offer a placeholder the parser rejects -- nor omit one
    /// it accepts.
    [[nodiscard]] Q_INVOKABLE QVariantList indicatorPlaceholders() const;

    /// The colour roles an indicator item's colour may name instead of a hex value, in
    /// vtbackend::StatusLineDefinitions::ColorRoleNames' order.
    /// @return The roles' names as a template spells them.
    [[nodiscard]] Q_INVOKABLE QStringList indicatorColorRoles() const;

    /// Every cell flag an indicator item can carry, as `{ key, label }`. `key` indexes the `flags` map
    /// of @ref parseIndicatorSegment.
    [[nodiscard]] Q_INVOKABLE QVariantList indicatorFlags() const;

    /// The indicator template for segment @p segmentIndex (0=left, 1=middle, 2=right) of the current
    /// profile draft; empty when there is no draft or the index is out of range.
    [[nodiscard]] Q_INVOKABLE QString indicatorSegment(int segmentIndex) const;

    /// Sets the indicator template for segment @p segmentIndex on the current profile draft.
    /// A no-op when there is no draft, the profile is read-only, or the index is out of range.
    Q_INVOKABLE void setIndicatorSegment(int segmentIndex, QString const& value);

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
    /// A slot whose colour is derived is pinned by any colour committed to it, the one it shows too.
    Q_INVOKABLE void setSchemeColor(QString const& key, QString const& color);
    /// Lets a pinned slot of the scheme draft follow the colours it derives from again. A no-op for a slot
    /// the scheme always sets, such as the background.
    /// @param key The slot (see @ref schemeColors for keys).
    Q_INVOKABLE void resetSchemeColor(QString const& key);
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
    /// What the draft's link patterns have wrong (one the reader would skip, or one of the profile's own the
    /// terminal would disable), so that a save refuses what the reader would discard. A field with a
    /// problem (a descriptor's `validate`) is no reason: the reader keeps it, and its row shows the problem.
    /// @return The first link pattern's problem; nullopt when none has one.
    [[nodiscard]] std::optional<std::string> draftProblem() const;
    /// The palette the scheme named @p name stands for: a `color_schemes:` entry of contour.yml, a GUI side
    /// file, or a built-in scheme, in that order; the default palette for a name none answers to.
    [[nodiscard]] vtbackend::ColorPalette schemePalette(std::string const& name) const;
    /// The palette the draft profile's status-line roles preview against: its scheme, or for a light/dark
    /// pair the one of the current appearance -- by name, so a scheme switched in the draft previews before
    /// it is saved. The default palette while no profile is open.
    [[nodiscard]] vtbackend::ColorPalette previewPalette() const;

    ConfigAccessor _config;
    std::shared_ptr<config::GuiConfigStore> _store;
    ApplyCallback _apply;
    std::shared_ptr<display::IGpuInventory const> _gpuInventory;
    vtbackend::ColorPreference _appearance;   ///< Which scheme of a light/dark profile the previews use.
    std::vector<display::GpuCandidate> _gpus; ///< Listed by refresh(), not on every property read.

    bool _locked = false;
    QString _linkGesture; ///< Listed by refresh(): how links are opened.
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

} // namespace contour::window
