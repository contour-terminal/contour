// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>

#include <crispy/utils.h>

#include <algorithm>
#include <optional>
#include <string>

using namespace std;

using crispy::toLower;

namespace contour::actions
{

optional<Action> fromString(string const& name)
{
    // Case-insensitive, so `input_mapping:` may spell an action however it reads best.
    auto const lowerCaseName = toLower(name);
    auto const& catalog = actionCatalog();
    auto const entry = std::ranges::find_if(
        catalog, [&](auto const& candidate) { return toLower(candidate.name) == lowerCaseName; });

    if (entry == catalog.end())
        return nullopt;

    // The prototype: a ParameterizedActionConcept action still has its argument filled in by the
    // caller (the YAML reader) from the sibling keys of the `action:` entry.
    return entry->prototype;
}

} // namespace contour::actions
