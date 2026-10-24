// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/Wit.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/EnumTable.hpp>
#include <vtcontrol/Reflect.hpp>

namespace vtcontrol
{

namespace
{
    using namespace std::string_view_literals;

    /// WIT's keywords: an identifier spelled like one is escaped with `%`.
    constexpr auto WitKeywords = std::array {
        "use"sv,      "type"sv,   "func"sv,   "u8"sv,     "u16"sv,     "u32"sv,         "u64"sv,
        "s8"sv,       "s16"sv,    "s32"sv,    "s64"sv,    "f32"sv,     "f64"sv,         "char"sv,
        "resource"sv, "own"sv,    "borrow"sv, "record"sv, "flags"sv,   "variant"sv,     "enum"sv,
        "bool"sv,     "string"sv, "option"sv, "result"sv, "future"sv,  "stream"sv,      "error-context"sv,
        "list"sv,     "map"sv,    "_"sv,      "as"sv,     "from"sv,    "static"sv,      "interface"sv,
        "tuple"sv,    "world"sv,  "import"sv, "export"sv, "package"sv, "constructor"sv, "include"sv,
        "with"sv,     "async"sv,
    };

    /// @return @p name as a WIT identifier: kebab-case, `%`-escaped when it is a keyword.
    [[nodiscard]] std::string identifier(std::string_view name)
    {
        auto kebab = kebabCase(name);
        if (std::ranges::contains(WitKeywords, std::string_view { kebab }))
            return "%" + kebab;
        return kebab;
    }

    /// The unqualified name of a type, for the WIT declaration of a record or an enum.
    template <typename T>
    [[nodiscard]] std::string typeIdentifier()
    {
        return identifier(reflect::TypeName<T>);
    }

    /// Whether a record has no fields, and so is never declared (WIT has no empty record).
    template <typename T>
    inline constexpr bool IsEmptyRecord = [] {
        if constexpr (ApiRecord<T>)
            return reflect::FieldCount<T> == 0;
        else
            return false;
    }();

    /// What a type is spelled as where it is used: a field's type, a parameter, a result.
    template <typename T>
    [[nodiscard]] std::string witType()
    {
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, RawJson>)
            return "string";
        else if constexpr (std::is_same_v<T, bool>)
            return "bool";
        else if constexpr (std::is_same_v<T, uint8_t>)
            return "u8";
        else if constexpr (std::is_same_v<T, uint32_t>)
            return "u32";
        else if constexpr (std::is_same_v<T, int32_t>)
            return "s32";
        else if constexpr (reflect::IsOptional<T>)
            return std::format("option<{}>", witType<typename T::value_type>());
        else if constexpr (reflect::IsVector<T>)
            return std::format("list<{}>", witType<typename T::value_type>());
        else if constexpr (HasEnumTable<T>)
            return typeIdentifier<T>();
        else if constexpr (ApiRecord<T> || std::is_same_v<T, ControlError>)
        {
            static_assert(!IsEmptyRecord<T>, "WIT has no empty record; an empty one cannot be a field");
            return typeIdentifier<T>();
        }
        else
            static_assert(false, "this type has no WIT spelling: add a row to witType()");
    }

    /// Declares the records and enums of `interface types`, each once and before its first use.
    class TypesRenderer
    {
      public:
        /// Declares @p T and, before it, everything it uses that is not declared yet.
        template <typename T>
        void declare()
        {
            if constexpr (reflect::IsOptional<T> || reflect::IsVector<T>)
                declare<typename T::value_type>();
            else if constexpr (HasEnumTable<T>)
            {
                if (!firstSight<T>())
                    return;
                _text += std::format("    enum {} {{\n", typeIdentifier<T>());
                for (auto const& row: EnumTable<T>::Rows)
                    _text += std::format("        {},\n", identifier(row.name));
                _text += "    }\n\n";
            }
            else if constexpr ((ApiRecord<T> || std::is_same_v<T, ControlError>) && !IsEmptyRecord<T>)
            {
                if (!firstSight<T>())
                    return;
                auto fields = std::string {};
                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    (declare<reflect::FieldType<T, I>>(), ...);
                    ((fields += std::format("        {}: {},\n",
                                            identifier(reflect::FieldName<T, I>),
                                            witType<reflect::FieldType<T, I>>())),
                     ...);
                }(std::make_index_sequence<reflect::FieldCount<T>> {});
                _text += std::format("    record {} {{\n{}    }}\n\n", typeIdentifier<T>(), fields);
            }
        }

        /// @return The declarations, each followed by a blank line.
        [[nodiscard]] std::string const& text() const noexcept { return _text; }

      private:
        /// @return Whether @p T was not declared yet; it counts as declared from now on.
        template <typename T>
        [[nodiscard]] bool firstSight()
        {
            return _declared.insert(typeIdentifier<T>()).second;
        }

        std::set<std::string> _declared;
        std::string _text;
    };

    /// Appends the types a func of @p Row names, which `interface control` uses from `types`, to @p used.
    template <typename Row>
    void appendUsedTypes(std::vector<std::string>& used)
    {
        auto const add = [&]<typename T>(std::type_identity<T>) {
            if constexpr (!IsEmptyRecord<T>)
                if (auto name = typeIdentifier<T>(); !std::ranges::contains(used, name))
                    used.push_back(std::move(name));
        };
        add(std::type_identity<typename Row::Request> {});
        add(std::type_identity<typename Row::Reply> {});
    }

    /// @return What @p row needs granted, as the sentence its doc comment ends with: the method's
    ///         permission, and the one its requestGrant rule asks for when a request does what it guards.
    template <typename Row>
    [[nodiscard]] std::string grantSentence(Row const& row)
    {
        if (!row.requestGrant)
            return std::format("Needs `{}`.", nameOf(row.permission));
        return std::format("Needs `{}`, and `{}` as well for {}.",
                           nameOf(row.permission),
                           nameOf(row.requestGrant->permission),
                           row.requestGrant->reason);
    }

    /// @return The func of @p row in `interface control`, with its summary and what it needs granted as
    ///         the doc comment.
    template <typename Row>
    [[nodiscard]] std::string func(Row const& row)
    {
        using Request = Row::Request;
        using Reply = Row::Reply;
        auto parameters = std::string {};
        if constexpr (!IsEmptyRecord<Request>)
            parameters = std::format("request: {}", witType<Request>());
        auto ok = std::string { "_" };
        if constexpr (!IsEmptyRecord<Reply>)
            ok = witType<Reply>();
        return std::format("    /// {}\n    /// {}\n    {}: func({}) -> result<{}, control-error>;\n",
                           row.summary,
                           grantSentence(row),
                           identifier(row.name),
                           parameters,
                           ok);
    }

    template <typename Row>
    [[nodiscard]] constexpr bool isEverywhere(Row const& row) noexcept
    {
        return row.exposure == Exposure::Everywhere;
    }
} // namespace

std::string kebabCase(std::string_view name)
{
    auto result = std::string {};
    result.reserve(name.size() + 4);
    for (auto const ch: name)
    {
        auto const byte = static_cast<unsigned char>(ch);
        if (ch == '.' || ch == '_')
            result += '-';
        else if (std::isupper(byte) != 0)
        {
            if (!result.empty() && result.back() != '-')
                result += '-';
            result += static_cast<char>(std::tolower(byte));
        }
        else
            result += ch;
    }
    return result;
}

std::string renderControlWit()
{
    auto types = TypesRenderer {};
    auto used = std::vector<std::string> {};
    auto funcs = std::string {};
    std::apply(
        [&](auto const&... row) {
            auto const render = [&]<typename Row>(Row const& method) {
                if (!isEverywhere(method))
                    return;
                types.declare<typename Row::Request>();
                types.declare<typename Row::Reply>();
                appendUsedTypes<Row>(used);
                funcs += (funcs.empty() ? "" : "\n") + func(method);
            };
            (render(row), ...);
        },
        Methods);
    types.declare<ControlError>();
    used.emplace_back("control-error");

    auto useList = std::string {};
    for (auto const& name: used)
        useList += std::format("{}        {}", useList.empty() ? "" : ",\n", name);

    auto typesText = types.text();
    typesText.pop_back(); // The blank line after the last declaration.

    return std::format(R"(// SPDX-License-Identifier: Apache-2.0
//
// Generated from the host's method catalog; regenerate it rather than edit it (see README.md).

/// The control verbs: what an extension may ask its terminal to do with windows, tabs and panes.
///
/// The package is terminal-neutral: any terminal can implement the host side. README.md says how an
/// extension imports it, and which permissions each verb needs.
package {};

/// The records and enums the control verbs take and return.
interface types {{
{}}}

/// One func per verb. Each fails with a `control-error` rather than trapping.
interface control {{
    use types.{{
{}
    }};

{}}}

/// A world for an extension that drives its terminal. An extension that also draws a status line
/// includes its own world and imports `terminal-extensions:control/control@0.1.0` beside it.
world controller {{
    import control;
}}
)",
                       ControlWitPackage,
                       typesText,
                       useList,
                       funcs);
}

} // namespace vtcontrol
