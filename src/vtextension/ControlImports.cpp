// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ControlImports.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Json.hpp>
#include <vtextension/ControlCodec.hpp>

namespace vtextension
{

namespace
{
    using Catalog = std::remove_cvref_t<decltype(vtcontrol::Methods)>;

    /// The catalog row of each import, in import order: the rows an extension may call.
    constexpr auto ImportRows = [] {
        auto rows = std::array<std::size_t, ControlImportCount> {};
        auto next = std::size_t { 0 };
        auto index = std::size_t { 0 };
        std::apply(
            [&](auto const&... row) {
                ((row.exposure == vtcontrol::Exposure::Everywhere ? (rows[next++] = index++) : index++), ...);
            },
            vtcontrol::Methods);
        return rows;
    }();

    /// The longest WIT func name an import may have, NUL included.
    constexpr auto MaxImportNameBytes = std::size_t { 48 };

    /// @return @p name as its WIT func is spelled, kebab-case (`object.setLabel` is `object-set-label`),
    ///         NUL-terminated: the same spelling as vtcontrol::kebabCase(), which the WIT is rendered with.
    [[nodiscard]] consteval auto witFuncName(std::string_view name)
    {
        auto text = std::array<char, MaxImportNameBytes> {};
        auto length = std::size_t { 0 };
        for (auto const ch: name)
        {
            if (ch == '.' || ch == '_')
                text.at(length++) = '-';
            else if (ch >= 'A' && ch <= 'Z')
            {
                if (length != 0 && text.at(length - 1) != '-')
                    text.at(length++) = '-';
                text.at(length++) = static_cast<char>(ch - 'A' + 'a');
            }
            else
                text.at(length++) = ch;
        }
        text.at(length) = '\0'; // fails to compile, through at(), for a name too long
        return text;
    }

    /// The WIT func name of import @p Import, for as long as the process lives.
    template <std::size_t Import>
    constexpr auto ImportName = witFuncName(std::get<ImportRows[Import]>(vtcontrol::Methods).name);

    /// The catalog row of import @p Import.
    template <std::size_t Import>
    using RowOf = std::tuple_element_t<ImportRows[Import], Catalog>;

    /// The request type of import @p Import.
    template <std::size_t Import>
    using RequestOf = RowOf<Import>::Request;

    /// Whether import @p Import takes its request by address rather than flat.
    template <std::size_t Import>
    constexpr bool TakesRequestByAddress = abi::Codec<RequestOf<Import>>::FlatCount > MaxFlatParameters;

    /// How many i32s import @p Import takes: its request, flat or by address, then its result's address.
    template <std::size_t Import>
    constexpr uint32_t ParameterCount =
        TakesRequestByAddress<Import> ? 2U
                                      : static_cast<uint32_t>(abi::Codec<RequestOf<Import>>::FlatCount) + 1U;

    constexpr auto HostFunctions = []<std::size_t... Import>(std::index_sequence<Import...>) {
        return std::array<wasm::HostFunction, ControlImportCount> { wasm::HostFunction {
            .module = ControlImportModule,
            .name = std::string_view { ImportName<Import>.data() },
            .parameterCount = ParameterCount<Import> }... };
    }(std::make_index_sequence<ControlImportCount> {});

    [[nodiscard]] std::unexpected<vtcontrol::ControlError> controlError(vtcontrol::ControlErrorCode code,
                                                                        std::string message)
    {
        return std::unexpected(
            vtcontrol::ControlError { .code = code, .message = std::move(message), .candidates = {} });
    }

    /// @return Why @p row may not be called by this extension; nothing if it may.
    template <typename Row>
    [[nodiscard]] std::expected<void, vtcontrol::ControlError> admit(Row const& row,
                                                                     IHostServices& services,
                                                                     vtcontrol::ControlGrants declared)
    {
        auto const permission = vtcontrol::nameOf(row.permission);
        if (!declared.contains(row.permission))
            return controlError(
                vtcontrol::ControlErrorCode::Forbidden,
                std::format("method '{}' needs the manifest line 'control {}'", row.name, permission));
        switch (services.controlConsent())
        {
            case GrantState::Granted: return {};
            case GrantState::Pending:
                return controlError(
                    vtcontrol::ControlErrorCode::Forbidden,
                    std::format("method '{}' needs grant '{}': not granted yet", row.name, permission));
            case GrantState::Denied:
                return controlError(
                    vtcontrol::ControlErrorCode::Forbidden,
                    std::format("method '{}' needs grant '{}': denied", row.name, permission));
        }
        return controlError(vtcontrol::ControlErrorCode::Forbidden, "no grant");
    }

    /// @return Why @p request may not be called by this extension for what it asks @p row to do, under the
    ///         row's requestGrant rule (`tab.create` naming a command needs `input`), the same rule
    ///         dispatch() applies; nothing if it may. The consent admit() checked covers every declared
    ///         permission.
    template <typename Row>
    [[nodiscard]] std::expected<void, vtcontrol::ControlError> admitRequest(
        Row const& row, typename Row::Request const& request, vtcontrol::ControlGrants declared)
    {
        auto const extra = vtcontrol::requestGrantOf(row.requestGrant, request);
        if (!extra || declared.contains(extra->permission))
            return {};
        return controlError(vtcontrol::ControlErrorCode::Forbidden,
                            std::format("method '{}': {} needs the manifest line 'control {}'",
                                        row.name,
                                        extra->reason,
                                        vtcontrol::nameOf(extra->permission)));
    }

    /// @return Why @p request may not be served to an extension though its grant covers it; nothing if it
    /// may.
    template <typename Request>
    [[nodiscard]] std::expected<void, vtcontrol::ControlError> refuseBlocking(Request const& request)
    {
        // A guest runs under a deadline of milliseconds, and must not block on a shell: `pane.send` waits for
        // nothing when an extension calls it. The socket API knows no such rule; dispatch() is not told.
        if constexpr (std::is_same_v<Request, vtcontrol::SendRequest>)
            if (request.wait != vtcontrol::SendWait::None)
                return controlError(
                    vtcontrol::ControlErrorCode::BadRequest,
                    "method 'pane.send' cannot wait when an extension calls it: 'wait' must be 'none'");
        return {};
    }

    /// Asks the host to serve @p request, and decodes its reply.
    template <typename Row>
    [[nodiscard]] std::expected<typename Row::Reply, vtcontrol::ControlError> serve(
        Row const& row,
        typename Row::Request const& request,
        IHostServices& services,
        vtcontrol::ControlGrants granted,
        std::chrono::milliseconds timeout)
    {
        using Reply = Row::Reply;
        return services.callControl(row.name, vtcontrol::encodeJson(request), granted, timeout)
            .and_then([&](std::string const& json) -> std::expected<Reply, vtcontrol::ControlError> {
                auto decoded = vtcontrol::decodeJson<Reply>(json);
                if (decoded)
                    return decoded;
                return controlError(
                    vtcontrol::ControlErrorCode::Failed,
                    std::format("the reply to '{}' does not decode: {}", row.name, decoded.error().message));
            });
    }

    /// Serves import @p Import: lift, admit, serve, store. @see callControlImport()
    template <std::size_t Import>
    [[nodiscard]] abi::Result<void> callImport(abi::IGuestMemory& memory,
                                               std::span<uint32_t const> arguments,
                                               IHostServices& services,
                                               vtcontrol::ControlGrants declared,
                                               ControlCallBudget budget)
    {
        using Row = RowOf<Import>;
        using Request = Row::Request;
        using Result = std::expected<typename Row::Reply, vtcontrol::ControlError>;
        static constexpr auto const& Method = std::get<ImportRows[Import]>(vtcontrol::Methods);

        auto const resultAddress = arguments.back();
        auto const request = [&]() -> abi::Result<Request> {
            if constexpr (TakesRequestByAddress<Import>)
                return abi::Codec<Request>::load(memory, arguments.front());
            else
            {
                auto reader = abi::FlatReader(arguments.first(arguments.size() - 1));
                return abi::Codec<Request>::liftFlat(memory, reader);
            }
        }();
        if (!request)
            return std::unexpected(request.error());
        // Where the result goes is proven inside the guest's memory before the host acts on the request: a
        // guest that is to trap must not have split a pane first.
        if (auto const inRange = abi::detail::checkRange(memory, resultAddress, abi::Codec<Result>::Size);
            !inRange)
            return std::unexpected(inRange.error());

        if (budget.remaining <= std::chrono::milliseconds::zero()
            || budget.deadline.load(std::memory_order_acquire) == DeadlineState::Expired)
            return std::unexpected(ExtensionError::DeadlineExceeded);

        auto const result = admit(Method, services, declared)
                                .and_then([&] { return admitRequest(Method, *request, declared); })
                                .and_then([&] { return refuseBlocking(*request); })
                                .and_then([&]() -> Result {
                                    return serve(Method, *request, services, declared, budget.remaining);
                                });

        // The host answered; if the guest's time ran out meanwhile, the watchdog is interrupting it, and the
        // answer -- however it came -- is dropped here rather than written into the memory of a guest whose
        // call has already failed.
        if (budget.deadline.load(std::memory_order_acquire) == DeadlineState::Expired)
            return std::unexpected(ExtensionError::DeadlineExceeded);
        return abi::Codec<Result>::store(memory, resultAddress, result);
    }

    using ImportHandler = abi::Result<void> (*)(abi::IGuestMemory&,
                                                std::span<uint32_t const>,
                                                IHostServices&,
                                                vtcontrol::ControlGrants,
                                                ControlCallBudget);

    /// What serves each import, by its index.
    constexpr auto ImportHandlers = []<std::size_t... Import>(std::index_sequence<Import...>) {
        return std::array<ImportHandler, ControlImportCount> { &callImport<Import>... };
    }(std::make_index_sequence<ControlImportCount> {});
} // namespace

std::span<wasm::HostFunction const> controlHostFunctions() noexcept
{
    return HostFunctions;
}

abi::Result<void> callControlImport(std::size_t importIndex,
                                    abi::IGuestMemory& memory,
                                    std::span<uint32_t const> arguments,
                                    IHostServices& services,
                                    vtcontrol::ControlGrants declared,
                                    ControlCallBudget budget)
{
    // The runtime calls each import with its own arity; anything else is a guest the host cannot serve.
    if (importIndex >= ImportHandlers.size()
        || arguments.size() != HostFunctions.at(importIndex).parameterCount)
        return std::unexpected(ExtensionError::Trapped);
    return ImportHandlers.at(importIndex)(memory, arguments, services, declared, budget);
}

vtcontrol::ControlGrants controlGrantsOf(std::span<vtcontrol::Permission const> permissions) noexcept
{
    auto grants = vtcontrol::ControlGrants::none();
    for (auto const permission: permissions)
        grants = grants.with(permission);
    return grants;
}

} // namespace vtextension
