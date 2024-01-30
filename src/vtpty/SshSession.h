// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>

#include <crispy/file_descriptor.h>
#include <crispy/logstore.h>
#include <crispy/overloaded.h>
#include <crispy/result.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <variant>

namespace vtpty
{

struct SshHostConfig
{
    using Environment = std::map<std::string, std::string>;

    std::string hostname;
    int port = 22;
    std::string username;
    std::filesystem::path privateKeyFile;
    std::filesystem::path publicKeyFile;
    std::filesystem::path knownHostsFile;
    bool forwardAgent = false;
    Environment env;

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::string toConfigString(std::string const& host = {}) const;
};

using SshHostConfigMap = std::map<std::string, SshHostConfig>;

crispy::result<SshHostConfigMap> loadSshConfig(std::filesystem::path const& configPath);
crispy::result<SshHostConfigMap> loadSshConfig();

/// SSH Login session.
class SshSession final: public Pty
{
  public:
    // clang-format off
    struct NormalExit { int exitCode; };
    struct SignalExit { std::string signal; std::string errorMessage; std::string languageTag; };
    // clang-format on

    using ExitStatus = std::variant<NormalExit, SignalExit>;
    using Environment = std::map<std::string, std::string>;

    explicit SshSession(SshHostConfig config);
    ~SshSession() override;

    void start() override;
    PtySlave& slave() noexcept override;
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    [[nodiscard]] ReadResult read(crispy::buffer_object<char>& storage,
                                  std::optional<std::chrono::milliseconds> timeout,
                                  size_t size) override;
    void wakeupReader() override;
    [[nodiscard]] int write(std::string_view buf) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    [[nodiscard]] bool isOperational() const noexcept;

    [[nodiscard]] std::optional<ExitStatus> exitStatus() const;

    enum class State
    {
        Initial,                            // initial state
        Started,                            // start() has been called
        Connect,                            // connect to SSH server (usually TCP/IPv4 or TCP/IPv6)
        Handshake,                          // SSH handshake
        VerifyHostKey,                      // verify host key against known_hosts file
        AuthenticateAgent,                  // authenticate with SSH agent
        AuthenticatePrivateKeyStart,        // start private key authentication
        AuthenticatePrivateKeyRequest,      // request private key's password
        AuthenticatePrivateKeyWaitForInput, // wait for private key's password (user input))
        AuthenticatePrivateKey,             // authenticate with private key
        AuthenticatePasswordStart,          // start password authentication
        AuthenticatePasswordWaitForInput,   // wait for password (user input)
        AuthenticatePassword,               // authenticate with password
        OpenChannel,                        // open SSH channel
        RequestAuthAgent,                   // request SSH (auth) agent forwarding
        RequestPty,                         // request pty
        SetEnv,                             // set environment variables
        StartShell,                         // shell is starting
        Operational,                        // setup done, and shell is running
        ResizeScreen,                       // resize screen, then move to operational
        Failure,                            // connection closed with protocol related error
        Closed,                             // connection closed by peer or us
    };

    // Wait for the socket to become readable and/or writable.
    int waitForSocket(std::optional<std::chrono::milliseconds> timeout = std::nullopt);

  private:
    bool connect(std::string_view host, int port);
    void logErrorWithDetails(int libssl2ErrorCode, std::string_view message) const;
    bool verifyHostKey();
    bool authenticateWithAgent();
    void authenticateWithPrivateKey();
    void authenticateWithPassword();

    void handlePreAuthenticationPasswordInput(std::string_view buf, State next);

    void injectRead(std::string_view buf);
    void injectWrite(std::string_view buf);

    // Handles each individual states.
    void processState();

    void setState(State nextState);

    void logInfo(std::string_view message) const;

    template <typename... Args>
    void logInfo(fmt::format_string<Args...> fmt, Args&&... args) const
    {
        logInfo(fmt::format(fmt, std::forward<Args>(args)...));
    }

    void logError(std::string_view message) const;

    template <typename... Args>
    void logError(fmt::format_string<Args...> fmt, Args&&... args) const
    {
        logError(fmt::format(fmt, std::forward<Args>(args)...));
    }

    SshHostConfig _config;

    PageSize _pageSize { .lines = LineCount(24), .columns = ColumnCount(80) };
    std::optional<ImageSize> _pixels = std::nullopt;
    std::unique_ptr<PtySlave> _ptySlave;
    std::mutex _mutex;

    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _p;

    State _state = State::Initial;
    int _walkIndex = 0;

    std::string _injectedWrite;
    std::string _injectedRead;
    std::mutex _injectMutex;
    std::condition_variable _injectCV;

    std::mutex _closedMutex;
    std::condition_variable _closedCV;
};

} // namespace vtpty

template <>
struct fmt::formatter<vtpty::SshSession::State>: fmt::formatter<std::string_view>
{
    auto format(vtpty::SshSession::State const& state, format_context& ctx) -> format_context::iterator
    {
        std::string_view name;
        // clang-format off
        switch (state)
        {
            case vtpty::SshSession::State::Initial: name = "Initial"; break;
            case vtpty::SshSession::State::Started: name = "Started"; break;
            case vtpty::SshSession::State::Connect: name = "Connect"; break;
            case vtpty::SshSession::State::Handshake: name = "Handshake"; break;
            case vtpty::SshSession::State::VerifyHostKey: name = "VerifyHostKey"; break;
            case vtpty::SshSession::State::AuthenticateAgent: name = "AuthenticateAgent"; break;
            case vtpty::SshSession::State::AuthenticatePrivateKeyStart: name = "AuthenticatePrivateKeyStart"; break;
            case vtpty::SshSession::State::AuthenticatePrivateKeyRequest: name = "AuthenticatePrivateKeyRequest"; break;
            case vtpty::SshSession::State::AuthenticatePrivateKeyWaitForInput: name = "AuthenticatePrivateKeyWaitForInput"; break;
            case vtpty::SshSession::State::AuthenticatePrivateKey: name = "AuthenticatePrivateKey"; break;
            case vtpty::SshSession::State::AuthenticatePasswordStart: name = "AuthenticatePasswordStart"; break;
            case vtpty::SshSession::State::AuthenticatePasswordWaitForInput: name = "AuthenticatePasswordWaitForInput"; break;
            case vtpty::SshSession::State::AuthenticatePassword: name = "AuthenticatePassword"; break;
            case vtpty::SshSession::State::OpenChannel: name = "OpenChannel"; break;
            case vtpty::SshSession::State::RequestAuthAgent: name = "RequestAuthAgent"; break;
            case vtpty::SshSession::State::RequestPty: name = "RequestPty"; break;
            case vtpty::SshSession::State::SetEnv: name = "SetEnv"; break;
            case vtpty::SshSession::State::StartShell: name = "StartShell"; break;
            case vtpty::SshSession::State::Operational: name = "Operational"; break;
            case vtpty::SshSession::State::ResizeScreen: name = "ResizeScreen"; break;
            case vtpty::SshSession::State::Failure: name = "Failure"; break;
            case vtpty::SshSession::State::Closed: name = "Closed"; break;
        }
        // clang-format on
        return fmt::formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtpty::SshSession::ExitStatus>: fmt::formatter<std::string>
{
    auto format(vtpty::SshSession::ExitStatus const& status, format_context& ctx) -> format_context::iterator
    {
        return std::visit(overloaded { [&](vtpty::SshSession::NormalExit exit) {
                                          return fmt::formatter<std::string>::format(
                                              fmt::format("{} (normal exit)", exit.exitCode), ctx);
                                      },
                                       [&](vtpty::SshSession::SignalExit exit) {
                                           return fmt::formatter<std::string>::format(
                                               fmt::format("{} ({})", exit.signal, exit.errorMessage), ctx);
                                       } },
                          status);
    }
};
