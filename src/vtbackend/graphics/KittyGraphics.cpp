// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyGraphics.hpp>

#include <array>
#include <charconv>
#include <cstdlib>
#include <format>
#include <system_error>
#include <utility>

namespace vtbackend::kitty_graphics
{

namespace
{
    /// Parses an integer key's value, leaving @p target untouched when the value is not a number.
    template <typename T>
    [[nodiscard]] bool assignNumber(std::string_view value, T& target) noexcept
    {
        if (value.empty())
            return false;
        auto parsed = T {};
        // from_chars takes a [first, last) range, not a NUL-terminated string.
        auto const* const first = value.data();
        auto const* const last = first + value.size();
        auto const [ptr, ec] = std::from_chars(first, last, parsed);
        if (ec != std::errc {} || ptr != last)
            return false;
        target = parsed;
        return true;
    }

    /// Applies one `key=value` pair to @p command.
    ///
    /// Unknown keys are ignored rather than rejected, deliberately: the protocol grows, and an older
    /// terminal that refuses a command because it did not recognise one key is less useful than one
    /// that honours the parts it does understand.
    [[nodiscard]] std::expected<void, Error> applyKey(Command& command, char key, std::string_view value)
    {
        switch (key)
        {
            case 'a': {
                if (value.size() != 1)
                    return std::unexpected(Error::InvalidAction);
                switch (value[0])
                {
                    case 'q':
                    case 't':
                    case 'T':
                    case 'p':
                    case 'd':
                    case 'f':
                    case 'a':
                    case 'c': command.action = static_cast<Action>(value[0]); break;
                    default: return std::unexpected(Error::InvalidAction);
                }
                break;
            }
            case 'f': {
                auto number = 0u;
                if (!assignNumber(value, number))
                    return std::unexpected(Error::InvalidFormat);
                switch (number)
                {
                    case 24: command.format = Format::Rgb; break;
                    case 32: command.format = Format::Rgba; break;
                    case 100: command.format = Format::Png; break;
                    default: return std::unexpected(Error::InvalidFormat);
                }
                break;
            }
            case 't': {
                if (value.size() != 1)
                    return std::unexpected(Error::InvalidMedium);
                switch (value[0])
                {
                    case 'd':
                    case 'f':
                    case 't':
                    case 's': command.medium = static_cast<Medium>(value[0]); break;
                    default: return std::unexpected(Error::InvalidMedium);
                }
                break;
            }
            case 'o': {
                if (value == "z")
                    command.compression = Compression::ZlibDeflate;
                else
                    return std::unexpected(Error::InvalidControlData);
                break;
            }
            case 'i': (void) assignNumber(value, command.imageId); break;
            case 'I': (void) assignNumber(value, command.imageNumber); break;
            case 'p': (void) assignNumber(value, command.placementId); break;
            case 'P': (void) assignNumber(value, command.parentImageId); break;
            case 's': (void) assignNumber(value, command.pixelWidth); break;
            case 'v': (void) assignNumber(value, command.pixelHeight); break;
            case 'x': (void) assignNumber(value, command.sourceX); break;
            case 'y': (void) assignNumber(value, command.sourceY); break;
            case 'w': (void) assignNumber(value, command.sourceWidth); break;
            case 'h': (void) assignNumber(value, command.sourceHeight); break;
            case 'c': (void) assignNumber(value, command.columns); break;
            case 'r': (void) assignNumber(value, command.rows); break;
            case 'z': (void) assignNumber(value, command.zIndex); break;
            case 'm': {
                auto number = 0u;
                (void) assignNumber(value, number);
                command.moreChunksFollow = number != 0;
                break;
            }
            case 'C': {
                auto number = 0u;
                (void) assignNumber(value, number);
                command.doNotMoveCursor = number != 0;
                break;
            }
            case 'q': {
                auto number = 0u;
                (void) assignNumber(value, number);
                command.quietOnSuccess = number >= 1;
                command.quietAlways = number >= 2;
                break;
            }
            case 'd': {
                if (value.size() != 1)
                    return std::unexpected(Error::InvalidControlData);
                command.deleteTarget = value[0];
                break;
            }
            case 'U': {
                auto number = 0u;
                (void) assignNumber(value, number);
                command.placementKind = number == 1 ? PlacementKind::Virtual : PlacementKind::Cursor;
                break;
            }
            case 'e': {
                if (value.size() != 1)
                    return std::unexpected(Error::InvalidControlData);
                command.queryFeature = value[0];
                break;
            }
            case 'X': (void) assignNumber(value, command.rawX); break;
            case 'Y': (void) assignNumber(value, command.frameBackground); break;
            default:
                // Unknown key: ignore, see above.
                break;
        }
        return {};
    }

    /// How a feature query's reply spells a key's value.
    enum class EchoForm : uint8_t
    {
        Number = 0, ///< In decimal.
        Letter = 1, ///< As the letter it is.
    };

    /// One key a feature query may ask about.
    struct FeatureKey
    {
        char key;                                  ///< The wire letter.
        std::string_view actions;                  ///< The actions (wire letters) Contour supports it for.
        EchoForm form;                             ///< How its value is spelled.
        int64_t (*value)(Command const&) noexcept; ///< Its value in the query; 0 when default.
    };

    /// The actions a feature query may ask about: all of them, as kitty's.
    constexpr auto FeatureActions = std::string_view { "tTqpdfac" };

    /// The delete targets deleteKittyGraphics() implements, both cases. `a` is the protocol's default
    /// and therefore never echoed.
    constexpr auto SupportedDeleteTargets = std::string_view { "AiIfF" };

    /// Every key a feature query is answered for, in kitty's reply order. Animation keys are left out on
    /// purpose: under `a=q` the parser keeps `c`, `r` and `z` as geometry, so their animation reading is
    /// not available here, and an omitted key only under-claims.
    // clang-format off
    constexpr auto FeatureKeys = std::array {
        FeatureKey { 'o', "tTq", EchoForm::Letter, [](Command const& c) noexcept -> int64_t { return c.compression == Compression::ZlibDeflate ? 'z' : 0; } },
        FeatureKey { 'f', "tTq", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.format ? static_cast<int64_t>(*c.format) : 0; } },
        FeatureKey { 's', "tTq", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.pixelWidth; } },
        FeatureKey { 'v', "tTq", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.pixelHeight; } },
        FeatureKey { 'p', "Tp", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.placementId; } },
        FeatureKey { 'c', "Tp", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.columns; } },
        FeatureKey { 'r', "Tp", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.rows; } },
        FeatureKey { 'C', "Tp", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.doNotMoveCursor ? 1 : 0; } },
        FeatureKey { 'U', "Tp", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.placementKind == PlacementKind::Virtual ? 1 : 0; } },
        FeatureKey { 'z', "Tp", EchoForm::Number, [](Command const& c) noexcept -> int64_t { return c.zIndex; } },
        FeatureKey { 'd', "d",   EchoForm::Letter, [](Command const& c) noexcept -> int64_t { return SupportedDeleteTargets.contains(c.deleteTarget) ? c.deleteTarget : 0; } },
    };
    // clang-format on
} // namespace

std::string supportedFeatureKeys(Command const& command)
{
    auto const action = command.queryFeature;
    if (action == 0 || !FeatureActions.contains(action))
        return {};

    auto keys = std::format(",e={}", action);
    for (auto const& feature: FeatureKeys)
    {
        if (!feature.actions.contains(action))
            continue;
        auto const value = feature.value(command);
        if (value == 0)
            continue;
        if (feature.form == EchoForm::Letter)
            keys += std::format(",{}={}", feature.key, static_cast<char>(value));
        else
            keys += std::format(",{}={}", feature.key, value);
    }
    return keys;
}

std::string_view errorCode(Error error) noexcept
{
    switch (error)
    {
        case Error::InvalidControlData: return "EINVAL";
        case Error::InvalidFormat: return "EINVAL";
        case Error::InvalidMedium: return "EINVAL";
        case Error::InvalidAction: return "EINVAL";
        case Error::MissingDimensions: return "EINVAL";
    }
    return "EINVAL";
}

std::expected<Command, Error> parseCommand(std::string_view apcPayload)
{
    auto command = Command {};

    // The payload is separated from the control data by the FIRST semicolon; semicolons inside the
    // payload are not separators, which is why this is a find rather than a split.
    auto const separator = apcPayload.find(';');
    auto const controlData = apcPayload.substr(0, separator);
    if (separator != std::string_view::npos)
        command.payload = apcPayload.substr(separator + 1);

    for (size_t offset = 0; offset < controlData.size();)
    {
        auto end = controlData.find(',', offset);
        if (end == std::string_view::npos)
            end = controlData.size();

        auto const pair = controlData.substr(offset, end - offset);
        offset = end + 1;

        if (pair.empty())
            continue;
        if (pair.size() < 3 || pair[1] != '=')
            return std::unexpected(Error::InvalidControlData);

        if (auto const result = applyKey(command, pair[0], pair.substr(2)); !result)
            return std::unexpected(result.error());
    }

    // `c`, `r` and `z` are cell geometry for every command except the animation ones, where they are
    // frame numbers and a delay. The action can appear anywhere in the control data -- notcurses puts
    // it first, others put it last -- so the reinterpretation has to happen once the whole block is
    // decoded, not while walking it. Moving rather than copying leaves no second reading behind for a
    // later change to pick up by accident.
    switch (command.action)
    {
        case Action::Frame:
            command.baseFrame = std::exchange(command.columns, 0);
            command.targetFrame = std::exchange(command.rows, 0);
            command.frameGapMilliseconds = std::exchange(command.zIndex, 0);
            command.compositionMode =
                std::exchange(command.rawX, 0) != 0 ? CompositionMode::Replace : CompositionMode::AlphaBlend;
            break;
        case Action::Animate:
            // Animation control overloads two keys more than frame transmission does: `s` is the play
            // state and `v` the loop count, not the pixel dimensions.
            command.currentFrame = std::exchange(command.columns, 0);
            command.targetFrame = std::exchange(command.rows, 0);
            command.frameGapMilliseconds = std::exchange(command.zIndex, 0);
            command.loopCount = std::exchange(command.pixelHeight, 0);
            switch (std::exchange(command.pixelWidth, 0))
            {
                case 1: command.animationState = AnimationState::Stop; break;
                case 2: command.animationState = AnimationState::RunAwaitingFrames; break;
                case 3: command.animationState = AnimationState::Loop; break;
                default: command.animationState = AnimationState::Unset; break;
            }
            break;
        case Action::Compose:
            // `C=` is the cursor policy elsewhere; here it picks overwrite over alpha blending.
            command.composeSourceFrame = std::exchange(command.rows, 0);
            command.composeDestinationFrame = std::exchange(command.columns, 0);
            command.composeSourceX = std::exchange(command.rawX, 0);
            command.composeSourceY = std::exchange(command.frameBackground, 0);
            command.composeDestinationX = std::exchange(command.sourceX, 0);
            command.composeDestinationY = std::exchange(command.sourceY, 0);
            command.compositionMode = std::exchange(command.doNotMoveCursor, false)
                                          ? CompositionMode::Replace
                                          : CompositionMode::AlphaBlend;
            break;
        case Action::Query:
        case Action::Transmit:
        case Action::TransmitAndDisplay:
        case Action::Put:
        case Action::Delete: break;
    }

    // NOTE: whether the dimensions a raw pixel transmission needs are actually present is checked by
    // the CALLER, not here: a continuation chunk legitimately carries no control data of its own, and
    // only the caller knows whether one is in flight.
    return command;
}

} // namespace vtbackend::kitty_graphics
