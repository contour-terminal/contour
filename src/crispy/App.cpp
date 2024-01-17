// SPDX-License-Identifier: Apache-2.0
#include <crispy/App.h>
#include <crispy/logstore.h>
#include <crispy/utils.h>

#include <fmt/chrono.h>

#include <range/v3/view/enumerate.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iomanip>
#include <numeric>
#include <optional>

#if !defined(_WIN32)
    #include <sys/ioctl.h>

    #include <pwd.h>
    #include <unistd.h>
#endif

using std::bind;
using std::cout;
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
namespace fs = std::filesystem;

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

CLI::help_display_style helpStyle()
{
    auto style = CLI::help_display_style {};

    style.optionStyle = CLI::option_style::Natural;

#if !defined(_WIN32)
    if (isatty(STDOUT_FILENO) == 0)
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

fs::path xdgStateHome()
{
    if (auto const* p = getenv("XDG_STATE_HOME"); (p != nullptr) && (*p != '\0'))
        return fs::path(p);

#if defined(_WIN32)
    if (auto const* p = getenv("LOCALAPPDATA"); p && *p)
        return fs::path(p);
#else
    if (passwd const* pw = getpwuid(getuid()); (pw != nullptr) && (pw->pw_dir != nullptr))
        return fs::path(pw->pw_dir) / ".local" / "state";
#endif

    return fs::temp_directory_path();
}
} // namespace

namespace crispy
{

app* app::_instance = nullptr;

app::app(std::string appName, std::string appTitle, std::string appVersion, std::string appLicense):
    _appName { std::move(appName) },
    _appTitle { std::move(appTitle) },
    _appVersion { std::move(appVersion) },
    _appLicense { std::move(appLicense) },
    _localStateDir { xdgStateHome() / _appName }
{
    if (char const* logFilterString = getenv("LOG"))
    {
        logstore::configure(logFilterString);
        customizeLogStoreOutput();
    }

    _instance = this;

    link(_appName + ".help", bind(&app::helpAction, this));
    link(_appName + ".version", bind(&app::versionAction, this));
    link(_appName + ".license", bind(&app::licenseAction, this));
}

app::~app()
{
    _instance = nullptr;
}

void app::link(std::string command, std::function<int()> handler)
{
    _handlers[std::move(command)] = std::move(handler);
}

void app::listDebugTags()
{
    auto& categories = logstore::get();
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

int app::helpAction()
{
    std::cout << CLI::helpText(_syntax.value(), helpStyle(), screenWidth());
    return EXIT_SUCCESS;
}

int app::licenseAction()
{
    auto const& store = crispy::cli::about::store();
    auto const titleWidth = std::accumulate(
        store.begin(), store.end(), 0u, [](size_t a, auto const& b) { return std::max(a, b.title.size()); });
    auto const licenseWidth = std::accumulate(store.begin(), store.end(), 0u, [](size_t a, auto const& b) {
        return std::max(a, b.license.size());
    });
    auto const urlWidth = std::accumulate(
        store.begin(), store.end(), 0u, [](size_t a, auto const& b) { return std::max(a, b.url.size()); });

    constexpr auto Horiz = "\u2550"sv;
    constexpr auto Vert = "\u2502"sv;
    constexpr auto Cross = "\u256A"sv;

    cout << '\n'
         << _appTitle << ' ' << _appVersion << '\n'
         << "License: " << _appLicense << '\n'
         << "\u2550"sv * (_appTitle.size() + _appVersion.size() + 1) << '\n'
         << '\n';

    cout << setw((int) titleWidth) << "Project" << ' ' << Vert << ' ' << setw((int) licenseWidth) << "License"
         << ' ' << Vert << ' ' << "Project URL" << '\n';

    cout << Horiz * titleWidth << Horiz << Cross << Horiz << Horiz * licenseWidth << Horiz << Cross << Horiz
         << Horiz * urlWidth << '\n';

    for (auto const& project: crispy::cli::about::store())
        cout << setw((int) titleWidth) << project.title << ' ' << Vert << ' ' << setw((int) licenseWidth)
             << project.license << ' ' << Vert << ' ' << project.url << '\n';

    return EXIT_SUCCESS;
}

int app::versionAction()
{
    std::cout << fmt::format("{} {}\n\n", _appTitle, _appVersion);
    return EXIT_SUCCESS;
}

// customize debuglog transform to shorten the file_name output a bit
int app::run(int argc, char const* argv[])
{
    try
    {
        customizeLogStoreOutput();

        _syntax = parameterDefinition();

        optional<CLI::flag_store> flagsOpt = CLI::parse(_syntax.value(), argc, argv);
        if (!flagsOpt.has_value())
        {
            std::cerr << "Failed to parse command line parameters.\n";
            return EXIT_FAILURE;
        }
        _flags = std::move(flagsOpt.value());

        // std::cout << fmt::format("Flags: {}\n", parameters().values.size());
        // for (auto const & [k, v] : parameters().values)
        //     std::cout << fmt::format(" - {}: {}\n", k, v);

        for (auto const& [name, handler]: _handlers)
            if (parameters().get<bool>(name))
                return handler();

        std::cerr << "Usage error." << '\n';
        return EXIT_FAILURE;
    }
    catch (exception const& e)
    {
        std::cerr << fmt::format("Unhandled error caught. {}", e.what()) << '\n';
        return EXIT_FAILURE;
    }
}

void app::customizeLogStoreOutput()
{
    logstore::sink::console().set_enabled(true);

    // A curated list of colors.
    static const bool colorized =
#if !defined(_WIN32)
        isatty(STDOUT_FILENO) != 0;
#else
        true;
#endif
    static constexpr auto Colors = std::array<int, 23> {
        2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 150, 155, 159, 165, 170, 175, 180, 185, 190, 195, 200,
    };
    logstore::set_formatter([](logstore::message_builder const& msg) -> std::string {
        auto const [sgrTag, sgrMessage, sgrReset] = [&]() -> std::tuple<string, string, string> {
            if (!colorized)
                return { "", "", "" };
            const auto* const tagStart = "\033[1m";
            auto const colorIndex =
                Colors.at(std::hash<string_view> {}(msg.get_category().name()) % Colors.size());
            auto const msgStart = fmt::format("\033[38;5;{}m", colorIndex);
            auto const resetSGR = fmt::format("\033[m");
            return { tagStart, msgStart, resetSGR };
        }();

        auto const fileName = fs::path(msg.location().file_name()).filename().string();

        // // fileName with path to file relative to project root
        // auto const srcIndex = string_view(msg.location().file_name()).find("src");
        // auto const fileName = string(srcIndex != string_view::npos
        //                                  ? string_view(msg.location().file_name()).substr(srcIndex + 4)
        //                                  : string(msg.location().file_name()));

        auto result = string {};

        auto const lines = crispy::split(msg.text(), '\n');
        for (auto const [i, line]: ranges::views::enumerate(lines))
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
                                      msg.get_category().name());
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

    logstore::errorLog.set_formatter([](logstore::message_builder const& msg) -> std::string {
        auto const [sgrTag, sgrMessage, sgrReset] = [&]() -> std::tuple<string, string, string> {
            if (!colorized)
                return { "", "", "" };
            const auto* const tagStart = "\033[1;31m";
            const auto* const msgStart = "\033[31m";
            const auto* const resetSGR = "\033[m";
            return { tagStart, msgStart, resetSGR };
        }();

        auto result = string {};
        auto const lines = crispy::split(msg.text(), '\n');
        for (auto const [i, line]: ranges::views::enumerate(lines))
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
