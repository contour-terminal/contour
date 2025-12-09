// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>

#include <crispy/file_descriptor.h>
#include <crispy/logstore.h>
#include <crispy/overloaded.h>
#include <crispy/result.h>

#include <condition_variable>
#include <functional>
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
    [[nodiscard]] std::string toKnownhostComment() const;
};

using SshHostConfigMap = std::map<std::string, SshHostConfig>;

crispy::result<SshHostConfigMap> loadSshConfig(std::filesystem::path const& configPath);
crispy::result<SshHostConfigMap> loadSshConfig();

/// SSH Host Key information.
///
/// This structure holds the SSH host key type and its raw binary data.
struct SshHostkey
{
    /// key type, e.g., LIBSSH2_HOSTKEY_TYPE_RSA
    int type;

    /// raw binary key data
    std::vector<char> data;
};

struct SshHostkeyHash
{
    int keyType;

    /// raw binary fingerprint data
    std::vector<char> fingerprint;

    explicit SshHostkeyHash(int keyType, char const* data, size_t length):
        keyType { keyType }, fingerprint { data ? data : "", data ? data + length : nullptr }
    {
    }

    [[nodiscard]] std::string toString() const
    {
        std::string result;
        for (size_t i = 0; i < fingerprint.size(); ++i)
        {
            result += std::format("{:02X}", static_cast<unsigned char>(fingerprint[i]));
            if (i + 1 < fingerprint.size())
                result += ":";
        }
        return result;
    }
};

/// SSH Host Key Verification Request.
///
/// This structure is used to request user verification of the SSH host key
/// during the SSH handshake process.
struct SshHostkeyVerificationRequest
{
    /// Hostname (and possibly port) of the SSH server to connect to.
    std::string hostname;

    /// Port of the SSH server to connect to.
    int port;

    /// Host key hash information.
    SshHostkeyHash hostkeyHash;
};

/// Callback type for responding to host key verification requests.
using SshHostkeyVerificationResponseCallback = std::function<void(bool accepted)>;

/// Callback type for host key verification requests.
using SshHostkeyVerificationRequestCallback = std::function<void(
    SshHostkeyVerificationRequest const& request, SshHostkeyVerificationResponseCallback const& response)>;

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

    explicit SshSession(SshHostConfig config, SshHostkeyVerificationRequestCallback hostkeyRequestCallback);
    ~SshSession() override;

    void start() override;
    PtySlave& slave() noexcept override;
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    [[nodiscard]] std::optional<ReadResult> read(crispy::buffer_object<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override;
    void wakeupReader() override;
    [[nodiscard]] int write(std::string_view buf) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    [[nodiscard]] bool isOperational() const noexcept;

    [[nodiscard]] std::optional<ExitStatus> exitStatus() const;

    enum class State : uint8_t
    {
        Initial,                            // initial state
        Started,                            // start() has been called
        Connect,                            // connect to SSH server (usually TCP/IPv4 or TCP/IPv6)
        Handshake,                          // SSH handshake
        VerifyHostkey,                      // verify host key against known_hosts file
        VerifyHostkeyWaitForInput,          // wait for user confirmation of host key
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

    enum class HostkeyVerificationStatus : uint8_t
    {
        Skipped,
        FailedWithMismatch,
        FailedToReadKnownHosts,
        FailedByUserTrust,
        Verified,
        WaitingForUserConfirmation,
    };

    /// Verifies the remote host key against the known_hosts file.
    ///
    /// If the host key is not found, the user is prompted to accept it.
    /// If the host key does not match, the connection is aborted.
    ///
    /// @return true if the host key is verified, false otherwise.
    [[nodiscard]] HostkeyVerificationStatus verifyHostkey();

    void hostkeyVerificationResultCallback(bool accepted);

    bool authenticateWithAgent();
    void authenticateWithPrivateKey();
    void authenticateWithPassword();

    void handlePreAuthenticationPasswordInput(std::string_view buf, State next);

    void injectRead(std::string_view buf);
    void injectWrite(std::string_view buf);

    // Handles each individual states.
    void processState();

    // some of the complex states to handle
    bool requestPty();
    bool setEnv();
    void resizeScreen();

    void setState(State nextState);

    void logInfo(std::string_view message) const;
    void logInject(std::string_view message) const;
    void logInfoWithInject(std::string_view message) const;

    template <typename... Args>
    void logInfoWithInject(std::format_string<Args...> fmt, Args&&... args) const
    {
        logInfoWithInject(std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void logInfo(std::format_string<Args...> fmt, Args&&... args) const
    {
        logInfo(std::format(fmt, std::forward<Args>(args)...));
    }

    void logError(std::string_view message) const;

    template <typename... Args>
    void logError(std::format_string<Args...> fmt, Args&&... args) const
    {
        logError(std::format(fmt, std::forward<Args>(args)...));
    }

    SshHostConfig _config;
    std::optional<bool> _hostkeyVerified = std::nullopt;
    SshHostkeyVerificationRequestCallback _hostkeyRequestCallback;

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
struct std::formatter<vtpty::SshSession::State>: std::formatter<std::string_view>
{
    auto format(vtpty::SshSession::State const& state, auto& ctx) const
    {
        std::string_view name;
        // clang-format off
        switch (state)
        {
            case vtpty::SshSession::State::Initial: name = "Initial"; break;
            case vtpty::SshSession::State::Started: name = "Started"; break;
            case vtpty::SshSession::State::Connect: name = "Connect"; break;
            case vtpty::SshSession::State::Handshake: name = "Handshake"; break;
            case vtpty::SshSession::State::VerifyHostkey: name = "VerifyHostkey"; break;
            case vtpty::SshSession::State::VerifyHostkeyWaitForInput: name = "VerifyHostkeyWaitForInput"; break;
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
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtpty::SshSession::ExitStatus>: std::formatter<std::string>
{
    auto format(vtpty::SshSession::ExitStatus const& status, auto& ctx) const
    {
        return std::visit(overloaded { [&](vtpty::SshSession::NormalExit exit) {
                                          return std::formatter<std::string>::format(
                                              std::format("{} (normal exit)", exit.exitCode), ctx);
                                      },
                                       [&](vtpty::SshSession::SignalExit exit) {
                                           return std::formatter<std::string>::format(
                                               std::format("{} ({})", exit.signal, exit.errorMessage), ctx);
                                       } },
                          status);
    }
};

template <>
struct std::formatter<vtpty::SshHostConfig>: std::formatter<std::string>
{
    auto format(vtpty::SshHostConfig const& config, auto& ctx) const
    {
        return std::formatter<std::string>::format(config.toString(), ctx);
    }
};
