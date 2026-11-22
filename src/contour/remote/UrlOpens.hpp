// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which of the links a daemon extension asked a window to open (OpenUrl) it opens: the decision, apart from
/// the launcher that carries it out, so that it is tested without one.

#include <span>
#include <string>
#include <string_view>

#include <vthost/UrlToOpen.hpp>

namespace contour::remote
{

/// Opens each link the daemon's rule takes (vthost::checkUrlToOpen()), checked again here: a daemon is not
/// trusted to have applied it. One it refuses is reported and not opened.
/// @param urls The links, oldest first.
/// @param open Opens one link on this machine, as following a hyperlink does.
/// @param refused Told a link that is not opened, and why.
template <typename Open, typename Refused>
void followUrlOpens(std::span<std::string const> urls, Open const& open, Refused const& refused)
{
    for (auto const& url: urls)
    {
        if (auto const checked = vthost::checkUrlToOpen(url); !checked)
            refused(std::string_view { url }, vthost::wordsOf(checked.error()));
        else
            open(std::string_view { url });
    }
}

} // namespace contour::remote
