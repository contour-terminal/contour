// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/KittyGraphics.h>

#include <charconv>
#include <cstdlib>

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
        auto const* const last = value.data() + value.size();
        auto const [ptr, ec] = std::from_chars(value.data(), last, parsed);
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
            default:
                // Unknown key: ignore, see above.
                break;
        }
        return {};
    }
} // namespace

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

    // NOTE: whether the dimensions a raw pixel transmission needs are actually present is checked by
    // the CALLER, not here: a continuation chunk legitimately carries no control data of its own, and
    // only the caller knows whether one is in flight.
    return command;
}

} // namespace vtbackend::kitty_graphics
