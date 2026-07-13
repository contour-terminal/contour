// SPDX-License-Identifier: Apache-2.0
#include <contour/AtomicFileWrite.h>
#include <contour/CommandHistoryStore.h>

#include <yaml-cpp/yaml.h>

#include <exception>
#include <format>
#include <string>

namespace contour
{

std::expected<std::vector<std::string>, std::string> FileCommandHistoryStore::load(
    std::filesystem::path const& path) const
{
    auto ids = std::vector<std::string> {};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return ids; // nothing recorded yet is not an error

    try
    {
        auto const doc = YAML::LoadFile(path.string());
        auto const recent = doc["recent"];
        if (!recent || !recent.IsSequence())
            return ids; // a file without a `recent:` sequence simply remembers nothing

        for (auto const& entry: recent)
            if (entry.IsScalar())
                ids.push_back(entry.as<std::string>());
    }
    catch (std::exception const& e)
    {
        return std::unexpected(std::string(e.what()));
    }

    return ids;
}

std::expected<void, std::string> FileCommandHistoryStore::save(std::filesystem::path const& path,
                                                               std::span<std::string const> ids)
{
    // The emitter (rather than hand-built text) so an id carrying YAML-significant characters — a
    // SendChars binding's escape sequence, say — is quoted correctly instead of producing a file that
    // fails to parse on the next start.
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "recent" << YAML::Value << YAML::BeginSeq;
    for (auto const& id: ids)
        out << id;
    out << YAML::EndSeq;
    out << YAML::EndMap;

    // Atomically, like the layout store: losing the MRU costs the user some ordering rather than their
    // work, but a HALF-written file would fail to parse on the next start, and a store the program
    // cannot read back is one the user has to go delete by hand. See atomicWriteFile().
    return atomicWriteFile(path, std::format("{}\n", out.c_str()));
}

} // namespace contour
