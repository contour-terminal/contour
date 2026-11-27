// SPDX-License-Identifier: Apache-2.0
#include <core/http/Framing.hpp>

#include <core/Assert.hpp>
#include <core/http/detail/Syntax.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace core::http
{

namespace
{

    // ---------------------------------------------------------------------------------------------
    // What differs between a request and a response: the codes a limit is reported with, and two
    // syntax rules RFC 9112 words differently for a server and for a user agent.

    /// What a parser does with an obs-fold line (RFC 9112 section 5.2).
    enum class ObsFold : std::uint8_t
    {
        Refuse, ///< A server rejects the message.
        Unfold, ///< A user agent replaces the fold with SP.
    };

    /// What a parser does with empty lines before the start line (RFC 9112 section 2.2).
    enum class LeadingEmptyLines : std::uint8_t
    {
        Refuse, ///< Nothing allows them.
        Skip,   ///< A server ignores them before a request line.
    };

    /// The rules of one @c MessageRole.
    struct RoleRules
    {
        MessageRole role;
        HttpErrorCode headSection;      ///< The head is past maxHeadBytes.
        HttpErrorCode startLineTooLong; ///< The start line is past maxFieldBytes.
        HttpErrorCode fieldTooLong;     ///< A field line is past maxFieldBytes.
        HttpErrorCode trailerSection;   ///< The trailers are past maxTrailerBytes.
        HttpErrorCode trailerTooLong;   ///< A trailer line is past maxFieldBytes.
        HttpErrorCode bodySize;         ///< The body is past maxBodyBytes.
        ObsFold obsFold;
        LeadingEmptyLines leadingEmptyLines;
    };

    constexpr auto RoleTable = std::array {
        RoleRules { .role = MessageRole::Request,
                    .headSection = HttpErrorCode::RequestHeaderSectionSize,
                    .startLineTooLong = HttpErrorCode::RequestUriTooLong,
                    .fieldTooLong = HttpErrorCode::RequestHeaderSize,
                    .trailerSection = HttpErrorCode::RequestTrailerSectionSize,
                    .trailerTooLong = HttpErrorCode::RequestTrailerSize,
                    .bodySize = HttpErrorCode::RequestBodySize,
                    .obsFold = ObsFold::Refuse,
                    .leadingEmptyLines = LeadingEmptyLines::Skip },
        RoleRules { .role = MessageRole::Response,
                    .headSection = HttpErrorCode::ResponseHeaderSectionSize,
                    .startLineTooLong = HttpErrorCode::ProtocolError,
                    .fieldTooLong = HttpErrorCode::ResponseHeaderSize,
                    .trailerSection = HttpErrorCode::ResponseTrailerSectionSize,
                    .trailerTooLong = HttpErrorCode::ResponseTrailerSize,
                    .bodySize = HttpErrorCode::ResponseBodySize,
                    .obsFold = ObsFold::Unfold,
                    .leadingEmptyLines = LeadingEmptyLines::Refuse },
    };

    [[nodiscard]] RoleRules const& rulesFor(MessageRole role)
    {
        auto const& rules = RoleTable.at(static_cast<std::size_t>(role));
        Require(rules.role == role);
        return rules;
    }

    // ---------------------------------------------------------------------------------------------
    // Field lines, shared by the head and the trailer section.

    /// The field name at the start of a (possibly partial) field line, if its colon has been seen.
    [[nodiscard]] std::optional<std::string> nameOf(std::string_view line)
    {
        auto const colon = line.find(':');
        if (colon == std::string_view::npos)
            return std::nullopt;
        return std::string { line.substr(0, colon) };
    }

    /// The bound of the section a field line goes into: how many lines it may hold, and the code for
    /// one more.
    struct FieldSection
    {
        std::size_t maxFields;
        HttpErrorCode tooMany;
    };

    /// Appends the field line held back for a possible obs-fold, if any, within @p section's count.
    [[nodiscard]] HttpResult<void> flushField(std::optional<Field>& pending,
                                              Fields& into,
                                              FieldSection section)
    {
        if (!pending)
            return {};
        if (into.entries().size() >= section.maxFields)
            return std::unexpected(
                detail::refusal(section.tooMany, "more than maxFields field lines", pending->name));
        auto field = std::move(*pending);
        pending.reset();
        return into.append(std::move(field.name), std::move(field.value));
    }

    /// Takes one complete field line: an obs-fold continues the held-back field (or is refused, by
    /// role), any other line appends the held-back one and is held back in its turn.
    [[nodiscard]] HttpResult<void> takeFieldLine(std::string_view line,
                                                 RoleRules const& rules,
                                                 FieldSection section,
                                                 std::optional<Field>& pending,
                                                 Fields& into)
    {
        if (detail::isIn(line.front(), detail::CharClass::Whitespace))
        {
            if (!pending)
                return std::unexpected(
                    detail::refusal(HttpErrorCode::ProtocolError, "whitespace before the first field line"));
            if (rules.obsFold == ObsFold::Refuse)
                return std::unexpected(
                    detail::refusal(HttpErrorCode::ProtocolError, "an obs-fold line", pending->name));
            auto const continuation = detail::trimWhitespace(line);
            if (!continuation.empty())
                pending->value += (pending->value.empty() ? "" : " ") + std::string { continuation };
            return {};
        }
        if (auto flushed = flushField(pending, into, section); !flushed)
            return flushed;
        auto const colon = line.find(':');
        if (colon == std::string_view::npos)
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError, "a field line without a colon"));
        // Whitespace between the name and the colon (RFC 9112 section 5.1) makes the name no token, so
        // Fields::append refuses it with every other malformed name.
        auto const name = line.substr(0, colon);
        pending = Field { .name = std::string { name },
                          .value = std::string { detail::trimWhitespace(line.substr(colon + 1)) } };
        return {};
    }

    // ---------------------------------------------------------------------------------------------
    // Start lines.

    /// The version of an `HTTP/x.y` token: major 1 only, and any minor past 0 is HTTP/1.1.
    [[nodiscard]] std::optional<HttpVersion> parseVersion(std::string_view text)
    {
        constexpr auto Prefix = std::string_view { "HTTP/1." };
        if (text.size() != Prefix.size() + 1 || !text.starts_with(Prefix)
            || !detail::isIn(text.back(), detail::CharClass::Digit))
            return std::nullopt;
        return text.back() == '0' ? HttpVersion::Http10 : HttpVersion::Http11;
    }

    /// The refusal of a start line whose version is not HTTP/1.x.
    [[nodiscard]] HttpError unsupportedVersion()
    {
        return detail::refusal(HttpErrorCode::ProtocolError,
                               "an unsupported HTTP version",
                               std::nullopt,
                               std::nullopt,
                               RefusalReason::UnsupportedVersion);
    }

    /// Whether a method may use only its own target form, or others besides.
    enum class MethodForms : std::uint8_t
    {
        Any,      ///< The method may use the other forms too.
        OnlyThis, ///< The method uses this form and no other.
    };

    /// A request-target form only one method may use (RFC 9112 sections 3.2.3 and 3.2.4).
    struct TargetFormRule
    {
        RequestTargetForm form;
        std::string_view method;
        MethodForms methodForms;
    };

    /// Authority-form is CONNECT's, and CONNECT has no other; `*` is OPTIONS', which may also name a
    /// resource. Origin- and absolute-form are any other method's.
    constexpr auto TargetFormRules = std::array {
        TargetFormRule {
            .form = RequestTargetForm::Authority, .method = "CONNECT", .methodForms = MethodForms::OnlyThis },
        TargetFormRule {
            .form = RequestTargetForm::Asterisk, .method = "OPTIONS", .methodForms = MethodForms::Any },
    };

    /// Refuses a request target whose form its method may not use.
    [[nodiscard]] HttpResult<void> checkTargetForm(std::string_view method, RequestTargetForm form)
    {
        for (auto const& rule: TargetFormRules)
        {
            auto const formIsRules = form == rule.form;
            auto const methodIsRules = method == rule.method;
            if (formIsRules != methodIsRules && (formIsRules || rule.methodForms == MethodForms::OnlyThis))
                return std::unexpected(
                    detail::refusal(HttpErrorCode::RequestUriInvalid,
                                    std::format("a request-target form {} may not use", method)));
        }
        return {};
    }

    /// Whether @p host is a valid Host field value (RFC 9112 section 3.2): an authority, possibly
    /// empty.
    [[nodiscard]] bool isHostValue(std::string_view host)
    {
        return host.empty() || detail::isAuthority(host, detail::PortRule::Optional);
    }

    // ---------------------------------------------------------------------------------------------
    // Body framing (RFC 9112 section 6.3): the fields are classified into shapes, and the first row
    // of FramingTable that matches the shapes decides.

    /// The shape of the Transfer-Encoding fields.
    enum class TransferShape : std::uint8_t
    {
        Absent,       ///< No Transfer-Encoding.
        Chunked,      ///< Exactly `chunked`: one coding, no parameter.
        CodedChunked, ///< A valid list whose final coding is chunked, after other codings or parameters.
        OtherLast,    ///< A valid list whose final coding is not chunked.
        Invalid,      ///< Empty, not tokens, an unterminated quote, chunked twice or with a parameter.
    };

    /// The shape of the Content-Length fields.
    enum class LengthShape : std::uint8_t
    {
        Absent,  ///< No Content-Length.
        Valid,   ///< One decimal value, possibly repeated.
        Invalid, ///< Anything else.
    };

    /// What of the request method decides a response's framing.
    enum class MethodShape : std::uint8_t
    {
        Other,
        Head,
        Connect,
    };

    /// What of the status decides a response's framing.
    enum class StatusShape : std::uint8_t
    {
        Other,
        Informational, ///< 1xx.
        NoContent,     ///< 204, 304.
        Success,       ///< Any other 2xx.
    };

    /// What a matching row decides.
    enum class Verdict : std::uint8_t
    {
        NoBody,
        Length,
        Chunked,
        UntilClose,
        Refuse,
    };

    /// Whether a row overrides the persistence the version and Connection field give.
    enum class PersistenceRule : std::uint8_t
    {
        FromFields, ///< As the version and the Connection field say (RFC 9112 section 9.3).
        Close,      ///< The connection closes.
    };

    /// The bit of @p value in a row's mask.
    template <typename Enum>
    [[nodiscard]] constexpr std::uint8_t bit(Enum value) noexcept
    {
        return static_cast<std::uint8_t>(1U << static_cast<unsigned>(value));
    }

    /// A row's mask that matches every value.
    constexpr std::uint8_t AnyValue = 0b1111'1111;

    /// One rule: masks over role, version, method, status, Transfer-Encoding and Content-Length,
    /// then what it decides.
    struct FramingRow
    {
        std::uint8_t roles;
        std::uint8_t versions;
        std::uint8_t methods;
        std::uint8_t statuses;
        std::uint8_t transfers;
        std::uint8_t lengths;
        Verdict verdict;
        /// Close on every Refuse row: a refusal is fatal to the connection, which the caller answers
        /// (if it can) and closes, so the refusal itself carries no Framing.
        PersistenceRule persistence;
        std::string_view why;
        /// What a Refuse row refuses for, beyond @c HttpErrorCode::ProtocolError; Unclassified elsewhere.
        RefusalReason reason = RefusalReason::Unclassified;
    };

    constexpr auto Request = bit(MessageRole::Request);
    constexpr auto Response = bit(MessageRole::Response);
    constexpr auto TransferPresent = bit(TransferShape::Chunked) | bit(TransferShape::CodedChunked)
                                     | bit(TransferShape::OtherLast) | bit(TransferShape::Invalid);
    constexpr auto ChunkedFinal = bit(TransferShape::Chunked) | bit(TransferShape::CodedChunked);
    constexpr auto LengthPresent = bit(LengthShape::Valid) | bit(LengthShape::Invalid);

    /// RFC 9112 section 6.3, in its order, with sections 6.1 and 7 folded in; the first match wins.
    constexpr auto FramingTable = std::array {
        FramingRow { Response,
                     AnyValue,
                     bit(MethodShape::Head),
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     Verdict::NoBody,
                     PersistenceRule::FromFields,
                     "a response to HEAD has no body" },
        FramingRow { Response,
                     AnyValue,
                     AnyValue,
                     bit(StatusShape::Informational),
                     AnyValue,
                     AnyValue,
                     Verdict::NoBody,
                     PersistenceRule::FromFields,
                     "a 1xx response has no body" },
        FramingRow { Response,
                     AnyValue,
                     AnyValue,
                     bit(StatusShape::NoContent),
                     AnyValue,
                     AnyValue,
                     Verdict::NoBody,
                     PersistenceRule::FromFields,
                     "a 204 or 304 response has no body" },
        FramingRow { Response,
                     AnyValue,
                     bit(MethodShape::Connect),
                     bit(StatusShape::Success),
                     AnyValue,
                     AnyValue,
                     Verdict::NoBody,
                     PersistenceRule::Close,
                     "a 2xx to CONNECT turns the connection into a tunnel" },
        FramingRow { AnyValue,
                     bit(HttpVersion::Http10),
                     AnyValue,
                     AnyValue,
                     TransferPresent,
                     AnyValue,
                     Verdict::Refuse,
                     PersistenceRule::Close,
                     "Transfer-Encoding in an HTTP/1.0 message" },
        FramingRow { AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     bit(TransferShape::Invalid),
                     AnyValue,
                     Verdict::Refuse,
                     PersistenceRule::Close,
                     "an invalid Transfer-Encoding (empty, not tokens, an unterminated quote, or chunked "
                     "twice or with a "
                     "parameter)" },
        FramingRow { Request,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     TransferPresent,
                     LengthPresent,
                     Verdict::Refuse,
                     PersistenceRule::Close,
                     "a request with both Transfer-Encoding and Content-Length" },
        FramingRow { Request,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     bit(TransferShape::OtherLast),
                     AnyValue,
                     Verdict::Refuse,
                     PersistenceRule::Close,
                     "chunked is not a request's final transfer coding" },
        FramingRow { Request,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     bit(TransferShape::CodedChunked),
                     AnyValue,
                     Verdict::Refuse,
                     PersistenceRule::Close,
                     "a request transfer coding other than chunked, or a parameter (RFC 9112 6.1: 501)",
                     RefusalReason::UnimplementedCoding },
        FramingRow { Request,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     bit(TransferShape::Chunked),
                     AnyValue,
                     Verdict::Chunked,
                     PersistenceRule::FromFields,
                     "a chunked request" },
        FramingRow { Response,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     ChunkedFinal,
                     LengthPresent,
                     Verdict::Chunked,
                     PersistenceRule::Close,
                     "Transfer-Encoding overrides Content-Length; the connection closes" },
        FramingRow { Response,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     ChunkedFinal,
                     AnyValue,
                     Verdict::Chunked,
                     PersistenceRule::FromFields,
                     "a chunked response" },
        FramingRow { Response,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     bit(TransferShape::OtherLast),
                     AnyValue,
                     Verdict::UntilClose,
                     PersistenceRule::Close,
                     "chunked is not the final coding; the body ends at close" },
        FramingRow { AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     bit(LengthShape::Invalid),
                     Verdict::Refuse,
                     PersistenceRule::Close,
                     "an invalid Content-Length" },
        FramingRow { AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     bit(LengthShape::Valid),
                     Verdict::Length,
                     PersistenceRule::FromFields,
                     "a length" },
        FramingRow { Request,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     Verdict::NoBody,
                     PersistenceRule::FromFields,
                     "a request without framing fields has no body" },
        FramingRow { Response,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     AnyValue,
                     Verdict::UntilClose,
                     PersistenceRule::Close,
                     "a response without framing fields ends at close" },
    };

    /// Whether a field's list parsed cleanly.
    enum class ListSyntax : std::uint8_t
    {
        Valid,             ///< Every element ended.
        UnterminatedQuote, ///< A quoted-string ran to the end of a value.
    };

    /// The elements of every value of the field @p name: the comma-separated list of RFC 9110
    /// section 5.6.1, each element without its whitespace, empty elements included. A comma inside a
    /// quoted-string (section 5.6.4, with its backslash escapes) does not split.
    template <typename Visit>
    ListSyntax forEachElement(Fields const& fields, std::string_view name, Visit visit)
    {
        /// Where the split is within a value.
        enum class Quoting : std::uint8_t
        {
            Outside, ///< Not in a quoted-string.
            Inside,  ///< In one.
            Escaped, ///< After a backslash in one.
        };
        auto syntax = ListSyntax::Valid;
        for (auto const value: fields.get(name))
        {
            auto quoting = Quoting::Outside;
            auto start = std::size_t { 0 };
            auto index = std::size_t { 0 };
            for (auto const octet: value)
            {
                if (quoting == Quoting::Escaped)
                    quoting = Quoting::Inside;
                else if (quoting == Quoting::Inside && octet == '\\')
                    quoting = Quoting::Escaped;
                else if (octet == '"')
                    quoting = quoting == Quoting::Inside ? Quoting::Outside : Quoting::Inside;
                else if (quoting == Quoting::Outside && octet == ',')
                {
                    visit(detail::trimWhitespace(value.substr(start, index - start)));
                    start = index + 1;
                }
                ++index;
            }
            visit(detail::trimWhitespace(value.substr(start)));
            if (quoting != Quoting::Outside)
                syntax = ListSyntax::UnterminatedQuote;
        }
        return syntax;
    }

    [[nodiscard]] TransferShape transferShapeOf(Fields const& fields)
    {
        if (fields.get("Transfer-Encoding").empty())
            return TransferShape::Absent;
        auto codings = std::size_t { 0 };
        auto chunkedSeen = false;
        auto lastIsChunked = false;
        auto valid = true;
        auto plain = true;
        auto const syntax = forEachElement(fields, "Transfer-Encoding", [&](std::string_view element) {
            if (element.empty())
                return;
            auto const semicolon = element.find(';');
            auto const coding = detail::trimWhitespace(element.substr(0, semicolon));
            auto const isChunked = detail::equalsIgnoringCase(coding, "chunked");
            auto const hasParameter = semicolon != std::string_view::npos;
            if (!detail::isToken(coding) || (isChunked && (chunkedSeen || hasParameter)))
                valid = false;
            plain = plain && isChunked && !hasParameter;
            ++codings;
            chunkedSeen = chunkedSeen || isChunked;
            lastIsChunked = isChunked;
        });
        if (syntax != ListSyntax::Valid || !valid || codings == 0)
            return TransferShape::Invalid;
        if (!lastIsChunked)
            return TransferShape::OtherLast;
        return plain && codings == 1 ? TransferShape::Chunked : TransferShape::CodedChunked;
    }

    /// The shape of the Content-Length fields and, when valid, their one value.
    struct LengthFacts
    {
        LengthShape shape = LengthShape::Absent;
        std::uint64_t value = 0;
    };

    [[nodiscard]] LengthFacts lengthOf(Fields const& fields)
    {
        if (fields.get("Content-Length").empty())
            return {};
        auto agreed = std::optional<std::uint64_t> {};
        auto valid = true;
        forEachElement(fields, "Content-Length", [&](std::string_view element) {
            auto const value = detail::parseDecimal(element);
            if (!value || (agreed && *agreed != *value))
            {
                valid = false;
                return;
            }
            agreed = value;
        });
        if (!valid || !agreed)
            return LengthFacts { .shape = LengthShape::Invalid, .value = 0 };
        return LengthFacts { .shape = LengthShape::Valid, .value = *agreed };
    }

    /// Whether the Connection field lists @p option.
    [[nodiscard]] bool hasConnectionOption(Fields const& fields, std::string_view option)
    {
        auto found = false;
        forEachElement(fields, "Connection", [&](std::string_view element) {
            found = found || detail::equalsIgnoringCase(element, option);
        });
        return found;
    }

    /// The persistence of RFC 9112 section 9.3: close if asked, else HTTP/1.1 persists and HTTP/1.0
    /// persists only when asked to keep alive.
    [[nodiscard]] Persistence persistenceOf(HttpVersion version, Fields const& fields)
    {
        if (hasConnectionOption(fields, "close"))
            return Persistence::Close;
        if (version == HttpVersion::Http11 || hasConnectionOption(fields, "keep-alive"))
            return Persistence::KeepAlive;
        return Persistence::Close;
    }

    [[nodiscard]] MethodShape methodShapeOf(std::string_view method)
    {
        if (method == "HEAD")
            return MethodShape::Head;
        if (method == "CONNECT")
            return MethodShape::Connect;
        return MethodShape::Other;
    }

    [[nodiscard]] StatusShape statusShapeOf(std::uint16_t status)
    {
        if (status >= 100 && status < 200)
            return StatusShape::Informational;
        if (status == 204 || status == 304)
            return StatusShape::NoContent;
        if (status >= 200 && status < 300)
            return StatusShape::Success;
        return StatusShape::Other;
    }

    [[nodiscard]] HttpResult<Framing> frame(
        MessageRole role, HttpVersion version, MethodShape method, StatusShape status, Fields const& fields)
    {
        auto const transfer = transferShapeOf(fields);
        auto const length = lengthOf(fields);
        auto const matches = [&](FramingRow const& row) {
            return (row.roles & bit(role)) != 0 && (row.versions & bit(version)) != 0
                   && (row.methods & bit(method)) != 0 && (row.statuses & bit(status)) != 0
                   && (row.transfers & bit(transfer)) != 0 && (row.lengths & bit(length.shape)) != 0;
        };
        auto const row = std::ranges::find_if(FramingTable, matches);
        // The last two rows match every request and every response.
        Require(row != FramingTable.end());
        auto const persistence =
            row->persistence == PersistenceRule::Close ? Persistence::Close : persistenceOf(version, fields);
        switch (row->verdict)
        {
            case Verdict::NoBody:
                return Framing { .kind = BodyFraming::None, .length = 0, .persistence = persistence };
            case Verdict::Length:
                return Framing { .kind = BodyFraming::Length,
                                 .length = length.value,
                                 .persistence = persistence };
            case Verdict::Chunked:
                return Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = persistence };
            case Verdict::UntilClose:
                return Framing { .kind = BodyFraming::UntilClose, .length = 0, .persistence = persistence };
            case Verdict::Refuse: break;
        }
        return std::unexpected(detail::refusal(
            HttpErrorCode::ProtocolError, std::string { row->why }, std::nullopt, std::nullopt, row->reason));
    }

    /// The fields the connection writes itself (D7).
    constexpr auto FramingFieldNames = std::array<std::string_view, 6> {
        "Transfer-Encoding", "Content-Length", "Connection", "Keep-Alive", "TE", "Upgrade",
    };

    // ---------------------------------------------------------------------------------------------
    // The chunked decoder's transition table: one row per state, one column per character class.

    /// The class of an octet outside chunk data.
    enum class ChunkClass : std::uint8_t
    {
        Hex,       ///< HEXDIG.
        Semicolon, ///< ';'.
        Space,     ///< SP, HTAB.
        Cr,        ///< CR.
        Lf,        ///< LF.
        Text,      ///< Any other visible octet or obs-text.
        Control,   ///< Any other control octet, NUL included.
    };
    constexpr auto ChunkClassCount = std::size_t { 7 };

    [[nodiscard]] constexpr ChunkClass chunkClassOf(char octet) noexcept
    {
        if (octet == '\r')
            return ChunkClass::Cr;
        if (octet == '\n')
            return ChunkClass::Lf;
        if (octet == ';')
            return ChunkClass::Semicolon;
        if (detail::isIn(octet, detail::CharClass::Hex))
            return ChunkClass::Hex;
        if (detail::isIn(octet, detail::CharClass::Whitespace))
            return ChunkClass::Space;
        if (detail::isIn(octet, detail::CharClass::FieldValue))
            return ChunkClass::Text;
        return ChunkClass::Control;
    }

    /// What a transition does besides moving to its state.
    enum class ChunkAction : std::uint8_t
    {
        Refuse,      ///< The octet is not allowed here.
        Line,        ///< Counts a chunk-line octet.
        Digit,       ///< Counts a chunk-line octet and adds a hex digit to the size.
        EndSize,     ///< Counts a chunk-line octet; the size has ended and needs a digit.
        EndLine,     ///< The chunk line has ended: data follows, or the trailers if the size is 0.
        Expect,      ///< The octet was the one expected; nothing else.
        TrailerByte, ///< Adds an octet to the trailer line.
        EndTrailer,  ///< The trailer line has ended: a field, or the end of the body if empty.
    };

    using State = ChunkedDecoder::State;

    struct ChunkTransition
    {
        State next = State::Failed;
        ChunkAction action = ChunkAction::Refuse;
        std::string_view why;
    };

    using ChunkRow = std::array<ChunkTransition, ChunkClassCount>;

    /// A row in which every class is refused for @p why but those @p allowed name.
    constexpr ChunkRow refuseAllBut(std::string_view why,
                                    std::initializer_list<std::pair<ChunkClass, ChunkTransition>> allowed)
    {
        auto row = ChunkRow {};
        for (auto& transition: row)
            transition = ChunkTransition { .next = State::Failed, .action = ChunkAction::Refuse, .why = why };
        for (auto const& [chunkClass, transition]: allowed)
            row.at(static_cast<std::size_t>(chunkClass)) = transition;
        return row;
    }

    constexpr auto StateCount = static_cast<std::size_t>(State::Failed) + 1;

    constexpr auto ChunkTable = [] {
        auto table = std::array<ChunkRow, StateCount> {};
        for (auto& row: table)
            row = refuseAllBut("no octet is read in this state", {});
        auto const at = [&table](State state) -> ChunkRow& {
            return table.at(static_cast<std::size_t>(state));
        };
        at(State::Size) = refuseAllBut(
            "a chunk size is hex digits, then a ';', whitespace or CRLF",
            { { ChunkClass::Hex, { .next = State::Size, .action = ChunkAction::Digit, .why = {} } },
              { ChunkClass::Semicolon,
                { .next = State::Extension, .action = ChunkAction::EndSize, .why = {} } },
              { ChunkClass::Space, { .next = State::SizeSpace, .action = ChunkAction::EndSize, .why = {} } },
              { ChunkClass::Cr, { .next = State::SizeLf, .action = ChunkAction::EndSize, .why = {} } } });
        at(State::SizeSpace) = refuseAllBut(
            "whitespace after a chunk size is followed by a ';' only",
            { { ChunkClass::Space, { .next = State::SizeSpace, .action = ChunkAction::Line, .why = {} } },
              { ChunkClass::Semicolon,
                { .next = State::Extension, .action = ChunkAction::Line, .why = {} } } });
        at(State::Extension) = refuseAllBut(
            "a chunk extension holds a control octet or a bare LF",
            { { ChunkClass::Hex, { .next = State::Extension, .action = ChunkAction::Line, .why = {} } },
              { ChunkClass::Semicolon, { .next = State::Extension, .action = ChunkAction::Line, .why = {} } },
              { ChunkClass::Space, { .next = State::Extension, .action = ChunkAction::Line, .why = {} } },
              { ChunkClass::Text, { .next = State::Extension, .action = ChunkAction::Line, .why = {} } },
              { ChunkClass::Cr, { .next = State::SizeLf, .action = ChunkAction::Line, .why = {} } } });
        at(State::SizeLf) = refuseAllBut(
            "a chunk line's CR is not followed by LF",
            { { ChunkClass::Lf, { .next = State::Data, .action = ChunkAction::EndLine, .why = {} } } });
        at(State::DataCr) = refuseAllBut(
            "chunk data is not followed by CRLF",
            { { ChunkClass::Cr, { .next = State::DataLf, .action = ChunkAction::Expect, .why = {} } } });
        at(State::DataLf) = refuseAllBut(
            "chunk data's CR is not followed by LF",
            { { ChunkClass::Lf, { .next = State::Size, .action = ChunkAction::Expect, .why = {} } } });
        at(State::TrailerLine) = refuseAllBut(
            "a trailer line holds a control octet or a bare LF",
            { { ChunkClass::Hex,
                { .next = State::TrailerLine, .action = ChunkAction::TrailerByte, .why = {} } },
              { ChunkClass::Semicolon,
                { .next = State::TrailerLine, .action = ChunkAction::TrailerByte, .why = {} } },
              { ChunkClass::Space,
                { .next = State::TrailerLine, .action = ChunkAction::TrailerByte, .why = {} } },
              { ChunkClass::Text,
                { .next = State::TrailerLine, .action = ChunkAction::TrailerByte, .why = {} } },
              { ChunkClass::Cr, { .next = State::TrailerLf, .action = ChunkAction::Expect, .why = {} } } });
        at(State::TrailerLf) = refuseAllBut(
            "a trailer line's CR is not followed by LF",
            { { ChunkClass::Lf,
                { .next = State::TrailerLine, .action = ChunkAction::EndTrailer, .why = {} } } });
        return table;
    }();

    /// The most hex digits a chunk size may have: 16 fill 64 bits, and a 17th could overflow them.
    constexpr auto MaxChunkSizeDigits = std::size_t { 16 };

    /// The value of the hex digit @p octet.
    [[nodiscard]] constexpr std::uint64_t hexValue(char octet) noexcept
    {
        if (octet >= '0' && octet <= '9')
            return static_cast<std::uint64_t>(octet - '0');
        return static_cast<std::uint64_t>(detail::toLowerAscii(octet) - 'a') + 10;
    }

} // namespace

// -------------------------------------------------------------------------------------------------
// Framing

HttpResult<Framing> requestFraming(HttpVersion version, Fields const& fields)
{
    return frame(MessageRole::Request, version, MethodShape::Other, StatusShape::Other, fields);
}

HttpResult<Framing> responseFraming(std::string_view requestMethod,
                                    HttpVersion version,
                                    std::uint16_t status,
                                    Fields const& fields)
{
    return frame(MessageRole::Response, version, methodShapeOf(requestMethod), statusShapeOf(status), fields);
}

bool isFramingField(std::string_view name) noexcept
{
    return std::ranges::any_of(FramingFieldNames, [name](std::string_view framing) {
        return detail::equalsIgnoringCase(framing, name);
    });
}

// -------------------------------------------------------------------------------------------------
// HeadParser

HeadParser::HeadParser(MessageRole role, HttpLimits limits): _role { role }, _limits { limits }
{
}

HttpResult<FeedResult> HeadParser::feed(std::string_view bytes)
{
    if (_state == State::Failed)
        return std::unexpected(*_error);
    if (_state == State::Complete)
        return FeedResult { .progress = Progress::Complete, .consumed = 0 };
    auto const& rules = rulesFor(_role);
    auto consumed = std::size_t { 0 };
    for (auto const octet: bytes)
    {
        ++consumed;
        if (++_headBytes > _limits.maxHeadBytes)
            return std::unexpected(fail(detail::refusal(rules.headSection,
                                                        "the head is past maxHeadBytes",
                                                        std::nullopt,
                                                        std::uint64_t { _headBytes })));
        switch (_lineState)
        {
            case LineState::Text:
                if (octet == '\r')
                {
                    _lineState = LineState::Cr;
                    break;
                }
                if (octet == '\n')
                    return std::unexpected(
                        fail(detail::refusal(HttpErrorCode::ProtocolError, "a bare LF ends a line")));
                if (_line.size() >= _limits.maxFieldBytes)
                {
                    auto const isStartLine = _state == State::StartLine;
                    return std::unexpected(
                        fail(detail::refusal(isStartLine ? rules.startLineTooLong : rules.fieldTooLong,
                                             "a line is past maxFieldBytes",
                                             isStartLine ? std::nullopt : nameOf(_line),
                                             std::uint64_t { _line.size() + 1 })));
                }
                _line += octet;
                break;
            case LineState::Cr:
                if (octet != '\n')
                    return std::unexpected(
                        fail(detail::refusal(HttpErrorCode::ProtocolError, "a bare CR in a line")));
                _lineState = LineState::Text;
                if (auto ended = endLine(); !ended)
                    return std::unexpected(fail(std::move(ended.error())));
                if (_state == State::Complete)
                    return FeedResult { .progress = Progress::Complete, .consumed = consumed };
                break;
        }
    }
    return FeedResult { .progress = Progress::NeedMore, .consumed = consumed };
}

HttpResult<void> HeadParser::endLine()
{
    auto const line = std::move(_line);
    _line.clear();
    if (_state == State::StartLine)
    {
        if (line.empty())
        {
            if (rulesFor(_role).leadingEmptyLines == LeadingEmptyLines::Skip)
                return {};
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError, "an empty line before the start line"));
        }
        _state = State::Fields;
        return startLine(line);
    }
    if (line.empty())
        return finish();
    return fieldLine(line);
}

HttpResult<void> HeadParser::startLine(std::string_view line)
{
    if (_role == MessageRole::Request)
    {
        // request-line = method SP request-target SP HTTP-version, one SP each.
        auto const first = line.find(' ');
        auto const second = first == std::string_view::npos ? first : line.find(' ', first + 1);
        if (second == std::string_view::npos || first == 0 || second == first + 1
            || second + 1 == line.size())
            return std::unexpected(detail::refusal(HttpErrorCode::ProtocolError, "a malformed request line"));
        auto method = Method::parse(line.substr(0, first));
        if (!method)
            return std::unexpected(std::move(method.error()));
        auto target = RequestTarget::parse(line.substr(first + 1, second - first - 1));
        if (!target)
            return std::unexpected(std::move(target.error()));
        auto const version = parseVersion(line.substr(second + 1));
        if (!version)
            return std::unexpected(unsupportedVersion());
        if (auto formed = checkTargetForm(method->text(), target->form()); !formed)
            return formed;
        _request.method = std::move(*method);
        _request.target = std::move(*target);
        _request.version = *version;
        return {};
    }

    // status-line = HTTP-version SP 3DIGIT SP [ reason-phrase ]; the SP after the code may be absent.
    auto const space = line.find(' ');
    auto const version = parseVersion(line.substr(0, space));
    if (!version)
        return std::unexpected(unsupportedVersion());
    auto const rest = space == std::string_view::npos ? std::string_view {} : line.substr(space + 1);
    auto const digits = rest.substr(0, 3);
    if (digits.size() != 3 || !detail::allIn(digits, detail::CharClass::Digit) || digits.front() < '1'
        || digits.front() > '5' || (rest.size() > 3 && rest[3] != ' '))
        return std::unexpected(detail::refusal(HttpErrorCode::ProtocolError, "a malformed status line"));
    auto reason = ReasonPhrase::parse(rest.size() > 3 ? rest.substr(4) : std::string_view {});
    if (!reason)
        return std::unexpected(std::move(reason.error()));
    _response.version = *version;
    _response.status = static_cast<std::uint16_t>(detail::parseDecimal(digits).value_or(0));
    _response.reason = std::move(*reason);
    return {};
}

HttpResult<void> HeadParser::fieldLine(std::string_view line)
{
    auto& fields = _role == MessageRole::Request ? _request.fields : _response.fields;
    auto const& rules = rulesFor(_role);
    return takeFieldLine(line,
                         rules,
                         FieldSection { .maxFields = _limits.maxFields, .tooMany = rules.headSection },
                         _pending,
                         fields);
}

HttpResult<void> HeadParser::finish()
{
    auto& fields = _role == MessageRole::Request ? _request.fields : _response.fields;
    if (auto flushed = flushField(
            _pending,
            fields,
            FieldSection { .maxFields = _limits.maxFields, .tooMany = rulesFor(_role).headSection });
        !flushed)
        return flushed;
    if (_role == MessageRole::Request)
    {
        auto const hosts = fields.get("Host");
        if (hosts.size() > 1)
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError, "more than one Host field", "Host"));
        if (hosts.empty() && _request.version == HttpVersion::Http11)
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError, "an HTTP/1.1 request without Host"));
        if (!hosts.empty() && !isHostValue(hosts.front()))
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError, "an invalid Host field", "Host"));
    }
    _state = State::Complete;
    return {};
}

HttpError HeadParser::fail(HttpError error)
{
    _state = State::Failed;
    _error = error;
    return error;
}

RequestHead HeadParser::takeRequest()
{
    Require(_role == MessageRole::Request && _state == State::Complete);
    return std::move(_request);
}

ResponseHead HeadParser::takeResponse()
{
    Require(_role == MessageRole::Response && _state == State::Complete);
    return std::move(_response);
}

// -------------------------------------------------------------------------------------------------
// ChunkedDecoder

ChunkedDecoder::ChunkedDecoder(MessageRole role, HttpLimits limits): _role { role }, _limits { limits }
{
    Require(_limits.maxReadChunk > 0);
}

HttpResult<ChunkStep> ChunkedDecoder::feed(std::string_view bytes)
{
    if (_state == State::Failed)
        return std::unexpected(*_error);
    auto consumed = std::size_t { 0 };
    while (consumed < bytes.size() && _state != State::Done)
    {
        if (_state == State::Data)
        {
            auto const take = static_cast<std::size_t>(
                std::min<std::uint64_t>({ _remaining, bytes.size() - consumed, _limits.maxReadChunk }));
            auto const data = bytes.substr(consumed, take);
            consumed += take;
            _remaining -= take;
            if (_remaining == 0)
                _state = State::DataCr;
            return ChunkStep { .consumed = consumed, .data = data, .progress = Progress::NeedMore };
        }
        if (auto stepped = step(bytes[consumed]); !stepped)
            return std::unexpected(fail(std::move(stepped.error())));
        ++consumed;
    }
    return ChunkStep { .consumed = consumed,
                       .data = {},
                       .progress = _state == State::Done ? Progress::Complete : Progress::NeedMore };
}

HttpResult<void> ChunkedDecoder::step(char octet)
{
    auto const& rules = rulesFor(_role);
    auto const& transition =
        ChunkTable.at(static_cast<std::size_t>(_state)).at(static_cast<std::size_t>(chunkClassOf(octet)));
    auto const inTrailers = _state == State::TrailerLine || _state == State::TrailerLf;
    if (inTrailers && ++_trailerBytes > _limits.maxTrailerBytes)
        return std::unexpected(detail::refusal(rules.trailerSection,
                                               "the trailers are past maxTrailerBytes",
                                               std::nullopt,
                                               std::uint64_t { _trailerBytes }));
    auto const inChunkLine = _state == State::Size || _state == State::SizeSpace || _state == State::Extension
                             || _state == State::SizeLf;
    if (inChunkLine && ++_lineBytes > _limits.maxChunkLineBytes)
        return std::unexpected(
            detail::refusal(HttpErrorCode::ProtocolError, "a chunk line is past maxChunkLineBytes"));

    switch (transition.action)
    {
        case ChunkAction::Refuse:
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError, std::string { transition.why }));
        case ChunkAction::Line:
        case ChunkAction::Expect: break;
        case ChunkAction::Digit:
            if (++_sizeDigits > MaxChunkSizeDigits)
                return std::unexpected(detail::refusal(HttpErrorCode::ProtocolError,
                                                       "a chunk size has more than 16 hex digits"));
            _size = (_size << 4U) | hexValue(octet);
            break;
        case ChunkAction::EndSize:
            if (_sizeDigits == 0)
                return std::unexpected(
                    detail::refusal(HttpErrorCode::ProtocolError, "a chunk size has no digits"));
            break;
        case ChunkAction::EndLine: {
            auto const limit = _limits.maxBodyBytes.value_or(std::numeric_limits<std::uint64_t>::max());
            if (_size > limit - std::min(limit, _bodyBytes))
                return std::unexpected(
                    detail::refusal(rules.bodySize,
                                    "the body is past maxBodyBytes",
                                    std::nullopt,
                                    _size > std::numeric_limits<std::uint64_t>::max() - _bodyBytes
                                        ? std::numeric_limits<std::uint64_t>::max()
                                        : _bodyBytes + _size));
            _bodyBytes += _size;
            _remaining = _size;
            _state = _size == 0 ? State::TrailerLine : State::Data;
            _size = 0;
            _sizeDigits = 0;
            _lineBytes = 0;
            return {};
        }
        case ChunkAction::TrailerByte:
            if (_line.size() >= _limits.maxFieldBytes)
                return std::unexpected(detail::refusal(rules.trailerTooLong,
                                                       "a trailer line is past maxFieldBytes",
                                                       nameOf(_line),
                                                       std::uint64_t { _line.size() + 1 }));
            _line += octet;
            break;
        case ChunkAction::EndTrailer: _state = transition.next; return endTrailerLine();
    }
    _state = transition.next;
    return {};
}

HttpResult<void> ChunkedDecoder::endTrailerLine()
{
    auto const& rules = rulesFor(_role);
    auto const section = FieldSection { .maxFields = _limits.maxFields, .tooMany = rules.trailerSection };
    auto const line = std::move(_line);
    _line.clear();
    if (line.empty())
    {
        if (auto flushed = flushField(_pending, _trailers, section); !flushed)
            return flushed;
        _state = State::Done;
        return {};
    }
    return takeFieldLine(line, rules, section, _pending, _trailers);
}

HttpError ChunkedDecoder::fail(HttpError error)
{
    _state = State::Failed;
    _error = error;
    return error;
}

Fields ChunkedDecoder::takeTrailers()
{
    return std::move(_trailers);
}

std::string chunkHeader(std::size_t size)
{
    Require(size > 0);
    return std::format("{:x}\r\n", size);
}

std::string lastChunk(Fields const& trailers)
{
    auto result = std::string { "0\r\n" };
    for (auto const& field: trailers.entries())
        result += field.name + ": " + field.value + "\r\n";
    result += "\r\n";
    return result;
}

} // namespace core::http
