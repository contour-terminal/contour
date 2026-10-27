// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>
#include <contour/config/GuiStateStore.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <yaml-cpp/yaml.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace contour::config
{

namespace
{
    /// The key the dismissed offers are listed under.
    constexpr auto DismissedOffersKey = std::string_view { "dismissed_new_defaults" };

    /// The key the Settings window's state is kept under.
    constexpr auto SettingsWindowKey = std::string_view { "settings_window" };

    /// The Settings window state @p node holds.
    /// @param node A `settings_window` node.
    /// @return The state; none when the node is absent or not a map. A key that does not read as its type is
    ///         left at its default, and an unknown `show` word reads as Normal.
    [[nodiscard]] std::optional<SettingsWindowState> settingsWindowOf(YAML::Node const& node)
    {
        if (!node || !node.IsMap())
            return std::nullopt;
        auto state = SettingsWindowState {};
        auto const read = [&node]<typename T>(std::string_view key, T& into) {
            if (auto const value = node[std::string { key }]; value && value.IsScalar())
                into = value.as<T>(into);
        };
        read("x", state.x);
        read("y", state.y);
        read("width", state.width);
        read("height", state.height);
        read("screen", state.screen);
        read("page", state.page);
        auto show = std::string {};
        read("show", show);
        state.show = configEnumFromToken<WindowShowState>(show).value_or(WindowShowState::Normal);
        return state;
    }

    /// The node that stores @p state.
    [[nodiscard]] YAML::Node settingsWindowNode(SettingsWindowState const& state)
    {
        auto node = YAML::Node { YAML::NodeType::Map };
        node["x"] = state.x;
        node["y"] = state.y;
        node["width"] = state.width;
        node["height"] = state.height;
        node["screen"] = state.screen;
        node["show"] = std::string { configEnumToken(state.show) };
        node["page"] = state.page;
        return node;
    }

    /// The YAML document @p content spells.
    /// @param content The bytes of a GUI state file.
    /// @return Its root node, or GuiStateError::Malformed when it is not YAML.
    [[nodiscard]] std::expected<YAML::Node, GuiStateError> parseDocument(std::string const& content)
    {
        try
        {
            return YAML::Load(content);
        }
        catch (YAML::Exception const&)
        {
            return std::unexpected { GuiStateError::Malformed };
        }
    }

    /// The state @p document holds.
    /// @param document A parsed GUI state document.
    /// @return The state, or GuiStateError::Malformed when it is not a map, or its offers are not a list.
    [[nodiscard]] std::expected<GuiState, GuiStateError> guiStateOf(YAML::Node const& document)
    {
        try
        {
            if (document.IsNull())
                return GuiState {}; // An empty file: nothing stored.
            if (!document.IsMap())
                return std::unexpected { GuiStateError::Malformed };
            auto state = GuiState {};
            state.settingsWindow = settingsWindowOf(document[std::string { SettingsWindowKey }]);
            auto const offers = document[std::string { DismissedOffersKey }];
            if (!offers)
                return state;
            if (!offers.IsSequence())
                return std::unexpected { GuiStateError::Malformed };
            for (auto const& entry: offers)
            {
                // A token this release does not know is one a newer release wrote: skip it, keep the rest.
                if (!entry.IsScalar())
                    continue;
                if (auto const offer = configEnumFromToken<NewDefaultsOffer>(entry.as<std::string>()))
                    state.dismissedOffers.push_back(*offer);
            }
            return state;
        }
        catch (YAML::Exception const&)
        {
            return std::unexpected { GuiStateError::Malformed };
        }
    }

    /// The tokens of @p offers that name no offer this release knows.
    /// @param offers A `dismissed_new_defaults` node.
    /// @return Its unknown scalar entries, in order.
    [[nodiscard]] std::vector<std::string> unknownTokens(YAML::Node const& offers)
    {
        auto tokens = std::vector<std::string> {};
        if (offers.IsSequence())
            for (auto const& entry: offers)
                if (entry.IsScalar() && !configEnumFromToken<NewDefaultsOffer>(entry.as<std::string>()))
                    tokens.push_back(entry.as<std::string>());
        return tokens;
    }

    /// The document that stores @p state over @p previous, the document the file held.
    ///
    /// What this release does not know survives the rewrite: every other key of @p previous, and the
    /// tokens of its list that name no offer this release knows, after the known ones.
    /// @param state The state to spell.
    /// @param previous The file's current document, a map; an empty node when there is none.
    /// @return `dismissed_new_defaults: [token, …]`, any other keys, and a trailing newline.
    [[nodiscard]] std::string emitGuiState(GuiState const& state, YAML::Node previous)
    {
        auto const key = std::string { DismissedOffersKey };
        auto offers = YAML::Node { YAML::NodeType::Sequence };
        for (auto const offer: state.dismissedOffers)
            offers.push_back(std::string { configEnumToken(offer) });
        for (auto const& token: unknownTokens(previous[key]))
            offers.push_back(token);
        offers.SetStyle(YAML::EmitterStyle::Flow);
        previous[key] = offers;
        if (state.settingsWindow)
            previous[std::string { SettingsWindowKey }] = settingsWindowNode(*state.settingsWindow);
        else
            previous.remove(std::string { SettingsWindowKey });
        auto out = YAML::Emitter {};
        out << previous;
        return std::string { out.c_str() } + '\n';
    }
} // namespace

FileGuiStateStore::FileGuiStateStore(core::platform::FileSystem const& fileSystem,
                                     std::filesystem::path const& configDir):
    _fileSystem { fileSystem }, _file { configDir / GuiStateFileName }
{
}

std::expected<GuiState, GuiStateError> FileGuiStateStore::load() const
{
    if (!_fileSystem.exists(_file))
        return GuiState {};
    return _fileSystem.readFile(_file)
        .transform_error([](std::string const&) { return GuiStateError::Unreadable; })
        .and_then(parseDocument)
        .and_then(guiStateOf);
}

std::expected<void, GuiStateError> FileGuiStateStore::save(GuiState const& state)
{
    auto previous = YAML::Node { YAML::NodeType::Map };
    if (_fileSystem.exists(_file))
    {
        // Read and parsed once: the document that loads is the one the rewrite keeps.
        auto const document =
            _fileSystem.readFile(_file)
                .transform_error([](std::string const&) { return GuiStateError::Unreadable; })
                .and_then(parseDocument);
        if (document && guiStateOf(*document))
        {
            // It loaded, so it is a map or nothing; nothing leaves the empty map.
            if (document->IsMap())
                previous = *document;
        }
        else
        {
            // A file this release cannot read may still hold what a user or a newer release wrote: keep a
            // copy before replacing it, and replace nothing when the copy fails.
            auto backup = _file;
            backup += GuiStateBackupSuffix;
            if (auto const copied =
                    _fileSystem.copyFile(_file, backup, core::platform::OverwritePolicy::Replace);
                !copied)
            {
                errorLog()("Could not back up {}: {}", core::pathToUtf8(_file), copied.error());
                return std::unexpected { GuiStateError::Unwritable };
            }
        }
    }
    return writeFileDurably(_fileSystem, _file, emitGuiState(state, previous))
        .transform_error([this](std::string const& error) {
            errorLog()("Could not write {}: {}", core::pathToUtf8(_file), error);
            return GuiStateError::Unwritable;
        });
}

} // namespace contour::config
