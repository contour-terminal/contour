// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>
#include <contour/config/GuiStateStore.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::config
{

namespace
{
    /// How the new-defaults offers a user answered are spelled in the file.
    struct OfferCodec
    {
        using Item = NewDefaultsOffer;
        static constexpr auto Key = std::string_view { "dismissed_new_defaults" };

        [[nodiscard]] static std::optional<Item> parse(std::string const& token)
        {
            return configEnumFromToken<NewDefaultsOffer>(token);
        }

        [[nodiscard]] static std::string spell(Item item) { return std::string { configEnumToken(item) }; }
    };

    /// How the shells whose install offer a user declined are spelled in the file: by their row's name.
    struct ShellCodec
    {
        using Item = ShellKind;
        static constexpr auto Key = std::string_view { "dismissed_shell_integration" };

        [[nodiscard]] static std::optional<Item> parse(std::string const& token)
        {
            auto const row = std::ranges::find(ShellRows, std::string_view { token }, &ShellRow::name);
            if (row == ShellRows.end())
                return std::nullopt;
            return row->kind;
        }

        [[nodiscard]] static std::string spell(Item item)
        {
            auto const* const row = shellRow(item);
            return row != nullptr ? std::string { row->name } : std::string {};
        }
    };

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

    /// The items @p document lists under @p Codec's key.
    /// @param document A parsed GUI state document, a map.
    /// @return The items it knows, in order, skipping a token this release does not know (a newer
    ///         release wrote it); none when the key is absent; GuiStateError::Malformed when it is not a
    ///         list.
    template <typename Codec>
    [[nodiscard]] std::expected<std::vector<typename Codec::Item>, GuiStateError> listedItems(
        YAML::Node const& document)
    {
        auto const list = document[std::string { Codec::Key }];
        if (!list)
            return std::vector<typename Codec::Item> {};
        if (!list.IsSequence())
            return std::unexpected { GuiStateError::Malformed };
        auto items = std::vector<typename Codec::Item> {};
        for (auto const& entry: list)
            if (entry.IsScalar())
                if (auto const item = Codec::parse(entry.as<std::string>()))
                    items.push_back(*item);
        return items;
    }

    /// The state @p document holds.
    /// @param document A parsed GUI state document.
    /// @return The state, or GuiStateError::Malformed when it is not a map, or one of its lists is not a
    /// list.
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
            auto offers = listedItems<OfferCodec>(document);
            if (!offers)
                return std::unexpected { offers.error() };
            auto shells = listedItems<ShellCodec>(document);
            if (!shells)
                return std::unexpected { shells.error() };
            state.dismissedOffers = *std::move(offers);
            state.dismissedShellIntegrations = *std::move(shells);
            return state;
        }
        catch (YAML::Exception const&)
        {
            return std::unexpected { GuiStateError::Malformed };
        }
    }

    /// The tokens of @p list that name no item of @p Codec this release knows.
    /// @param list The node under @p Codec's key.
    /// @return Its unknown scalar entries, in order.
    template <typename Codec>
    [[nodiscard]] std::vector<std::string> unknownTokens(YAML::Node const& list)
    {
        auto tokens = std::vector<std::string> {};
        if (list.IsSequence())
            for (auto const& entry: list)
                if (entry.IsScalar() && !Codec::parse(entry.as<std::string>()))
                    tokens.push_back(entry.as<std::string>());
        return tokens;
    }

    /// Stores @p items under @p Codec's key of @p document, keeping the tokens already there that
    /// this release does not know, after the known ones.
    template <typename Codec>
    void storeList(YAML::Node& document, std::vector<typename Codec::Item> const& items)
    {
        auto const key = std::string { Codec::Key };
        auto list = YAML::Node { YAML::NodeType::Sequence };
        for (auto const item: items)
            list.push_back(Codec::spell(item));
        for (auto const& token: unknownTokens<Codec>(document[key]))
            list.push_back(token);
        list.SetStyle(YAML::EmitterStyle::Flow);
        document[key] = list;
    }

    /// The document that stores @p state over @p previous, the document the file held.
    ///
    /// What this release does not know survives the rewrite: every other key of @p previous, and the
    /// tokens of its lists that name nothing this release knows, after the known ones.
    /// @param state The state to spell.
    /// @param previous The file's current document, a map; an empty node when there is none.
    /// @return `dismissed_new_defaults: [token, …]`, `dismissed_shell_integration: [token, …]`, any other
    ///         keys, and a trailing newline.
    [[nodiscard]] std::string emitGuiState(GuiState const& state, YAML::Node previous)
    {
        storeList<OfferCodec>(previous, state.dismissedOffers);
        storeList<ShellCodec>(previous, state.dismissedShellIntegrations);
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
