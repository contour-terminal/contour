// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One example of every catalog PDU, so a test can sweep the whole DecodedPdu variant instead of naming
/// alternatives one at a time (and silently missing the next one added). The examples carry content a trace
/// must not leak, which is what PduTrace_test sweeps them for.

#include <cstddef>
#include <utility>
#include <vector>

#include <vthost/proto/Pdu.hpp>

namespace vthost::testing
{

/// A SessionState carrying content the trace must not leak. Built field-by-field rather than
/// with designated initializers: the interesting fields straddle several the test does not care
/// about, and the pedantic build requires designators to be contiguous.
[[nodiscard]] inline proto::SessionState sampleSessionState()
{
    auto value = proto::SessionState {};
    value.session = 4;
    value.columns = 80;
    value.lines = 24;
    value.title = "secret-project";
    value.cwd = "file:///home/user/secret";
    auto block = proto::WireCommandBlock {};
    block.commandLine = "secret-command";
    block.workingDirectory = "/home/user/secret-dir";
    value.commandBlocks = { block };
    return value;
}

/// A one-line Delta with a distinctive cursor, built for the same reason as above.
[[nodiscard]] inline proto::Delta sampleDelta()
{
    auto value = proto::Delta {};
    value.session = 7;
    value.generation = 3;
    value.seqno = 98;
    value.lines = std::vector<proto::WireLine>(1);
    auto block = proto::WireCommandBlock {};
    block.commandLine = "secret-command";
    value.commandBlocks = { block };
    value.retiredCommandBlocks = { 3 };
    return value;
}

/// One example of every catalog alternative, so the tests below can sweep the whole variant
/// instead of naming alternatives one at a time (and silently missing the 18th).
[[nodiscard]] inline std::vector<std::pair<proto::PduType, proto::DecodedPdu>> everyAlternative()
{
    return {
        { proto::PduType::Invalid, proto::DecodedPdu { proto::Invalid { .ident = 250 } } },
        { proto::PduType::ClientHello,
          proto::DecodedPdu { proto::ClientHello {
              .versions = { 4, 12 },
              .capabilities = { .idents = { 1, 4711 } },
              .token = "s3cr3t",
              // The settings block is the user's configuration; PduTrace_test proves
              // the trace summarizes rather than transcribes it.
              .sessionSettings = proto::WireSessionSettings { .historyLineCount = 5000,
                                                              .wordDelimiters = "secret-delimiters" },
              .clientArea = proto::WireClientArea { .columns = 132, .lines = 43 },
              .attachIntent = { .kind = proto::AttachIntentKind::Resume, .windows = { 1, 7 } },
              .profileName = "work" } } },
        { proto::PduType::ServerHello,
          proto::DecodedPdu { proto::ServerHello { .agreedVersion = 4,
                                                   .capabilities = { .idents = { 1 } },
                                                   .machineId = "secret-machine-id",
                                                   .hostname = "secret-host",
                                                   .followedWindows = { 1, 3 } } } },
        { proto::PduType::Input,
          proto::DecodedPdu { proto::Input {
              .session = 4, .data = { std::byte { 'h' }, std::byte { 'i' }, std::byte { '!' } } } } },
        { proto::PduType::ResizeRequest,
          proto::DecodedPdu { proto::ResizeRequest { .columns = 120, .lines = 40 } } },
        { proto::PduType::ResizePane,
          proto::DecodedPdu { proto::ResizePane { .session = 4, .columns = 60, .lines = 20 } } },
        { proto::PduType::FetchImage,
          proto::DecodedPdu { proto::FetchImage { .session = 4, .imageId = 9 } } },
        { proto::PduType::ImageData,
          proto::DecodedPdu { proto::ImageData {
              .imageId = 9, .format = 1, .width = 32, .height = 16, .data = std::vector<std::byte>(64) } } },
        { proto::PduType::ImageGone, proto::DecodedPdu { proto::ImageGone { .imageId = 9 } } },
        { proto::PduType::SessionState, proto::DecodedPdu { sampleSessionState() } },
        { proto::PduType::Delta, proto::DecodedPdu { sampleDelta() } },
        { proto::PduType::SessionBell, proto::DecodedPdu { proto::SessionBell { .session = 4 } } },
        { proto::PduType::SessionNotify,
          proto::DecodedPdu {
              proto::SessionNotify { .session = 4, .title = "secret-title", .body = "secret-body" } } },
        { proto::PduType::SessionClipboard,
          proto::DecodedPdu {
              proto::SessionClipboard { .session = 4, .selection = "c", .data = "/home/user/secret" } } },
        { proto::PduType::LayoutState,
          proto::DecodedPdu {
              proto::LayoutState { .window = 1, .activeTab = 0, .tabs = std::vector<proto::WireTab>(2) } } },
        { proto::PduType::CreateTab,
          proto::DecodedPdu { proto::CreateTab { .session = 4, .profileName = "work" } } },
        { proto::PduType::SplitPane,
          proto::DecodedPdu { proto::SplitPane { .session = 4, .orientation = 1, .ratio = 5000 } } },
        { proto::PduType::ClosePane, proto::DecodedPdu { proto::ClosePane { .session = 4 } } },
        { proto::PduType::NewWindow, proto::DecodedPdu { proto::NewWindow {} } },
        { proto::PduType::ResizeSplit,
          proto::DecodedPdu { proto::ResizeSplit { .firstSession = 4, .secondSession = 7, .ratio = 6000 } } },
        { proto::PduType::SessionSettingsChanged,
          proto::DecodedPdu { proto::SessionSettingsChanged {
              .sessionSettings = proto::WireSessionSettings { .wordDelimiters = "secret-delimiters" } } } },
        { proto::PduType::PerformOperation,
          proto::DecodedPdu { proto::PerformOperation { .session = 4,
                                                        .operation = proto::EngineOperation::WriteScreen,
                                                        .text = "secret-screen-text" } } },
        { proto::PduType::PermissionRequest,
          proto::DecodedPdu {
              proto::PermissionRequest { .session = 4,
                                         .hostId = "secret-key@/home/user/secret.wasm",
                                         .name = "secret-name",
                                         .version = 1,
                                         .digest = "5f3c",
                                         .commands = { proto::WireDeclaredCommand {
                                             .name = "secret-command", .argv = { "secret-argv" } } } } } },
        { proto::PduType::PermissionReply,
          proto::DecodedPdu { proto::PermissionReply {
              .hostId = "secret-key@/home/user/secret.wasm", .digest = "5f3c", .grantState = 1 } } },
        { proto::PduType::Unsupported,
          proto::DecodedPdu { proto::Unsupported { .refusedSerial = 9, .ident = 200 } } },
        { proto::PduType::DaemonStatusRequest, proto::DecodedPdu { proto::DaemonStatusRequest {} } },
        { proto::PduType::DaemonStatusReply,
          proto::DecodedPdu { proto::DaemonStatusReply { .releaseVersion = "0.8.1",
                                                         .processId = 4242,
                                                         .uptimeSeconds = 7385,
                                                         .socketPath = "/run/user/1000/contour/default",
                                                         .sessions = 3,
                                                         .clients = 1 } } },
        { proto::PduType::StopDaemon, proto::DecodedPdu { proto::StopDaemon { .reason = 1 } } },
        { proto::PduType::EditTab,
          proto::DecodedPdu {
              proto::EditTab { .session = 4, .kind = proto::TabEditKind::SetTitle, .title = "build" } } },
        { proto::PduType::Heartbeat, proto::DecodedPdu { proto::Heartbeat { .nonce = 7 } } },
    };
}

} // namespace vthost::testing
