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
#include <crispy/indexed.h>
#include <crispy/logstore.h>
#include <crispy/utils.h>

#include <fmt/chrono.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <numeric>
#include <optional>

#if !defined(_WIN32)
    #include <pwd.h>
    #include <unistd.h>

    #include <sys/ioctl.h>
#endif

using std::bind;
using std::cout;
using std::endl;
using std::exception;
using std::left;
using std::max;
using std::nullopt;
using std::optional;
using std::setfill;
using std::setw;
using std::string;
using std::string_view;
using std::chrono::duration_cast;

using namespace std::string_view_literals;

namespace chrono = std::chrono;

namespace CLI = crispy::cli;

namespace
{
std::string operator*(std::string_view a, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; ++i)
        s += a;
    return s;
}

CLI::HelpStyle helpStyle()
{
    auto style = CLI::HelpStyle {};

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

unsigned screenWidth()
{
    constexpr auto DefaultWidth = 80u;

#if !defined(_WIN32)
    auto ws = winsize {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1)
        return ws.ws_col;
#endif

    return DefaultWidth;
}

} // namespace

namespace crispy
{

App* App::instance_ = nullptr;

App::App(std::string _appName, std::string _appTitle, std::string _appVersion, std::string _appLicense):
    appName_ { std::move(_appName) },
    appTitle_ { std::move(_appTitle) },
    appVersion_ { std::move(_appVersion) },
    appLicense_ { std::move(_appLicense) },
    localStateDir_ { xdgStateHome() / appName_ }
{
    if (char const* logFilterString = getenv("LOG"))
    {
        logstore::configure(logFilterString);
        customizeLogStoreOutput();
    }

    instance_ = this;

    link(appName_ + ".help", bind(&App::helpAction, this));
    link(appName_ + ".version", bind(&App::versionAction, this));
    link(appName_ + ".license", bind(&App::licenseAction, this));
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
    auto categories = logstore::get();
    sort(begin(categories), end(categories), [](auto const& a, auto const& b) {
        return a.get().name() < b.get().name();
    });

    auto const maxNameLength =
        std::accumulate(begin(categories), end(categories), size_t { 0 }, [&](auto acc, auto const& cat) {
            return !cat.get().visible() ? acc : max(acc, cat.get().name().size());
        });
    auto const column1Length = maxNameLength + 2;

    for (auto const& category: categories)
    {
        if (!category.get().visible())
            continue;

        // TODO: maybe have color assigned per category AND have that colored here then too?
        std::cout << left << setw(int(column1Length)) << category.get().name() << "; "
                  << category.get().description() << '\n';
    }
}

int App::helpAction()
{
    std::cout << CLI::helpText(syntax_.value(), helpStyle(), screenWidth());
    return EXIT_SUCCESS;
}

int App::licenseAction()
{
    auto const& store = crispy::cli::about::store();
    auto const titleWidth = std::accumulate(
        store.begin(), store.end(), 0u, [](size_t a, auto const& b) { return std::max(a, b.title.size()); });
    auto const licenseWidth = std::accumulate(store.begin(), store.end(), 0u, [](size_t a, auto const& b) {
        return std::max(a, b.license.size());
    });
    auto const urlWidth = std::accumulate(
        store.begin(), store.end(), 0u, [](size_t a, auto const& b) { return std::max(a, b.url.size()); });

    auto const Horiz = "\u2550"sv;
    auto const Vert = "\u2502"sv;
    auto const Cross = "\u256A"sv;

    cout << endl
         << appTitle_ << ' ' << appVersion_ << endl
         << "License: " << appLicense_ << endl
         << "\u2550"sv * (appTitle_.size() + appVersion_.size() + 1) << endl
         << endl;

    cout << setw((int) titleWidth) << "Project" << ' ' << Vert << ' ' << setw((int) licenseWidth) << "License"
         << ' ' << Vert << ' ' << "Project URL" << endl;

    cout << Horiz * titleWidth << Horiz << Cross << Horiz << Horiz * licenseWidth << Horiz << Cross << Horiz
         << Horiz * urlWidth << endl;

    for (auto const& project: crispy::cli::about::store())
        cout << setw((int) titleWidth) << project.title << ' ' << Vert << ' ' << setw((int) licenseWidth)
             << project.license << ' ' << Vert << ' ' << project.url << endl;

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
        customizeLogStoreOutput();

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

void App::customizeLogStoreOutput()
{
    logstore::Sink::console().set_enabled(true);

    // A curated list of colors.
    static const bool colorized =
#if !defined(_WIN32)
        isatty(STDOUT_FILENO);
#else
        true;
#endif
    static constexpr auto colors = std::array<int, 23> {
        2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 150, 155, 159, 165, 170, 175, 180, 185, 190, 195, 200,
    };
    logstore::set_formatter([](logstore::MessageBuilder const& _msg) -> std::string {
        auto const [sgrTag, sgrMessage, sgrReset] = [&]() -> std::tuple<string, string, string> {
            if (!colorized)
                return { "", "", "" };
            auto const tagStart = "\033[1m";
            auto const colorIndex =
                colors.at(std::hash<string_view> {}(_msg.category().name()) % colors.size());
            auto const msgStart = fmt::format("\033[38;5;{}m", colorIndex);
            auto const resetSGR = fmt::format("\033[m");
            return { tagStart, msgStart, resetSGR };
        }();

#if 1
        auto const fileName = FileSystem::path(_msg.location().file_name()).filename().string();
#else
        // fileName with path to file relative to project root
        auto const srcIndex = string_view(_msg.location().file_name()).find("src");
        auto const fileName = string(srcIndex != string_view::npos
                                         ? string_view(_msg.location().file_name()).substr(srcIndex + 4)
                                         : string(_msg.location().file_name()));
#endif

        auto result = string {};

        for (auto const [i, line]: crispy::indexed(crispy::split(_msg.text(), '\n')))
        {
            if (i != 0)
                result += "        ";
            else
            {
                // clang-format off
                auto const now = chrono::system_clock::now();
                auto const micros =
                    duration_cast<chrono::microseconds>(now.time_since_epoch()).count() % 1'000'000;
                result += sgrTag;
                result += fmt::format("[{:%Y-%m-%d %H:%M:%S}.{:06}] [{}]",
                                      chrono::system_clock::now(),
                                      micros,
                                      _msg.category().name());
                result += sgrReset;
                result += ' ';
                // clang-format on
            }

            result += sgrMessage;
            result += line;
            result += sgrReset;
            result += '\n';
        }

        return result;
    });

    logstore::ErrorLog.set_formatter([](logstore::MessageBuilder const& _msg) -> std::string {
        auto const [sgrTag, sgrMessage, sgrReset] = [&]() -> std::tuple<string, string, string> {
            if (!colorized)
                return { "", "", "" };
            auto const tagStart = "\033[1;31m";
            auto const msgStart = "\033[31m";
            auto const resetSGR = "\033[m";
            return { tagStart, msgStart, resetSGR };
        }();

        auto result = string {};

        for (auto const [i, line]: crispy::indexed(crispy::split(_msg.text(), '\n')))
        {
            if (i != 0)
                result += "        ";
            else
            {
                result += sgrTag;
                result += fmt::format("[{}] ", "error");
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

} // namespace crispy
