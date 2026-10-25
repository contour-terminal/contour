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

    /// The document that stores @p state.
    /// @param state The state to spell.
    /// @return `dismissed_new_defaults: [token, …]` and a trailing newline.
    [[nodiscard]] std::string emitGuiState(GuiState const& state)
    {
        auto out = YAML::Emitter {};
        out << YAML::BeginMap << YAML::Key << std::string { DismissedOffersKey } << YAML::Value << YAML::Flow
            << YAML::BeginSeq;
        for (auto const offer: state.dismissedOffers)
            out << std::string { configEnumToken(offer) };
        out << YAML::EndSeq << YAML::EndMap;
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
    return atomicWriteFile(_file, emitGuiState(state)).transform_error([this](std::string const& error) {
        errorLog()("Could not write {}: {}", core::pathToUtf8(_file), error);
        return GuiStateError::Unwritable;
    });
}

} // namespace contour::config
