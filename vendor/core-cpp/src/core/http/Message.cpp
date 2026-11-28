// SPDX-License-Identifier: Apache-2.0
#include <core/http/Message.hpp>

#include <core/http/detail/Syntax.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::http
{

namespace
{

    /// Whether @p text is `scheme ":" "//" ...` with URI characters after it (an absolute URI with an
    /// authority, as HTTP's are).
    bool isAbsoluteUri(std::string_view text)
    {
        auto const colon = text.find(':');
        if (colon == std::string_view::npos || colon == 0
            || !detail::isIn(text.front(), detail::CharClass::Alpha))
            return false;
        auto const scheme = text.substr(0, colon);
        auto const rest = text.substr(colon + 1);
        return detail::isUriText(scheme, detail::CharClass::Alpha | detail::CharClass::Digit, "+-.")
               && rest.starts_with("//")
               && detail::isUriText(
                   rest, detail::CharClass::Unreserved | detail::CharClass::SubDelim, ":@/?[]");
    }

} // namespace

std::string HttpError::toString() const
{
    auto result = std::string { http::toString(code) };
    if (fieldName)
        result += " [field " + *fieldName + "]";
    if (size)
        result += " [size " + std::to_string(*size) + "]";
    if (coding)
        result += " [coding " + *coding + "]";
    if (dnsRcode)
        result += " [rcode " + *dnsRcode + "]";
    if (dnsInfoCode)
        result += " [info code " + std::to_string(*dnsInfoCode) + "]";
    if (tls && !tls->message.empty())
        result += " [TLS " + tls->message + "]";
    if (!detail.empty())
        result += " (" + detail + ")";
    return result;
}

HttpResult<void> Fields::append(std::string name, std::string value)
{
    if (!detail::isToken(name))
        return std::unexpected(
            detail::refusal(HttpErrorCode::ProtocolError, "a field name is not a token", std::move(name)));
    if (!detail::allIn(value, detail::CharClass::FieldValue))
        return std::unexpected(
            detail::refusal(HttpErrorCode::ProtocolError,
                            "a field value holds a control character (CR, LF, NUL or another)",
                            std::move(name)));
    if (detail::trimWhitespace(value).size() != value.size())
        return std::unexpected(detail::refusal(HttpErrorCode::ProtocolError,
                                               "a field value has leading or trailing whitespace",
                                               std::move(name)));
    _entries.push_back(Field { .name = std::move(name), .value = std::move(value) });
    return {};
}

std::vector<std::string_view> Fields::get(std::string_view name) const
{
    auto values = std::vector<std::string_view> {};
    for (auto const& entry: _entries)
        if (detail::equalsIgnoringCase(entry.name, name))
            values.emplace_back(entry.value);
    return values;
}

std::size_t Fields::erase(std::string_view name)
{
    return std::erase_if(_entries,
                         [name](Field const& entry) { return detail::equalsIgnoringCase(entry.name, name); });
}

Method::Method(): _text { "GET" }
{
}

Method::Method(std::string text): _text { std::move(text) }
{
}

HttpResult<Method> Method::parse(std::string_view token)
{
    if (!detail::isToken(token))
        return std::unexpected(
            detail::refusal(HttpErrorCode::RequestMethodInvalid, "a method is not a token"));
    return Method { std::string { token } };
}

RequestTarget::RequestTarget(): _text { "/" }, _form { RequestTargetForm::Origin }
{
}

RequestTarget::RequestTarget(std::string text, RequestTargetForm form):
    _text { std::move(text) }, _form { form }
{
}

HttpResult<RequestTarget> RequestTarget::parse(std::string_view target)
{
    auto const form = [target]() -> std::optional<RequestTargetForm> {
        if (target == "*")
            return RequestTargetForm::Asterisk;
        if (target.starts_with('/'))
        {
            // origin-form = absolute-path [ "?" query ]: pchar, '/' and '?'.
            if (detail::isUriText(
                    target, detail::CharClass::Unreserved | detail::CharClass::SubDelim, ":@/?"))
                return RequestTargetForm::Origin;
            return std::nullopt;
        }
        if (isAbsoluteUri(target))
            return RequestTargetForm::Absolute;
        if (detail::isAuthority(target, detail::PortRule::Required))
            return RequestTargetForm::Authority;
        return std::nullopt;
    }();
    if (!form)
        return std::unexpected(detail::refusal(HttpErrorCode::RequestUriInvalid,
                                               "a request target is not one of the four forms"));
    return RequestTarget { std::string { target }, *form };
}

ReasonPhrase::ReasonPhrase(std::string text): _text { std::move(text) }
{
}

HttpResult<ReasonPhrase> ReasonPhrase::parse(std::string_view text)
{
    if (!detail::allIn(text, detail::CharClass::FieldValue))
        return std::unexpected(
            detail::refusal(HttpErrorCode::ProtocolError, "a reason phrase holds a control character"));
    return ReasonPhrase { std::string { text } };
}

} // namespace core::http
