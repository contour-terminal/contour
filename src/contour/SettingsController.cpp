// SPDX-License-Identifier: Apache-2.0
#include <contour/SettingsController.h>

#include <text_shaper/font.h>

#include <QtGui/QColor>

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <utility>

namespace contour
{

using config::SettingsOrigin;
using config::TerminalProfile;

namespace
{
    /// A data-driven descriptor for one editable scalar profile field: adding a field to the settings
    /// page is adding a row here, not editing logic in three places.
    struct ProfileFieldDescriptor
    {
        QString key;                                                //!< Stable identifier / QML key.
        QString label;                                              //!< Human-readable label.
        QString help;                                               //!< One-line help text.
        QString type;                                               //!< "bool" | "double" | "string".
        std::function<QVariant(TerminalProfile const&)> get;        //!< Reads the field as a QVariant.
        std::function<void(TerminalProfile&, QVariant const&)> set; //!< Writes the field from QVariant.
    };

    /// The editable scalar profile fields. Kept intentionally small for the first version; it grows one
    /// row at a time toward full parity without touching any other code.
    std::vector<ProfileFieldDescriptor> const& profileFieldDescriptors()
    {
        static auto const descriptors = std::vector<ProfileFieldDescriptor> {
            { "show_title_bar",
              "Show title bar",
              "Whether the window shows its title bar.",
              "bool",
              [](TerminalProfile const& p) { return QVariant(p.showTitleBar.value()); },
              [](TerminalProfile& p, QVariant const& v) { p.showTitleBar = v.toBool(); } },
            { "dim_unfocused",
              "Dim when unfocused",
              "How much to dim a pane while it is not focused (0.0 = off, 1.0 = fully dimmed).",
              "double",
              [](TerminalProfile const& p) { return QVariant(p.dimUnfocused.value()); },
              [](TerminalProfile& p, QVariant const& v) { p.dimUnfocused = v.toDouble(); } },
            { "font_family",
              "Font family",
              "The regular font family name.",
              "string",
              [](TerminalProfile const& p) {
                  return QVariant(QString::fromStdString(p.fonts.value().regular.familyName));
              },
              [](TerminalProfile& p, QVariant const& v) {
                  p.fonts.value().regular.familyName = v.toString().toStdString();
              } },
            { "font_size",
              "Font size (pt)",
              "The font size in points.",
              "double",
              [](TerminalProfile const& p) { return QVariant(p.fonts.value().size.pt); },
              [](TerminalProfile& p, QVariant const& v) {
                  p.fonts.value().size = text::font_size { v.toDouble() };
              } },
            { "size_indicator_on_resize",
              "Show size on resize",
              "Briefly show the terminal dimensions when the window is resized.",
              "bool",
              [](TerminalProfile const& p) { return QVariant(p.sizeIndicatorOnResize.value()); },
              [](TerminalProfile& p, QVariant const& v) { p.sizeIndicatorOnResize = v.toBool(); } },
        };
        return descriptors;
    }

    /// A data-driven descriptor for one editable color slot of a palette.
    struct SchemeColorDescriptor
    {
        QString key;
        QString label;
        std::function<vtbackend::RGBColor(vtbackend::ColorPalette const&)> get;
        std::function<void(vtbackend::ColorPalette&, vtbackend::RGBColor)> set;
    };

    /// The color slots the scheme editor exposes: the default fg/bg plus the 8 normal and 8 bright
    /// ANSI colors. Generated once from the ANSI names so the two 8-color banks are not hand-listed.
    std::vector<SchemeColorDescriptor> const& schemeColorDescriptors()
    {
        static auto const descriptors = [] {
            auto list = std::vector<SchemeColorDescriptor> {};
            list.push_back({ "background",
                             "Background",
                             [](auto const& p) { return p.defaultBackground; },
                             [](auto& p, auto c) { p.defaultBackground = c; } });
            list.push_back({ "foreground",
                             "Foreground",
                             [](auto const& p) { return p.defaultForeground; },
                             [](auto& p, auto c) { p.defaultForeground = c; } });
            static constexpr auto ansiNames =
                std::array<std::string_view, 8> { "black", "red",     "green", "yellow",
                                                  "blue",  "magenta", "cyan",  "white" };
            for (auto const bright: { false, true })
                for (auto i = size_t { 0 }; i < ansiNames.size(); ++i)
                {
                    auto const slot = bright ? i + 8 : i;
                    list.push_back({ QString::fromStdString((bright ? "bright_" : "normal_")
                                                            + std::string(ansiNames[i])),
                                     QString::fromStdString((bright ? "Bright " : "Normal ")
                                                            + std::string(ansiNames[i])),
                                     [slot](auto const& p) { return p.palette.at(slot); },
                                     [slot](auto& p, auto c) { p.palette.at(slot) = c; } });
                }
            return list;
        }();
        return descriptors;
    }

    /// "#rrggbb" for @p color, the format the QML color pickers exchange.
    QString rgbToHex(vtbackend::RGBColor color)
    {
        return QString::asprintf("#%02x%02x%02x", color.red, color.green, color.blue);
    }

    /// Parses "#rrggbb" (or any Qt-recognized color string) into an RGBColor, black on a parse failure.
    vtbackend::RGBColor hexToRgb(QString const& hex)
    {
        auto const color = QColor(hex);
        return vtbackend::RGBColor { static_cast<uint8_t>(color.red()),
                                     static_cast<uint8_t>(color.green()),
                                     static_cast<uint8_t>(color.blue()) };
    }

    /// The QML-facing token for a provenance value.
    QString originString(SettingsOrigin origin)
    {
        switch (origin)
        {
            case SettingsOrigin::Builtin: return QStringLiteral("builtin");
            case SettingsOrigin::MainConfig: return QStringLiteral("main");
            case SettingsOrigin::SideFile: return QStringLiteral("side");
        }
        return QStringLiteral("builtin");
    }
} // namespace

SettingsController::SettingsController(ConfigAccessor config,
                                       std::shared_ptr<GuiConfigStore> store,
                                       ApplyCallback apply,
                                       QObject* parent):
    QObject { parent },
    _config { std::move(config) },
    _store { std::move(store) },
    _apply { std::move(apply) }
{
    refresh();
}

SettingsOrigin SettingsController::profileOrigin(std::string const& name) const
{
    auto const& origins = _config().profileOrigins;
    auto const it = origins.find(name);
    return it != origins.end() ? it->second : SettingsOrigin::Builtin;
}

bool SettingsController::fail(std::string const& error)
{
    emit errorOccurred(QString::fromStdString(error));
    return false;
}

void SettingsController::refresh()
{
    auto const& cfg = _config();
    _locked = cfg.guiConfigLocked.value();
    _defaultProfile = QString::fromStdString(cfg.defaultProfileName.value());

    // Profiles, name-sorted for a stable list.
    auto profileNames = std::vector<std::string> {};
    for (auto const& [name, _]: cfg.profiles.value())
        profileNames.push_back(name);
    std::ranges::sort(profileNames);

    _profiles.clear();
    for (auto const& name: profileNames)
    {
        auto const origin = profileOrigin(name);
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(name);
        row[QStringLiteral("origin")] = originString(origin);
        row[QStringLiteral("editable")] = (!_locked && origin == SettingsOrigin::SideFile);
        row[QStringLiteral("isDefault")] = (name == cfg.defaultProfileName.value());
        _profiles.push_back(row);
    }

    // Color schemes: those the config knows about, unioned with any colorschemes/*.yml on disk.
    auto schemeNames = std::set<std::string> {};
    for (auto const& [name, _]: cfg.colorschemes.value())
        schemeNames.insert(name);
    auto const schemesDir = cfg.configFile.parent_path() / "colorschemes";
    auto onDisk = std::set<std::string> {};
    auto ec = std::error_code {};
    if (std::filesystem::is_directory(schemesDir, ec))
        for (auto const& entry: std::filesystem::directory_iterator(schemesDir, ec))
            if (entry.is_regular_file() && entry.path().extension() == ".yml")
            {
                schemeNames.insert(entry.path().stem().string());
                onDisk.insert(entry.path().stem().string());
            }

    _colorSchemes.clear();
    for (auto const& name: schemeNames)
    {
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(name);
        row[QStringLiteral("editable")] = (!_locked && onDisk.contains(name));
        _colorSchemes.push_back(row);
    }

    emit changed();
}

// {{{ Profile draft

QVariantList SettingsController::profileFields() const
{
    auto fields = QVariantList {};
    if (!_hasDraft)
        return fields;
    for (auto const& descriptor: profileFieldDescriptors())
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("help")] = descriptor.help;
        row[QStringLiteral("type")] = descriptor.type;
        row[QStringLiteral("value")] = descriptor.get(_draft);
        fields.push_back(row);
    }
    return fields;
}

void SettingsController::editProfile(QString const& name)
{
    auto const& cfg = _config();
    auto const* profile = cfg.findProfile(name.toStdString());
    if (profile == nullptr)
        return;
    _draft = *profile;
    _editingProfile = name;
    _hasDraft = true;
    _editingReadOnly = _locked || profileOrigin(name.toStdString()) != SettingsOrigin::SideFile;
    emit draftChanged();
}

void SettingsController::newProfile(QString const& basedOn)
{
    auto const& cfg = _config();
    auto const* base = cfg.findProfile(basedOn.toStdString());
    if (base == nullptr)
        base = cfg.findProfile(cfg.defaultProfileName.value());
    _draft = base != nullptr ? *base : TerminalProfile {};
    _editingProfile.clear(); // unsaved: needs Save As
    _hasDraft = true;
    _editingReadOnly = _locked;
    emit draftChanged();
}

void SettingsController::setProfileField(QString const& key, QVariant const& value)
{
    if (!_hasDraft)
        return;
    for (auto const& descriptor: profileFieldDescriptors())
        if (descriptor.key == key)
        {
            descriptor.set(_draft, value);
            emit draftChanged();
            return;
        }
}

QString SettingsController::colorSchemeMode() const
{
    if (_hasDraft && std::holds_alternative<config::DualColorConfig>(_draft.colors.value()))
        return QStringLiteral("dual");
    return QStringLiteral("simple");
}

QString SettingsController::colorScheme() const
{
    if (_hasDraft)
        if (auto const* simple = std::get_if<config::SimpleColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(simple->colorScheme);
    return {};
}

QString SettingsController::colorSchemeLight() const
{
    if (_hasDraft)
        if (auto const* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(dual->colorSchemeLight);
    return {};
}

QString SettingsController::colorSchemeDark() const
{
    if (_hasDraft)
        if (auto const* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(dual->colorSchemeDark);
    return {};
}

void SettingsController::setColorSchemeMode(QString const& mode)
{
    if (!_hasDraft)
        return;
    auto& colors = _draft.colors.value();
    if (mode == QStringLiteral("dual") && std::holds_alternative<config::SimpleColorConfig>(colors))
    {
        auto const name = std::get<config::SimpleColorConfig>(colors).colorScheme;
        colors = config::DualColorConfig { .colorSchemeLight = name, .colorSchemeDark = name };
        emit draftChanged();
    }
    else if (mode == QStringLiteral("simple") && std::holds_alternative<config::DualColorConfig>(colors))
    {
        auto const name = std::get<config::DualColorConfig>(colors).colorSchemeDark;
        colors = config::SimpleColorConfig { .colorScheme = name };
        emit draftChanged();
    }
}

void SettingsController::setColorScheme(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* simple = std::get_if<config::SimpleColorConfig>(&_draft.colors.value()))
    {
        simple->colorScheme = name.toStdString();
        emit draftChanged();
    }
}

void SettingsController::setColorSchemeLight(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
    {
        dual->colorSchemeLight = name.toStdString();
        emit draftChanged();
    }
}

void SettingsController::setColorSchemeDark(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
    {
        dual->colorSchemeDark = name.toStdString();
        emit draftChanged();
    }
}

bool SettingsController::saveProfile()
{
    if (!_hasDraft || _locked)
        return fail("The settings page is read-only.");
    if (_editingProfile.isEmpty())
        return fail("This profile has no name yet — use \"Save As\".");
    if (_editingReadOnly)
        return fail("This profile is defined in contour.yml and cannot be overwritten — use \"Save As\".");

    if (auto const result = _store->saveProfile(_editingProfile.toStdString(), _draft); !result)
        return fail(result.error());

    _apply();
    refresh();
    editProfile(_editingProfile); // re-seed from the reloaded config
    return true;
}

bool SettingsController::saveProfileAs(QString const& newName)
{
    if (!_hasDraft || _locked)
        return fail("The settings page is read-only.");
    if (newName.trimmed().isEmpty())
        return fail("Enter a name for the new profile.");
    if (profileOrigin(newName.toStdString()) == SettingsOrigin::MainConfig)
        return fail("A profile with that name is defined in contour.yml; choose another name.");

    if (auto const result = _store->saveProfile(newName.toStdString(), _draft); !result)
        return fail(result.error());

    _apply();
    refresh();
    editProfile(newName);
    return true;
}

bool SettingsController::deleteProfile(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");
    if (profileOrigin(name.toStdString()) != SettingsOrigin::SideFile)
        return fail("Only GUI-created profiles can be deleted here.");

    if (auto const result = _store->deleteProfile(name.toStdString()); !result)
        return fail(result.error());

    if (_editingProfile == name)
    {
        _hasDraft = false;
        _editingProfile.clear();
        emit draftChanged();
    }
    _apply();
    refresh();
    return true;
}

// }}}

bool SettingsController::setDefaultProfile(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");

    auto settings = _config().guiManagedSettings;
    settings.defaultProfile = name.toStdString();
    if (auto const result = _store->saveGuiSettings(settings); !result)
        return fail(result.error());

    _apply();
    refresh();
    return true;
}

// {{{ Color-scheme draft

QVariantList SettingsController::schemeColors() const
{
    auto colors = QVariantList {};
    if (!_hasSchemeDraft)
        return colors;
    for (auto const& descriptor: schemeColorDescriptors())
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("color")] = rgbToHex(descriptor.get(_schemeDraft));
        colors.push_back(row);
    }
    return colors;
}

void SettingsController::editColorScheme(QString const& name)
{
    auto const& cfg = _config();
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = cfg.colorschemes.value().find(name.toStdString());
        it != cfg.colorschemes.value().end())
        palette = it->second;
    else if (auto const loaded = config::loadColorSchemeFile(cfg.configFile.parent_path() / "colorschemes"
                                                             / (name.toStdString() + ".yml")))
        palette = *loaded;

    _schemeDraft = palette;
    _editingScheme = name;
    _hasSchemeDraft = true;
    emit schemeDraftChanged();
}

void SettingsController::newColorScheme(QString const& basedOn)
{
    auto const& cfg = _config();
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = cfg.colorschemes.value().find(basedOn.toStdString());
        it != cfg.colorschemes.value().end())
        palette = it->second;

    _schemeDraft = palette;
    _editingScheme.clear();
    _hasSchemeDraft = true;
    emit schemeDraftChanged();
}

void SettingsController::setSchemeColor(QString const& key, QString const& color)
{
    if (!_hasSchemeDraft)
        return;
    for (auto const& descriptor: schemeColorDescriptors())
        if (descriptor.key == key)
        {
            descriptor.set(_schemeDraft, hexToRgb(color));
            emit schemeDraftChanged();
            return;
        }
}

bool SettingsController::saveColorScheme(QString const& name)
{
    if (!_hasSchemeDraft || _locked)
        return fail("The settings page is read-only.");
    if (name.trimmed().isEmpty())
        return fail("Enter a name for the color scheme.");

    if (auto const result = _store->saveColorScheme(name.toStdString(), _schemeDraft); !result)
        return fail(result.error());

    _apply();
    refresh();
    _editingScheme = name;
    emit schemeDraftChanged();
    return true;
}

bool SettingsController::deleteColorScheme(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");

    if (auto const result = _store->deleteColorScheme(name.toStdString()); !result)
        return fail(result.error());

    if (_editingScheme == name)
    {
        _hasSchemeDraft = false;
        _editingScheme.clear();
        emit schemeDraftChanged();
    }
    _apply();
    refresh();
    return true;
}

// }}}

} // namespace contour
