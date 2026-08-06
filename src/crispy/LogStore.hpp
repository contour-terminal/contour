// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/Algorithm.hpp>
#include <crispy/Utils.hpp>

#include <gsl/pointers>

#include <algorithm>
#include <cassert>
#include <format>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef __has_include
    #if __cpp_lib_source_location
        #include <source_location>
    #endif
#endif

// NB: Don't do that now. It seems to only cause problems, such as
// __has_include reports presence and in can in fact be included, but it's
// not giving us the expected std::...SourceLocation, wow.
//
// #if __has_include(<source_location>) && !defined(_WIN32)
//     #include <source_location>
// #elif __has_include(<experimental/source_location>)
//     #include <experimental/source_location>
//     #define LOGSTORE_HAS_EXPERIMENTAL_SOURCE_LOCATION 1
// #endif

namespace logstore
{

class Category;
class Sink;

class SourceLocationCustom
{
  public:
    SourceLocationCustom(char const* filename, int line, char const* functionName) noexcept:
        _fileName { filename }, _line { line }, _functionName { functionName }
    {
    }

    // NOLINTNEXTLINE(readability-identifier-naming): drop-in for std::source_location.
    [[nodiscard]] char const* file_name() const noexcept { return _fileName; }
    [[nodiscard]] int line() const noexcept { return _line; }
    // NOLINTNEXTLINE(readability-identifier-naming): drop-in for std::source_location.
    [[nodiscard]] char const* function_name() const noexcept { return _functionName; }

    static SourceLocationCustom current() noexcept
    {
        return { __builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION() };
    }

  private:
    char const* _fileName;
    int _line;
    char const* _functionName;
};

#ifdef __has_include
    #ifdef __cpp_lib_source_location
using SourceLocation = std::source_location;
    #else
using SourceLocation = SourceLocationCustom;
    #endif
#endif

class MessageBuilder
{
  private:
    gsl::not_null<Category const*> _category;
    SourceLocation _location;
    std::string _buffer;

  public:
    explicit MessageBuilder(Category const& cat, SourceLocation loc = SourceLocation::current());

    [[nodiscard]] Category const& getCategory() const noexcept { return *_category; }
    [[nodiscard]] SourceLocation const& location() const noexcept { return _location; }

    [[nodiscard]] std::string const& text() const noexcept { return _buffer; }

    MessageBuilder& append(std::string_view msg)
    {
        _buffer += msg;
        return *this;
    }

    template <typename... Ts>
    MessageBuilder& append(std::string_view fmt, Ts const&... args)
    {
        _buffer += std::vformat(fmt, std::make_format_args(args...));
        return *this;
    }

    MessageBuilder& operator()(std::string const& msg)
    {
        _buffer += msg;
        return *this;
    }

    template <typename... Ts>
    MessageBuilder& operator()(std::string_view fmt, Ts const&... args)
    {
        _buffer += std::vformat(fmt, std::make_format_args(args...));
        return *this;
    }

    [[nodiscard]] std::string message() const;

    ~MessageBuilder();
};

/// Defines a logging category, such as: error, warning, metrics, vt.backend, or renderer.
///
/// A program can have multiple logging categories, all pointing to the same
/// or each to an individual logging sink.
class Category
{
  public:
    using Formatter = std::function<std::string(MessageBuilder const&)>;
    enum class State : uint8_t
    {
        Enabled,
        Disabled
    };
    enum class Visibility : uint8_t
    {
        Public,
        Hidden
    };

    Category(std::string_view name,
             std::string_view desc,
             State state = State::Disabled,
             Visibility visibility = Visibility::Public) noexcept;
    ~Category();

    [[nodiscard]] std::string_view name() const noexcept { return _name; }
    [[nodiscard]] std::string_view description() const noexcept { return _description; }

    [[nodiscard]] bool isEnabled() const noexcept { return _state == State::Enabled; }
    void enable(bool enabled = true) noexcept { _state = enabled ? State::Enabled : State::Disabled; }
    void disable() noexcept { _state = State::Disabled; }

    [[nodiscard]] bool visible() const noexcept { return _visibility == Visibility::Public; }
    void setVisible(bool visible) { _visibility = visible ? Visibility::Public : Visibility::Hidden; }

    operator bool() const noexcept { return isEnabled(); }

    [[nodiscard]] Formatter const& getFormatter() const { return _formatter; }
    void setFormatter(Formatter formatter) { _formatter = std::move(formatter); }

    void setSink(logstore::Sink& s) { _sink = s; }
    [[nodiscard]] logstore::Sink& sink() const noexcept { return _sink.get(); }

    [[nodiscard]] MessageBuilder build(SourceLocation location = SourceLocation::current()) const
    {
        return MessageBuilder(*this, location);
    }

    [[nodiscard]] MessageBuilder operator()(SourceLocation location = SourceLocation::current()) const
    {
        return MessageBuilder(*this, location);
    }

    static std::string defaultFormatter(MessageBuilder const& message);

  private:
    std::string_view _name;
    std::string_view _description;
    State _state;
    Visibility _visibility;
    Formatter _formatter;
    std::reference_wrapper<logstore::Sink> _sink;
};

/// Logging sink API.
///
/// Such as the console, a log file, or UDP endpoint.
class Sink
{
  public:
    using Writer = std::function<void(std::string_view const&)>;

    Sink(bool enabled, Writer writer);
    Sink(bool enabled, std::ostream& output);
    Sink(bool enabled, std::shared_ptr<std::ostream> f);

    void setWriter(Writer writer);

    /// Writes given built message to this sink.
    void write(MessageBuilder const& message);

    void setEnabled(bool enabled) { _enabled = enabled; }

    /// Retrieves reference to standard debug-logging sink.
    static Sink& console();
    static Sink& error_console(); // NOLINT(readability-identifier-naming)

  private:
    bool _enabled;
    Writer _writer;
};

std::vector<std::reference_wrapper<Category>>& get();
Category* get(std::string_view categoryName);
void setSink(Sink& sink);
void setFormatter(Category::Formatter const& f);
void enable(std::string_view categoryName, bool enabled = true);
void disable(std::string_view categoryName);
void configure(std::string_view filterString);

// {{{ implementation
inline std::string MessageBuilder::message() const
{
    if (_category->getFormatter())
        return _category->getFormatter()(*this);
    else if (!_buffer.empty() && _buffer.back() == '\n')
        return _buffer;
    else if (!_buffer.empty())
        return _buffer + '\n';
    else
        return "";
}

inline std::vector<std::reference_wrapper<Category>>& get()
{
    static std::vector<std::reference_wrapper<Category>> logStore;
    return logStore;
}

inline Category* get(std::string_view categoryName)
{
    for (auto const& cat: get())
        if (cat.get().name() == categoryName)
            return &cat.get();
    return nullptr;
}

inline void setSink(Sink& s)
{
    for (auto const& cat: get())
        cat.get().setSink(s);
}

inline void setFormatter(Category::Formatter const& f)
{
    for (auto const& cat: get())
        cat.get().setFormatter(f);
}

inline void enable(std::string_view categoryName, bool enabled)
{
    for (auto const& cat: get())
        if (cat.get().name() == categoryName)
            cat.get().enable(enabled);
}

inline void disable(std::string_view categoryName)
{
    enable(categoryName, false);
}

/// Decides whether one filter pattern selects a category.
///
/// The single definition of the filter grammar, so `configure()` and any caller that wants to
/// report unmatched patterns cannot disagree about what a pattern means.
/// @param pattern One filter element; a trailing `*` makes it a prefix match.
/// @param categoryName The category's name.
/// @return Whether @p pattern selects @p categoryName. An empty pattern selects nothing.
[[nodiscard]] inline bool matchesFilterPattern(std::string_view pattern,
                                               std::string_view categoryName) noexcept
{
    // An empty element ("a,,b") has no back() to inspect, and matches nothing.
    if (pattern.empty())
        return false;
    if (pattern.back() != '*')
        return categoryName == pattern;
    // TODO: '*' excludes hidden categories
    //
    // starts_with, not the three-iterator std::equal this used to be: that form bounds only the
    // PATTERN range, so a pattern longer than the category name (e.g. "vthost.*" against
    // "error") read past the end of the name.
    return categoryName.starts_with(pattern.substr(0, pattern.size() - 1));
}

/// Applies a category filter: "all", or a comma-separated list of names each with an optional
/// trailing `*` prefix wildcard.
///
/// This is a SELECTION, not a union — every category the filter does not name is DISABLED.
/// The `error` category is deliberately exempt: errors are not a debug tier, and asking for
/// extra detail (`--log vthost.trace.proto`, `LOG=vt.parser`) must never take failure reporting
/// away. Every caller gets that guarantee, rather than each remembering to restore it.
///
/// @param filterString The filter to apply; an empty string disables everything but `error`.
inline void configure(std::string_view filterString)
{
    // Restored at the end, so an explicit "error" in the filter is not the only way to keep it.
    if (filterString == "all")
    {
        for (auto& category: logstore::get())
            category.get().enable();
    }
    else
    {
        auto const filters = crispy::split(filterString, ',');
        for (auto& category: logstore::get())
        {
            category.get().enable(crispy::any_of(filters, [&](std::string_view filterPattern) {
                return matchesFilterPattern(filterPattern, category.get().name());
            }));
        }
    }

    // By NAME rather than by touching the errorLog object, which is declared further down this
    // header; the registry lookup needs no declaration order.
    enable("error");
}

inline MessageBuilder::MessageBuilder(logstore::Category const& cat, SourceLocation location):
    _category { &cat }, _location { location }
{
}

inline MessageBuilder::~MessageBuilder()
{
    _category->sink().write(*this);
}

inline Category::Category(std::string_view name,
                          std::string_view desc,
                          State state,
                          Visibility visibility) noexcept:
    _name { name },
    _description { desc },
    _state { state },
    _visibility { visibility },
    _sink { logstore::Sink::console() }
{
    assert(std::none_of(get().begin(), get().end(), [&](Category const& x) { return x.name() == _name; }));
    get().emplace_back(*this);
}

inline Category::~Category()
{
    for (auto i = get().begin(), e = get().end(); i != e; ++i)
    {
        if (&i->get() == this)
        {
            get().erase(i);
            break;
        }
    }
}

inline std::string Category::defaultFormatter(MessageBuilder const& message)
{
    return std::format("[{}:{}:{}]: {}\n",
                       message.getCategory().name(),
                       message.location().file_name(),
                       message.location().line(),
                       message.text());
}

inline void Sink::write(MessageBuilder const& message)
{
    if (_enabled && message.getCategory().isEnabled())
        _writer(message.message());
}

inline void Sink::setWriter(Writer writer)
{
    _writer = std::move(writer);
}
// }}}

auto inline errorLog = logstore::Category("error", "Error Logger", Category::State::Enabled);

#define errorLog() (::logstore::errorLog())

} // namespace logstore
