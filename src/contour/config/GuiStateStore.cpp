// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>
#include <contour/config/GuiStateStore.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace contour::config
{

namespace
{
    /// The key the dismissed offers are listed under.
    constexpr auto DismissedOffersKey = std::string_view { "dismissed_new_defaults" };

    /// The bytes of @p path.
    /// @param path The file to read.
    /// @return Its content, or GuiStateError::Unreadable.
    [[nodiscard]] std::expected<std::string, GuiStateError> readWholeFile(std::filesystem::path const& path)
    {
        auto stream = std::ifstream { path, std::ios::binary };
        if (!stream)
            return std::unexpected { GuiStateError::Unreadable };
        auto content = std::ostringstream {};
        content << stream.rdbuf();
        if (stream.bad())
            return std::unexpected { GuiStateError::Unreadable };
        return std::move(content).str();
    }

    /// The state @p content spells.
    /// @param content A GUI state document.
    /// @return The state, or GuiStateError::Malformed when it is not YAML, or its offers are not a list.
    [[nodiscard]] std::expected<GuiState, GuiStateError> parseGuiState(std::string const& content)
    {
        try
        {
            auto const document = YAML::Load(content);
            if (document.IsNull())
                return GuiState {}; // An empty file: nothing stored.
            if (!document.IsMap())
                return std::unexpected { GuiStateError::Malformed };
            auto const offers = document[std::string { DismissedOffersKey }];
            if (!offers)
                return GuiState {};
            if (!offers.IsSequence())
                return std::unexpected { GuiStateError::Malformed };
            auto state = GuiState {};
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

    /// The map @p content spells.
    /// @param content A YAML document.
    /// @return Its root when that is a map, else an empty map.
    [[nodiscard]] YAML::Node mapIn(std::string const& content)
    {
        try
        {
            if (auto document = YAML::Load(content); document.IsMap())
                return document;
        }
        catch (YAML::Exception const&)
        {
            errorLog()("The GUI state changed while it was saved; rewriting it whole.");
        }
        return YAML::Node { YAML::NodeType::Map };
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
        auto out = YAML::Emitter {};
        out << previous;
        return std::string { out.c_str() } + '\n';
    }
} // namespace

FileGuiStateStore::FileGuiStateStore(std::filesystem::path configDir):
    _file { std::move(configDir) / GuiStateFileName }
{
}

std::expected<GuiState, GuiStateError> FileGuiStateStore::load() const
{
    auto ec = std::error_code {};
    if (!std::filesystem::exists(_file, ec))
        return ec ? std::expected<GuiState, GuiStateError> { std::unexpected { GuiStateError::Unreadable } }
                  : GuiState {};
    return readWholeFile(_file).and_then(parseGuiState);
}

std::expected<void, GuiStateError> FileGuiStateStore::save(GuiState const& state)
{
    auto previous = YAML::Node { YAML::NodeType::Map };
    if (auto ec = std::error_code {}; std::filesystem::exists(_file, ec))
    {
        if (load())
        {
            // It loaded, so it reads and parses as a map or as nothing.
            previous = mapIn(readWholeFile(_file).value_or(std::string {}));
        }
        else
        {
            // A file this release cannot read may still hold what a user or a newer release wrote: keep a
            // copy before replacing it, and replace nothing when the copy fails.
            auto backup = _file;
            backup += GuiStateBackupSuffix;
            std::filesystem::copy_file(_file, backup, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                errorLog()("Could not back up {}: {}", core::pathToUtf8(_file), ec.message());
                return std::unexpected { GuiStateError::Unwritable };
            }
        }
    }
    return atomicWriteFile(_file, emitGuiState(state, std::move(previous)))
        .transform_error([this](std::string const& error) {
            errorLog()("Could not write {}: {}", core::pathToUtf8(_file), error);
            return GuiStateError::Unwritable;
        });
}

} // namespace contour::config
