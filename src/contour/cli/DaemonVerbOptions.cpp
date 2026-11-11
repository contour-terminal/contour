// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/DaemonVerbOptions.hpp>
#include <contour/cli/Hosting.hpp>

#include <core/Utf8Path.hpp>
#include <core/codec/Stream.hpp>

#include <new>
#include <utility>

#include <vthost/SocketPath.hpp>

namespace contour::cli
{

namespace
{
    constexpr std::string_view Prefix = "contour.daemon.";

    [[nodiscard]] std::string textOf(core::cli::FlagStore const& parameters, std::string_view name)
    {
        return parameters.str(std::string { Prefix } + std::string { name });
    }

    /// The CLI parser delivers a switch as a `bool`; it becomes an OptionFlag at this boundary.
    [[nodiscard]] OptionFlag flagOf(core::cli::FlagStore const& parameters, std::string_view name)
    {
        return parameters.boolean(std::string { Prefix } + std::string { name }) ? OptionFlag::Given
                                                                                 : OptionFlag::Absent;
    }

    [[nodiscard]] DaemonOptionsError errorOf(core::codec::DecodeError error) noexcept
    {
        switch (error)
        {
            case core::codec::DecodeError::Truncated: return DaemonOptionsError::Truncated;
            case core::codec::DecodeError::UnknownFormatVersion: return DaemonOptionsError::NewerVersion;
            case core::codec::DecodeError::MalformedRecord:
            case core::codec::DecodeError::InvalidValue:
            case core::codec::DecodeError::TooLarge: break;
        }
        return DaemonOptionsError::Malformed;
    }

    /// The options record's fields: the options as given, then the socket they resolved to.
    struct LaunchRecord
    {
        DaemonVerbOptions given;
        std::string socketPath; ///< UTF-8.

        void visitFields(this auto& self, auto& visitor)
        {
            self.given.visitFields(visitor);
            visitor(DaemonLaunchSocketPathField, self.socketPath);
        }
    };
} // namespace

DaemonVerbOptions readDaemonVerbOptions(core::cli::FlagStore const& parameters)
{
    return DaemonVerbOptions {
        .socket = textOf(parameters, "socket"),
        .label = textOf(parameters, "label"),
        .config = textOf(parameters, "config"),
        .profile = textOf(parameters, "profile"),
        .background = flagOf(parameters, "background"),
        .stdio = flagOf(parameters, "stdio"),
        .inPlace = flagOf(parameters, "in-place"),
        .handover = textOf(parameters, "handover"),
        .lifecycle = textOf(parameters, "lifecycle"),
        .exitWithLastSession = flagOf(parameters, "exit-with-last-session"),
        .sizePolicy = textOf(parameters, "size-policy"),
        .tmuxCompatSocket = textOf(parameters, "tmux-compat-socket"),
        .listenTcp = textOf(parameters, "listen-tcp"),
        .token = textOf(parameters, "token"),
        .tokenFile = textOf(parameters, "token-file"),
        .accessFile = textOf(parameters, "access-file"),
        .tlsCert = textOf(parameters, "tls-cert"),
        .tlsKey = textOf(parameters, "tls-key"),
        .log = textOf(parameters, "log"),
        .logFile = textOf(parameters, "log-file"),
        .authorizedKeys = textOf(parameters, "authorized-keys"),
        .auth = textOf(parameters, "auth"),
    };
}

DaemonLaunch DaemonLaunch::resolve(DaemonVerbOptions given, std::string const& configuredSocket)
{
    auto socketPath = vthost::muxSocketPath(vthost::socketLabelOf(given.label),
                                            resolveSocketOption(given.socket, configuredSocket));
    return DaemonLaunch { std::move(given), std::move(socketPath) };
}

DaemonLaunch DaemonLaunch::restore(DaemonVerbOptions given, std::filesystem::path socketPath)
{
    return DaemonLaunch { std::move(given), std::move(socketPath) };
}

std::vector<uint8_t> encodeDaemonLaunch(DaemonLaunch const& launch)
{
    // The fields in one frame after the version: a record cut short at a field boundary is refused as
    // truncated, where its fields alone would decode, the missing ones at their defaults.
    auto const record =
        LaunchRecord { .given = launch.given(), .socketPath = core::pathToUtf8(launch.socketPath()) };
    auto fields = core::codec::Writer {};
    core::codec::encodeFields(fields, record);
    auto const payload = std::move(fields).take();

    auto writer = core::codec::Writer {};
    writer.varint(DaemonOptionsVersion);
    writer.bytes(payload);
    return std::move(writer).take();
}

std::expected<DaemonLaunch, DaemonOptionsError> decodeDaemonLaunch(std::span<uint8_t const> record)
{
    auto reader = core::codec::Reader { record };
    auto const version = reader.varint();
    if (!version)
        return std::unexpected { errorOf(version.error()) };
    if (*version > DaemonOptionsVersion)
        return std::unexpected { DaemonOptionsError::NewerVersion };
    auto const payload = reader.bytes();
    if (!payload)
        return std::unexpected { errorOf(payload.error()) };
    if (!reader.atEnd())
        return std::unexpected { DaemonOptionsError::Malformed };
    try
    {
        auto fields = LaunchRecord {};
        if (auto const decoded = core::codec::decodeFields(*payload, fields); !decoded)
            return std::unexpected { errorOf(decoded.error()) };
        auto socketPath = core::pathFromUtf8(fields.socketPath);
        if (!socketPath)
            return std::unexpected { DaemonOptionsError::Malformed };
        return DaemonLaunch::restore(std::move(fields.given), *std::move(socketPath));
    }
    catch (std::bad_alloc const&)
    {
        // What the decode budget does not bound -- a single string as long as the record -- may not fit.
        return std::unexpected { DaemonOptionsError::Malformed };
    }
}

} // namespace contour::cli
