// SPDX-License-Identifier: Apache-2.0
#include <contour/config/HostSideFile.hpp>

#include <yaml-cpp/yaml.h>

namespace contour::config
{

std::string emitHostYaml(HostDraft const& host)
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    for (auto const& field: HostFields)
        if (auto const& value = host.*field.member; !value.empty())
            out << YAML::Key << std::string { field.key } << YAML::Value << value;
    out << YAML::EndMap;
    return std::string { out.c_str() } + '\n';
}

std::optional<HostDraft> parseHostDraftYaml(std::string const& contents)
{
    try
    {
        auto const document = YAML::Load(contents);
        if (!document.IsMap())
            return std::nullopt;
        auto draft = HostDraft {};
        for (auto const& field: HostFields)
            if (auto const value = document[std::string { field.key }]; value && value.IsScalar())
                draft.*field.member = value.as<std::string>();
        return draft;
    }
    catch (YAML::Exception const&)
    {
        return std::nullopt;
    }
}

} // namespace contour::config
