// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/SettingsEditSession.hpp>
#include <contour/display/GpuInventory.hpp>
#include <contour/platform/FontFamilySource.hpp>

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace contour::window
{

/// What a SettingsController reaches the outside world through, fixed at construction.
struct SettingsControllerServices
{
    /// Where the GUI side files are read and written.
    std::shared_ptr<config::GuiConfigStore> store {};
    /// Reloads the configuration after Settings wrote a side file. The host refreshes the controller from
    /// it (refresh()), as it does after a hand edit of contour.yml.
    std::function<std::expected<void, std::string>()> reload {};
    /// The machine's GPUs for the "Rendering GPU" row; null lists none.
    std::shared_ptr<display::IGpuInventory const> gpuInventory {};
    /// The monospace families the font picker lists; null lists the bundled one alone
    /// (platform::NullFontFamilySource).
    std::shared_ptr<platform::IFontFamilySource const> fontFamilies {};
    /// Runs the debounced save: a QtSaveScheduler in the app, a hand-fired one in tests.
    std::shared_ptr<config::SaveScheduler> scheduler {};
};

/// The editable bridge between the GUI settings page and the configuration.
///
/// Everything applies live: the controller edits an in-memory *draft* of one profile, one color scheme
/// and of `settings.yml`, and every change stages the draft's whole side file in one
/// config::SettingsEditSession, which writes it SettingsSaveDebounce later and reloads the configuration.
/// "Revert Changes" puts back every side file as it was when the session first touched it. It NEVER writes
/// the hand-maintained `contour.yml`; profiles and schemes defined there are surfaced read-only (see
/// @ref SettingsOrigin) and offer "Duplicate to Edit".
///
/// The drafts are re-seeded only when the user opens another entity, when a write failed, when the file
/// changed outside Settings, and on Revert; refresh() rebuilds the lists alone.
///
/// Every collaborator it touches is injected (per the project's DI principle): see
/// @ref SettingsControllerServices.
class SettingsController: public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the WindowController")

    /// True when `gui_config_locked` is set in contour.yml: the page opens read-only and Settings stages
    /// nothing.
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
    /// { key, label, help, type, group (a SettingsGroup), groupTitle, value, options, overridden }. Edits are
    /// written to settings.yml as overrides on contour.yml — the same side-file discipline the rest of the
    /// page uses. A dotted key ("gutter.line_numbers") is a path to one leaf of a contour.yml section, and
    /// overrides only that leaf. A value the loader would reject is refused with errorOccurred and not
    /// written. `overridden` says that settings.yml holds an override of the key and that it took, or that
    /// one is committed and not yet written; `value` is then the committed one.
    Q_PROPERTY(QVariantList globalFields READ globalFields NOTIFY globalFieldsChanged)

    /// The Settings sections, from SettingsSectionRows: one QVariantMap per section { id, title, icon (a
    /// SettingsIcon), groups: [{ id (a SettingsGroup), title, scope ("global"|"profile") }] }.
    Q_PROPERTY(QVariantList sections READ sections NOTIFY changed)

    /// The sidebar's rows: the sections { kind: "section", id, title, icon }, then one row per profile
    /// { kind: "profile", name, origin, editable, isDefault, tabColor ("#rrggbb", or empty for none) }, then
    /// { kind: "edit", title: "Edit contour.yml…" }.
    Q_PROPERTY(QVariantList sidebar READ sidebar NOTIFY changed)

    /// The monospace font families the font picker lists, the bundled one first; listed by refresh().
    Q_PROPERTY(QStringList fontFamilies READ fontFamilies NOTIFY changed)

    /// Whether this Settings session changed anything "Revert Changes" can put back. A QML-facing bool.
    Q_PROPERTY(bool hasChanges READ hasChanges NOTIFY changesChanged)

    /// The scalar fields of the profile currently being edited: one QVariantMap per field
    /// { key, label, help, type ("bool"|"int"|"double"|"string"), group (a SettingsGroup), groupTitle,
    /// groupGlyph, value }. Empty when nothing is open.
    Q_PROPERTY(QVariantList profileFields READ profileFields NOTIFY draftChanged)

    /// The link patterns of the profile draft: the built-ins first, in table order, then the profile's own,
    /// in its order. One QVariantMap per pattern { name, regex, clickable, url, builtin, kind
    /// ("url"|"path"|"custom"), problem }. `clickable` and `builtin` are QML-facing bools (the page reads
    /// them as such); `problem` says what the reader or the pattern compiler finds wrong, empty when nothing
    /// is. A built-in's row shows the regex in effect, which the page does not edit. Empty when nothing is
    /// open.
    Q_PROPERTY(QVariantList linkPatterns READ linkPatterns NOTIFY draftChanged)

    /// The name of the profile draft being edited ("" when none is open).
    Q_PROPERTY(QString editingProfile READ editingProfile NOTIFY draftChanged)

    /// True when the open profile draft cannot be edited (it is a contour.yml/builtin profile, or the page
    /// is locked): the UI offers "Duplicate to Edit".
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
    /// The name of the color-scheme draft being edited ("" when none is open).
    Q_PROPERTY(QString editingScheme READ editingScheme NOTIFY editingSchemeChanged)

  public:
    /// Returns the current, already-side-file-merged configuration. Called on every refresh, so a
    /// reload is reflected the next time the page is opened or refreshed.
    using ConfigAccessor = std::function<config::Config const&()>;

    /// @param config     Accessor for the live configuration (read-only; edits go through the store).
    /// @param services   The store, the reload, the GPU inventory and the save scheduler.
    /// @param appearance The appearance (light or dark) the terminals are drawn in now: a light/dark
    ///                   profile's preview takes the scheme of this mode, as the terminal does.
    /// @param parent     Optional QObject parent.
    SettingsController(ConfigAccessor config,
                       SettingsControllerServices services,
                       vtbackend::ColorPreference appearance,
                       QObject* parent = nullptr);
    ~SettingsController() override;

    SettingsController(SettingsController const&) = delete;
    SettingsController(SettingsController&&) = delete;
    SettingsController& operator=(SettingsController const&) = delete;
    SettingsController& operator=(SettingsController&&) = delete;

    /// Follows a change of the OS appearance, as the sessions do (TerminalSessionManager::
    /// updateColorPreference). Live state driven from outside, not configuration: the light/dark switch
    /// happens while the page is open, and its previews must repaint. Emits draftChanged on a change.
    /// @param appearance The appearance the terminals are drawn in from now on.
    void setColorPreference(vtbackend::ColorPreference appearance);

    [[nodiscard]] bool locked() const noexcept { return _editability == config::SettingsEditability::Locked; }
    [[nodiscard]] QString linkGesture() const { return _linkGesture; }
    [[nodiscard]] QVariantList profiles() const { return _profiles; }
    [[nodiscard]] QVariantList colorSchemes() const { return _colorSchemes; }
    [[nodiscard]] QString defaultProfile() const { return _defaultProfile; }
    [[nodiscard]] QVariantList keybindings() const { return _keybindings; }
    [[nodiscard]] QVariantList globalFields() const;

    [[nodiscard]] QVariantList profileFields() const;
    [[nodiscard]] QVariantList linkPatterns() const;
    [[nodiscard]] QString editingProfile() const { return _editingProfile; }
    [[nodiscard]] bool editingReadOnly() const noexcept { return _profileAccess == DraftAccess::ReadOnly; }
    [[nodiscard]] QString colorSchemeMode() const;
    [[nodiscard]] QString colorScheme() const;
    [[nodiscard]] QString colorSchemeLight() const;
    [[nodiscard]] QString colorSchemeDark() const;

    [[nodiscard]] QVariantList schemeColors() const;
    [[nodiscard]] QString editingScheme() const { return _editingScheme; }
    [[nodiscard]] bool hasChanges() const noexcept;
    [[nodiscard]] QVariantList sections() const;
    [[nodiscard]] QVariantList sidebar() const;
    [[nodiscard]] QStringList fontFamilies() const { return _fontFamilies; }

    /// The rows @p query finds, as matchSettings() orders them: the open profile's fields, then the globals.
    /// @param query What was typed into the search field.
    /// @return One QVariantMap per row { key, label, section (its id), group (a SettingsGroup), scope
    ///         ("global"|"profile") }; empty for a blank query.
    [[nodiscard]] Q_INVOKABLE QVariantList searchSettings(QString const& query) const;

    /// Rebuilds the list models (profiles, color schemes, default) from the current configuration, and the
    /// settings.yml draft while nothing is pending for it. Leaves the open drafts as they are. The host calls
    /// it after every configuration reload -- Settings' own and a hand edit's alike -- and when the page is
    /// shown. A change of `gui_config_locked` starts a new Settings session.
    Q_INVOKABLE void refresh();

    // {{{ Session
    /// Puts back every side file this Settings session touched as it was when first touched, then
    /// re-seeds the drafts. A failure is reported through errorOccurred.
    Q_INVOKABLE void revertChanges();
    /// Writes what is staged now rather than after the debounce: on closing Settings and on quitting.
    /// A failure is reported through errorOccurred.
    Q_INVOKABLE void flushPendingChanges();
    // }}}

    // {{{ Profile draft
    /// Opens @p name for editing (loads its values into the draft). Read-only if it is not a side file.
    Q_INVOKABLE void editProfile(QString const& name);
    /// Sets a scalar profile field on the draft (see @ref profileFields for keys/types) and stages it.
    /// @return Nothing, or why it was refused (Locked, ReadOnly, UnknownSetting, InvalidValue for a link
    ///         pattern the reader would skip) or could not be staged.
    [[nodiscard]] std::expected<void, config::SettingsError> setProfileField(QString const& key,
                                                                             QVariant const& value);
    /// Copies profile @p name into a new side file, "<name> copy" ("<name> copy 2", … when taken), and
    /// opens it: "Duplicate to Edit".
    /// @return The new profile's name, or why there is none.
    [[nodiscard]] std::expected<QString, config::SettingsError> duplicateProfile(QString const& name);
    /// Writes a new side-file profile "New Profile" ("New Profile 2", … when taken) seeded from the default
    /// profile, and opens it.
    /// @return The new profile's name, or why there is none.
    [[nodiscard]] std::expected<QString, config::SettingsError> createProfile();
    /// Removes profile @p name's side file. Side-file profiles only.
    [[nodiscard]] std::expected<void, config::SettingsError> deleteProfile(QString const& name);
    /// Renames a GUI-created profile's side file from @p oldName to @p newName, moving the default-profile
    /// pointer and the open draft with it. Side-file profiles only.
    [[nodiscard]] std::expected<void, config::SettingsError> renameProfile(QString const& oldName,
                                                                           QString const& newName);
    /// Sets the default profile (settings.yml, overriding contour.yml).
    [[nodiscard]] std::expected<void, config::SettingsError> setDefaultProfile(QString const& name);

    /// The page's commit of a profile field: setProfileField, a failure reported through errorOccurred.
    Q_INVOKABLE void commitProfileField(QString const& key, QVariant const& value);
    /// The page's "Duplicate to Edit": duplicateProfile, a failure reported through errorOccurred.
    Q_INVOKABLE void duplicate(QString const& name);
    /// The page's "+": createProfile, a failure reported through errorOccurred.
    Q_INVOKABLE void addProfile();
    /// The page's Delete: deleteProfile, a failure reported through errorOccurred.
    Q_INVOKABLE void requestDeleteProfile(QString const& name);
    /// The page's Rename: renameProfile, a failure reported through errorOccurred.
    Q_INVOKABLE void requestRenameProfile(QString const& oldName, QString const& newName);
    /// The page's Set as Default: setDefaultProfile, a failure reported through errorOccurred.
    Q_INVOKABLE void requestDefaultProfile(QString const& name);

    /// Switches the draft's color selection between "simple" and "dual", and stages it.
    Q_INVOKABLE void setColorSchemeMode(QString const& mode);
    /// Sets the simple-mode scheme name / the dual-mode light/dark scheme names on the draft, and stages it.
    Q_INVOKABLE void setColorScheme(QString const& name);
    Q_INVOKABLE void setColorSchemeLight(QString const& name);
    Q_INVOKABLE void setColorSchemeDark(QString const& name);
    // }}}

    // {{{ Link patterns of the profile draft
    /// Appends a pattern of the profile's own, clickable, under a name no pattern has; its regex is still to
    /// be given, so it is staged once it has one. A no-op without a draft or on a read-only one.
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

    /// Sets the indicator template for segment @p segmentIndex on the current profile draft, and stages it.
    /// A no-op when there is no draft, the profile is read-only, or the index is out of range.
    Q_INVOKABLE void setIndicatorSegment(int segmentIndex, QString const& value);

    /// Overrides a global setting in the settings.yml draft, and stages it.
    /// @return Nothing, or why it was refused: Locked, UnknownSetting, InvalidValue (a value the loader would
    ///         reject, refused before it is staged), NotApplied, ReadOnly (settings.yml does not parse).
    [[nodiscard]] std::expected<void, config::SettingsError> setGlobalField(QString const& key,
                                                                            QVariant const& value);
    /// Clears a global override in the settings.yml draft, so the setting falls back to contour.yml or the
    /// default, and stages it.
    [[nodiscard]] std::expected<void, config::SettingsError> resetGlobalField(QString const& key);
    /// Records the app's own override @p key = @p value in the settings.yml draft and writes it now, with
    /// whatever else is staged: the new-defaults banner's "Use New", which goes through this one save model
    /// so that Settings never reads the app's own write as a change made outside it.
    /// @param key   The settings.yml key.
    /// @param value Its YAML scalar.
    /// @return Nothing, or why it was refused (Locked, ReadOnly for an unparsable settings.yml) or did not
    ///         write or reload.
    [[nodiscard]] std::expected<void, config::SettingsError> writeGlobalOverride(std::string_view key,
                                                                                 std::string_view value);
    /// The page's commit of a global field: setGlobalField, a failure reported through errorOccurred.
    Q_INVOKABLE void commitGlobalField(QString const& key, QVariant const& value);
    /// The page's Reset of a global field: resetGlobalField, a failure reported through errorOccurred.
    Q_INVOKABLE void commitGlobalReset(QString const& key);

    // {{{ Color-scheme draft
    /// Opens color scheme @p name for editing (loads its colors into the scheme draft). Read-only unless it
    /// is a GUI side file.
    Q_INVOKABLE void editColorScheme(QString const& name);
    /// Sets one color slot on the scheme draft (see @ref schemeColors for keys), and stages it. @p color is
    /// "#rrggbb". A slot whose colour is derived is pinned by any colour committed to it, the one it shows
    /// too.
    /// @return Nothing, or why it was refused (Locked, ReadOnly, UnknownSetting) or could not be staged.
    [[nodiscard]] std::expected<void, config::SettingsError> setSchemeColor(QString const& key,
                                                                            QString const& color);
    /// The page's commit of a colour slot: setSchemeColor, a failure reported through errorOccurred.
    Q_INVOKABLE void commitSchemeColor(QString const& key, QString const& color);
    /// Lets a pinned slot of the scheme draft follow the colours it derives from again, and stages it. A
    /// no-op for a slot the scheme always sets, such as the background.
    /// @param key The slot (see @ref schemeColors for keys).
    Q_INVOKABLE void resetSchemeColor(QString const& key);
    /// Copies color scheme @p name into a new side file, "<name> copy" ("<name> copy 2", … when taken), and
    /// opens it.
    /// @return The new scheme's name, or why there is none.
    [[nodiscard]] std::expected<QString, config::SettingsError> duplicateColorScheme(QString const& name);
    /// Removes color scheme @p name's side file. Side-file schemes only.
    [[nodiscard]] std::expected<void, config::SettingsError> deleteColorScheme(QString const& name);
    /// Renames a GUI-created color scheme's side file from @p oldName to @p newName, moving the open
    /// scheme draft with it. Side-file schemes only.
    [[nodiscard]] std::expected<void, config::SettingsError> renameColorScheme(QString const& oldName,
                                                                               QString const& newName);
    /// The page's Duplicate of a scheme: duplicateColorScheme, a failure reported through errorOccurred.
    Q_INVOKABLE void duplicateScheme(QString const& name);
    /// The page's Delete of a scheme: deleteColorScheme, a failure reported through errorOccurred.
    Q_INVOKABLE void requestDeleteScheme(QString const& name);
    /// The page's Rename of a scheme: renameColorScheme, a failure reported through errorOccurred.
    Q_INVOKABLE void requestRenameScheme(QString const& oldName, QString const& newName);
    // }}}

  signals:
    /// The list models (profiles, color schemes, default profile, locked) changed.
    void changed();
    /// The profile draft changed (opened, a field edited, saved).
    void draftChanged();
    /// The color-scheme draft changed.
    void schemeDraftChanged();
    /// The global fields changed: the settings.yml draft, or the configuration they show.
    void globalFieldsChanged();
    /// Whether this Settings session has something to revert may have changed.
    void changesChanged();
    /// The IDENTITY of the color scheme being edited changed (a different scheme selected, one duplicated,
    /// renamed or deleted) — but NOT a mere color tweak. Kept separate from
    /// @ref schemeDraftChanged so the editor's name field can re-seed on an identity change without being
    /// wiped every time the user adjusts a color while typing a new name.
    void editingSchemeChanged();
    /// A user-facing error occurred (e.g. a write failed); @p message is human-readable.
    void errorOccurred(QString const& message);

  private:
    /// The origin of @p name among the current profiles, defaulting to Builtin if unknown.
    [[nodiscard]] config::SettingsOrigin profileOrigin(std::string const& name) const;
    /// Whether a GUI-owned side file @c colorschemes/<name>.yml exists, as the store reads it. This is the
    /// editability ground truth the color-scheme list uses: only such schemes can be edited, renamed or
    /// deleted.
    /// @param name The color scheme name. @return true if the side file exists.
    [[nodiscard]] bool colorSchemeSideFileExists(std::string const& name) const;
    /// The palette of colour scheme @p name's GUI side file, read through the store.
    /// @param name The color scheme name. @return The palette; nullopt without a side file that parses.
    [[nodiscard]] std::optional<vtbackend::ColorPalette> sideFileScheme(std::string const& name) const;
    /// Whether @p name is a color scheme declared inline in contour.yml with no GUI side file backing it.
    /// Such a name would shadow a @c colorschemes/<name>.yml side file at load time (the inline node wins),
    /// so the GUI must refuse to write one. @param name The color scheme name. @return true if inline.
    [[nodiscard]] bool isInlineColorScheme(std::string const& name) const;
    /// Reports @p error to the UI.
    void fail(std::string const& error);
    /// Reports @p result's error to the UI, if it has one: the QML wrappers' idiom.
    template <typename T>
    void report(std::expected<T, config::SettingsError> const& result);
    /// What the draft's link patterns have wrong (one the reader would skip, or one of the profile's own the
    /// terminal would disable), so that staging refuses what the reader would discard. A field with a
    /// problem (a descriptor's `validate`) is no reason: the reader keeps it, and its row shows the problem.
    /// @return The first link pattern's problem; nullopt when none has one.
    [[nodiscard]] std::optional<std::string> draftProblem() const;
    /// The palette the scheme named @p name stands for: a `color_schemes:` entry of contour.yml, a GUI side
    /// file, or a built-in scheme, in that order; the default palette for a name none answers to. Read once
    /// per name until the next refresh() (@ref _previewPaletteCache).
    [[nodiscard]] vtbackend::ColorPalette schemePalette(std::string const& name) const;
    /// The palette the draft profile's status-line roles preview against: its scheme, or for a light/dark
    /// pair the one of the current appearance -- by name, so a scheme switched in the draft previews before
    /// it is saved. The default palette while no profile is open.
    [[nodiscard]] vtbackend::ColorPalette previewPalette() const;

    /// A new session over the store and the scheduler, Locked while `gui_config_locked` is set.
    [[nodiscard]] std::unique_ptr<config::SettingsEditSession> makeSession();
    /// What the session's errors do: shown on the page, and for a file that did not take as staged -- a write
    /// that failed, or a version written outside Settings -- the drafts re-seeded from the configuration in
    /// effect.
    void onSessionError(config::SettingsError const& error);
    /// Re-seeds every open draft from the configuration in effect; one whose entity is gone closes.
    void reseedDrafts();
    /// Re-seeds the settings.yml draft from the configuration in effect.
    void reseedGlobalDraft();
    /// Whether the open profile draft may be edited.
    /// @return Nothing, or why not: Locked, or ReadOnly naming where the profile is defined.
    [[nodiscard]] std::expected<void, config::SettingsError> profileDraftWritable() const;
    /// Whether settings.yml may be written: not while locked, and not while it does not parse.
    [[nodiscard]] std::expected<void, config::SettingsError> globalDraftWritable() const;
    /// Stages @p bytes for @p file, and says the session's changes may have changed.
    [[nodiscard]] std::expected<void, config::SettingsError> stage(config::SideFile const& file,
                                                                   std::optional<std::string> bytes);
    /// Stages the open profile draft, unless a link pattern of it the reader would skip.
    [[nodiscard]] std::expected<void, config::SettingsError> stageProfileDraft();
    /// The void setters' staging of the profile draft: a failure is reported, except a link pattern still
    /// in error, which its row shows.
    void stageProfileDraftReporting();
    /// Stages the open color-scheme draft.
    [[nodiscard]] std::expected<void, config::SettingsError> stageSchemeDraft();
    /// Stages the settings.yml draft; on failure it is re-seeded, so no row shows a value not staged.
    [[nodiscard]] std::expected<void, config::SettingsError> stageGlobalDraft();
    /// Rebuilds the profile rows and the default profile from the configuration and the settings.yml draft.
    void rebuildProfileRows();
    /// Whether profile @p name is a GUI side file, written or staged in this session.
    [[nodiscard]] bool isSideFileProfile(std::string const& name) const;
    /// @p file's bytes as the configuration in effect loaded them; nullopt when it loaded none.
    [[nodiscard]] std::optional<std::string> loadedBytesOf(config::SideFile const& file) const;
    /// Re-seeds each open editable draft whose side file a reload read anew, while nothing is pending for it:
    /// a change made outside Settings and reloaded is what the page shows and what the session saw.
    void followReloadedDrafts();
    /// The side-file profiles whose spelled status line follows a switch of the defaults to @p now
    /// (config::indicatorForEdition), as the files to stage with the switch; the open draft follows too.
    /// @param now The edition switched to.
    /// @return One file per profile that changes.
    [[nodiscard]] std::vector<config::StagedFile> profilesAdoptingEdition(config::DefaultsEdition now);
    /// Whether profile @p name exists, as a side file, or is staged.
    [[nodiscard]] bool profileNameTaken(std::string const& name) const;
    /// Whether color scheme @p name exists, as a side file, inline, or is staged.
    [[nodiscard]] bool schemeNameTaken(std::string const& name) const;
    /// Opens @p profile as the editable draft named @p name, staged as a new side file.
    [[nodiscard]] std::expected<QString, config::SettingsError> openNewProfile(
        std::string const& name, config::TerminalProfile profile);

    /// Whether a draft is open, and whether it may be edited.
    enum class DraftAccess : uint8_t
    {
        Closed = 0, ///< No draft is open.
        ReadOnly,   ///< Open, shown, not editable (contour.yml, built in, or locked).
        Editable,   ///< Open and editable: a GUI side file.
    };

    ConfigAccessor _config;
    SettingsControllerServices _services;
    vtbackend::ColorPreference _appearance;   ///< Which scheme of a light/dark profile the previews use.
    std::vector<display::GpuCandidate> _gpus; ///< Listed by refresh(), not on every property read.
    QStringList _fontFamilies;                ///< Listed by refresh(), not on every property read.
    /// Forwards to _services.scheduler, telling the page after each debounced flush. Declared before
    /// _session, which borrows it.
    std::unique_ptr<config::SaveScheduler> _scheduler;
    std::unique_ptr<config::SettingsEditSession> _session;

    /// Whether `gui_config_locked`, as refresh() last read it, lets Settings write; the session follows it.
    config::SettingsEditability _editability = config::SettingsEditability::Editable;
    QString _linkGesture; ///< Listed by refresh(): how links are opened.
    QVariantList _profiles;
    QVariantList _colorSchemes;
    QVariantList _keybindings;
    QString _defaultProfile;

    /// The settings.yml draft: what every global commit edits and stages, so two commits inside one debounce
    /// both land. Re-seeded from the configuration while nothing is pending for settings.yml.
    config::GuiManagedSettings _globalDraft;

    // The profile draft.
    config::TerminalProfile _draft;
    QString _editingProfile; //!< "" while none is open.
    DraftAccess _profileAccess = DraftAccess::Closed;
    std::optional<std::string> _draftSeededFrom; //!< The side file's bytes an editable draft was seeded from.

    // The color-scheme draft.
    vtbackend::ColorPalette _schemeDraft;
    QString _editingScheme; //!< "" while none is open.
    DraftAccess _schemeAccess = DraftAccess::Closed;
    std::optional<std::string>
        _schemeSeededFrom; //!< The side file's bytes an editable draft was seeded from.

    /// A scheme's palette as @ref schemePalette last read it, under the name it was read for.
    struct CachedPalette
    {
        std::string name;                ///< The scheme name the palette was read for.
        vtbackend::ColorPalette palette; ///< What @ref schemePalette returned for it.
    };
    /// The preview's palette, read once per scheme name rather than on every parse of a segment (a side
    /// file's read and parse is not free, and the editor parses on every keystroke). A different draft
    /// scheme or appearance names a different scheme and misses; refresh() -- which every scheme save,
    /// rename, delete and configuration reload runs -- forgets it. Mutable: a cache behind a const read.
    mutable std::optional<CachedPalette> _previewPaletteCache;
};

} // namespace contour::window
