/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <crispy/App.h>
#include <crispy/debuglog.h>
#include <crispy/utils.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <numeric>
#include <optional>

#if !defined(_WIN32)
#include <pwd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using std::bind;
using std::max;
using std::exception;
using std::left;
using std::nullopt;
using std::optional;
using std::setw;
using std::string;
using std::string_view;

namespace CLI = crispy::cli;

namespace // {{{ helper
{
    CLI::HelpStyle helpStyle()
    {
        auto style = CLI::HelpStyle{};

        style.optionStyle = CLI::OptionStyle::Natural;

#if !defined(_WIN32)
        if (!isatty(STDOUT_FILENO))
        {
            style.colors.reset();
            style.hyperlink = false;
        }
#endif

         return style;
    }

    int screenWidth()
    {
        constexpr auto DefaultWidth = 80;

#if !defined(_WIN32)
        auto ws = winsize{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1)
            return ws.ws_col;
#endif

        return DefaultWidth;
    }

    void customizeDebugLog()
    {
        // A curated list of colors.
        static const bool colorized =
#if !defined(_WIN32)
            isatty(STDOUT_FILENO);
#else
            true;
#endif
        static constexpr auto colors = std::array<int, 23>{
            2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15,
            150, 155, 159, 165, 170, 175, 180, 185, 190, 195, 200,
        };
        crispy::logging_sink::for_debug().set_transform([](crispy::log_message const& _msg) -> std::string
        {
            auto const [sgrTag, sgrMessage, sgrReset] = [&]() -> std::tuple<string, string, string>
            {
                if (!colorized)
                    return {"", "", ""};
                auto const tagStart = "\033[1m";
                auto const colorIndex = colors.at((_msg.tag().value) % colors.size());
                auto const msgStart = fmt::format("\033[38;5;{}m", colorIndex);
                auto const resetSGR = fmt::format("\033[m");
                return {tagStart, msgStart, resetSGR};
            }();

            auto const srcIndex = string_view(_msg.location().file_name()).find("src");
            auto const fileName = string(srcIndex != string_view::npos
                ? string_view(_msg.location().file_name()).substr(srcIndex + 4)
                : string(_msg.location().file_name()));

            auto result = string{};

            for (auto const [i, line] : crispy::indexed(crispy::split(_msg.text(), '\n')))
            {
                if (i != 0)
                    result += "        ";
                else
                {
                    result += sgrTag;
                    if (_msg.tag().value == crispy::ErrorTag.value)
                    {
                        result += fmt::format("[{}] ", "error");
                    }
                    else
                    {
                        result += fmt::format("[{}:{}:{}] ",
                                              crispy::debugtag::get(_msg.tag()).name,
                                              fileName,
                                              _msg.location().line()
                                );
                    }
                    result += sgrReset;
                }

                result += sgrMessage;
                result += line;
                result += sgrReset;
                result += '\n';
            }

            return result;
        });
    }

    FileSystem::path xdgStateHome()
    {
        if (auto const* p = getenv("XDG_STATE_HOME"); p && *p)
            return FileSystem::path(p);

#if defined(_WIN32)
        if (auto const* p = getenv("LOCALAPPDATA"); p && *p)
            return FileSystem::path(p);
#else
        if (passwd const* pw = getpwuid(getuid()); pw && pw->pw_dir)
            return FileSystem::path(pw->pw_dir) / ".local" / "state";
#endif

        return FileSystem::temp_directory_path();
    }
} // }}}

namespace crispy {

App* App::instance_ = nullptr;

App::App(std::string _appName, std::string _appTitle, std::string _appVersion) :
    appName_{ std::move(_appName) },
    appTitle_{ std::move(_appTitle) },
    appVersion_{ std::move(_appVersion) },
    localStateDir_{xdgStateHome() / appName_}
{
    instance_ = this;

    link(appName_ + ".help", bind(&App::helpAction, this));
    link(appName_ + ".version", bind(&App::versionAction, this));
}

App::~App()
{
    instance_ = nullptr;
}

void App::link(std::string _command, std::function<int()> _handler)
{
    handlers_[move(_command)] = move(_handler);
}

void App::listDebugTags()
{
    auto tags = crispy::debugtag::store();
    sort(
        begin(tags),
        end(tags),
        [](crispy::debugtag::tag_info const& a, crispy::debugtag::tag_info const& b) {
           return a.name < b.name;
        }
    );
    auto const maxNameLength = std::accumulate(
        begin(tags),
        end(tags),
        size_t{0},
        [&](auto _acc, auto const& _tag) { return max(_acc, _tag.name.size()); }
    );
    auto const column1Length = maxNameLength + 2u;
    for (auto const& tag: tags)
    {
        std::cout
            << left << setw(int(column1Length)) << tag.name
            << "; " << tag.description << '\n';
    }
}

int App::helpAction()
{
    std::cout << CLI::helpText(syntax_.value(), helpStyle(), screenWidth());
    return EXIT_SUCCESS;
}

int App::versionAction()
{
    std::cout << fmt::format("{} {}\n\n", appTitle_, appVersion_);
    return EXIT_SUCCESS;
}

// customize debuglog transform to shorten the file_name output a bit
int App::run(int argc, char const* argv[])
{
    try
    {
        customizeDebugLog();

        syntax_ = parameterDefinition();

        optional<CLI::FlagStore> flagsOpt = CLI::parse(syntax_.value(), argc, argv);
        if (!flagsOpt.has_value())
        {
            std::cerr << "Failed to parse command line parameters.\n";
            return EXIT_FAILURE;
        }
        flags_ = std::move(flagsOpt.value());

        // std::cout << fmt::format("Flags: {}\n", parameters().values.size());
        // for (auto const & [k, v] : parameters().values)
        //     std::cout << fmt::format(" - {}: {}\n", k, v);

        for (auto const& [name, handler]: handlers_)
            if (parameters().get<bool>(name))
                return handler();

        std::cerr << fmt::format("Usage error.\n");
        return EXIT_FAILURE;
    }
    catch (exception const& e)
    {
        std::cerr << fmt::format("Unhandled error caught. {}", e.what());
        return EXIT_FAILURE;
    }
}

}
