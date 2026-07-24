// SPDX-License-Identifier: Apache-2.0
#include <vthost/imsg/Identify.h>

#include <array>
#include <cstring>
#include <format>

namespace vthost::imsg
{

namespace
{
    /// The payload shape a row validates before its apply runs.
    enum class PayloadKind : uint8_t
    {
        Empty,   ///< datalen 0 (the fd travels in ancillary data).
        CString, ///< NUL-terminated (tmux kills peers whose last byte isn't NUL).
        Int32,   ///< exactly sizeof(int).
        UInt64,  ///< exactly sizeof(uint64_t).
        Pid,     ///< exactly sizeof(pid_t) — an int32 on every supported platform.
    };

    using ApplyFn = void (*)(IdentifyState&, ImsgFrame&);

    struct IdentifyRow
    {
        uint32_t type;
        PayloadKind kind;
        ApplyFn apply;
    };

    [[nodiscard]] std::string payloadString(ImsgFrame const& frame)
    {
        // The shape check guaranteed a trailing NUL; strip it.
        return { reinterpret_cast<char const*>(frame.payload.data()), frame.payload.size() - 1 };
    }

    template <typename T>
    [[nodiscard]] T payloadValue(ImsgFrame const& frame)
    {
        auto value = T {};
        std::memcpy(&value, frame.payload.data(), sizeof(T));
        return value;
    }

    // One row per identify message; adding a message is adding a row.
    constexpr auto IdentifyTable = std::to_array<IdentifyRow>({
        { msgtype::IdentifyFlags, // legacy 32-bit flags (3.7b sends LONGFLAGS)
          PayloadKind::Int32,
          [](IdentifyState& state, ImsgFrame& frame) {
              state.flags |= static_cast<uint32_t>(payloadValue<int32_t>(frame));
          } },
        { msgtype::IdentifyLongFlags,
          PayloadKind::UInt64,
          [](IdentifyState& state, ImsgFrame& frame) { state.flags = payloadValue<uint64_t>(frame); } },
        { msgtype::IdentifyTerm,
          PayloadKind::CString,
          [](IdentifyState& state, ImsgFrame& frame) { state.term = payloadString(frame); } },
        { msgtype::IdentifyTtyName,
          PayloadKind::CString,
          [](IdentifyState& state, ImsgFrame& frame) { state.ttyName = payloadString(frame); } },
        { msgtype::IdentifyCwd,
          PayloadKind::CString,
          [](IdentifyState& state, ImsgFrame& frame) { state.cwd = payloadString(frame); } },
        { msgtype::IdentifyFeatures,
          PayloadKind::Int32,
          [](IdentifyState& state, ImsgFrame& frame) { state.features = payloadValue<int32_t>(frame); } },
        { msgtype::IdentifyTerminfo,
          PayloadKind::CString,
          [](IdentifyState& state, ImsgFrame& frame) { state.terminfo.push_back(payloadString(frame)); } },
        { msgtype::IdentifyEnviron,
          PayloadKind::CString,
          [](IdentifyState& state, ImsgFrame& frame) {
              // The real server stores only entries containing '='.
              if (auto entry = payloadString(frame); entry.contains('='))
                  state.environment.push_back(std::move(entry));
          } },
        { msgtype::IdentifyClientPid,
          PayloadKind::Pid,
          [](IdentifyState& state, ImsgFrame& frame) { state.clientPid = payloadValue<int32_t>(frame); } },
        { msgtype::IdentifyStdin,
          PayloadKind::Empty,
          [](IdentifyState& state, ImsgFrame& frame) { state.stdinFd = std::move(frame.fd); } },
        { msgtype::IdentifyStdout,
          PayloadKind::Empty,
          [](IdentifyState& state, ImsgFrame& frame) { state.stdoutFd = std::move(frame.fd); } },
        { msgtype::IdentifyDone,
          PayloadKind::Empty,
          [](IdentifyState& state, ImsgFrame&) { state.done = true; } },
        { msgtype::IdentifyOldCwd, // unused upstream; accepted and ignored
          PayloadKind::CString,
          [](IdentifyState&, ImsgFrame&) {} },
    });

    [[nodiscard]] bool shapeOk(PayloadKind kind, std::vector<std::byte> const& payload)
    {
        switch (kind)
        {
            case PayloadKind::Empty: return payload.empty();
            case PayloadKind::CString: return !payload.empty() && payload.back() == std::byte { 0 };
            case PayloadKind::Int32: return payload.size() == sizeof(int32_t);
            case PayloadKind::UInt64: return payload.size() == sizeof(uint64_t);
            case PayloadKind::Pid: return payload.size() == sizeof(int32_t);
        }
        return false;
    }
} // namespace

std::expected<void, ImsgError> applyIdentify(IdentifyState& state, ImsgFrame frame)
{
    if (state.done)
        return std::unexpected(ImsgError::IdentifyAfterDone);

    for (auto const& row: IdentifyTable)
    {
        if (row.type != frame.type)
            continue;
        if (!shapeOk(row.kind, frame.payload))
            return std::unexpected(row.kind == PayloadKind::CString ? ImsgError::MalformedString
                                                                    : ImsgError::WrongPayloadSize);
        row.apply(state, frame);
        return {};
    }
    return {}; // unknown identify types are ignored, as the real server does
}

std::expected<void, RejectReason> checkAcceptance(IdentifyState const& state)
{
    if ((state.flags & ClientControlControl) != 0)
        return std::unexpected(RejectReason::ControlControl);
    if ((state.flags & ClientControl) == 0)
        return std::unexpected(RejectReason::NotControlClient);
    if (!state.stdinFd.valid() || !state.stdoutFd.valid())
        return std::unexpected(RejectReason::MissingStdioFds);
    return {};
}

std::string rejectMessage(RejectReason reason)
{
    switch (reason)
    {
        case RejectReason::NotControlClient:
            return "contour daemon: only control-mode (-C) tmux clients are supported on this socket";
        case RejectReason::ControlControl: return "contour daemon: -CC is not supported; use -C";
        case RejectReason::MissingStdioFds:
            return "contour daemon: the identify handshake carried no stdio descriptors";
    }
    return "contour daemon: rejected";
}

} // namespace vthost::imsg
