// SPDX-License-Identifier: Apache-2.0

// clang-format off
#include <system_error>
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <windows.h>
    #include <ws2tcpip.h>

    // Link with ws2_32.lib, Mswsock.lib, and Advapi32.lib
    #pragma comment(lib, "Ws2_32.lib")
    #pragma comment(lib, "Mswsock.lib")
    #pragma comment(lib, "AdvApi32.lib")
#endif
// clang-format on

#include <vtpty/Process.h>
#include <vtpty/Pty.h>
#include <vtpty/SshSession.h>

#include <crispy/escape.h>
#include <crispy/utils.h>

#include <fstream>

#include <libssh2.h>
#include <libssh2_publickey.h>

#if not defined(_WIN32)
    #include <sys/ioctl.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <sys/types.h>

    #include <fcntl.h>
    #include <netdb.h>
    #include <termios.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
#endif

#define LIBSSH2_MAKE_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))

#define LIBSSH2_VERSION_CODE \
    LIBSSH2_MAKE_VERSION(LIBSSH2_VERSION_MAJOR, LIBSSH2_VERSION_MINOR, LIBSSH2_VERSION_PATCH)

#define LIBSSH2_VERSION_AT_LEAST(a, b, c) (LIBSSH2_VERSION_CODE >= LIBSSH2_MAKE_VERSION((a), (b), (c)))

#if LIBSSH2_VERSION_AT_LEAST(1, 2, 8)
    #define LIBSSH2_HANDSHAKE_FUNCTION libssh2_session_handshake
#else
    #define LIBSSH2_HANDSHAKE_FUNCTION libssh2_session_startup
#endif

// It would be nice to move to NB IO mode, but I didn't have the time to debug it.
// #define SSH_SESSION_NB_IO /* BUGGY EZ */

using crispy::file_descriptor;

using namespace std::string_literals;
using namespace std::string_view_literals;

#if defined(_WIN32)
template <>
struct crispy::close_native_handle<SOCKET>
{
    void operator()(SOCKET handle) { closesocket(handle); }
};
#endif

namespace vtpty
{

auto inline sshLog = logstore::category("ssh", "SSH I/O logger", logstore::category::state::Enabled);

// {{{ helper
namespace
{
    constexpr auto MaxPasswordTries = 3;

    template <typename T>
    std::string_view libssl2ErrorString(T rc)
    {
        switch (rc)
        {
            case LIBSSH2_ERROR_NONE: return "No error";
            case LIBSSH2_ERROR_SOCKET_NONE: return "Generic error code";
            case LIBSSH2_ERROR_BANNER_RECV: return "Banner receive failed";
            case LIBSSH2_ERROR_BANNER_SEND: return "Banner send failed";
            case LIBSSH2_ERROR_INVALID_MAC: return "Invalid MAC received";
            case LIBSSH2_ERROR_KEX_FAILURE: return "Key exchange failed";
            case LIBSSH2_ERROR_ALLOC: return "Allocation failed";
            case LIBSSH2_ERROR_SOCKET_SEND: return "Unable to send data on socket";
            case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE: return "Key exchange failed";
            case LIBSSH2_ERROR_TIMEOUT: return "Timeout";
            case LIBSSH2_ERROR_HOSTKEY_INIT: return "Host key init failed";
            case LIBSSH2_ERROR_HOSTKEY_SIGN: return "Host key sign failed";
            case LIBSSH2_ERROR_DECRYPT: return "Decrypt failed";
            case LIBSSH2_ERROR_SOCKET_DISCONNECT: return "Socket disconnected";
            case LIBSSH2_ERROR_PROTO: return "Protocol error";
            case LIBSSH2_ERROR_PASSWORD_EXPIRED: return "Password expired";
            case LIBSSH2_ERROR_FILE: return "File operation failed";
            case LIBSSH2_ERROR_METHOD_NONE: return "No matching method found";
            case LIBSSH2_ERROR_AUTHENTICATION_FAILED: return "Authentication failed";
            case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED: return "Public key unverified";
            case LIBSSH2_ERROR_CHANNEL_OUTOFORDER: return "Out of order";
            case LIBSSH2_ERROR_CHANNEL_FAILURE: return "Channel failure";
            case LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED: return "Request denied";
            case LIBSSH2_ERROR_CHANNEL_UNKNOWN: return "Unknown channel error";
            case LIBSSH2_ERROR_CHANNEL_WINDOW_EXCEEDED: return "Window exceeded";
            case LIBSSH2_ERROR_CHANNEL_PACKET_EXCEEDED: return "Packet exceeded";
            case LIBSSH2_ERROR_CHANNEL_CLOSED: return "Channel closed";
            case LIBSSH2_ERROR_CHANNEL_EOF_SENT: return "EOF sent";
            case LIBSSH2_ERROR_SCP_PROTOCOL: return "SCP protocol error";
            case LIBSSH2_ERROR_ZLIB: return "ZLib error";
            case LIBSSH2_ERROR_SOCKET_TIMEOUT: return "Socket timeout";
            case LIBSSH2_ERROR_SFTP_PROTOCOL: return "SFTP protocol error";
            case LIBSSH2_ERROR_REQUEST_DENIED: return "Request denied";
            case LIBSSH2_ERROR_METHOD_NOT_SUPPORTED: return "Method not supported";
            case LIBSSH2_ERROR_INVAL: return "Invalid value";
            case LIBSSH2_ERROR_INVALID_POLL_TYPE: return "Invalid poll type";
            case LIBSSH2_ERROR_PUBLICKEY_PROTOCOL: return "Public key protocol error";
            case LIBSSH2_ERROR_EAGAIN: return "EAGAIN";
            case LIBSSH2_ERROR_BUFFER_TOO_SMALL: return "Buffer too small";
            case LIBSSH2_ERROR_BAD_USE: return "Bad use";
            case LIBSSH2_ERROR_COMPRESS: return "Compression error";
            case LIBSSH2_ERROR_OUT_OF_BOUNDARY: return "Out of boundary";
            case LIBSSH2_ERROR_AGENT_PROTOCOL: return "Agent protocol error";
            case LIBSSH2_ERROR_SOCKET_RECV: return "Unable to receive data from socket";
            case LIBSSH2_ERROR_ENCRYPT: return "Encryption error";
            case LIBSSH2_ERROR_BAD_SOCKET: return "Bad socket";
            case LIBSSH2_ERROR_KNOWN_HOSTS: return "Known hosts error";
#if defined(LIBSSH2_ERROR_CHANNEL_WINDOW_FULL)
            case LIBSSH2_ERROR_CHANNEL_WINDOW_FULL: return "Window full";
#endif
#if defined(LIBSSH2_ERROR_KEYFILE_AUTH_FAILED)
            case LIBSSH2_ERROR_KEYFILE_AUTH_FAILED: return "Keyfile authentication failed";
#endif
#if defined(LIBSSH2_ERROR_RANDGEN)
            case LIBSSH2_ERROR_RANDGEN: return "Random generator error";
#endif
#if defined(LIBSSH2_ERROR_MISSING_USERAUTH_BANNER)
            case LIBSSH2_ERROR_MISSING_USERAUTH_BANNER: return "Missing userauth banner";
#endif
#if defined(LIBSSH2_ERROR_ALGO_UNSUPPORTED)
            case LIBSSH2_ERROR_ALGO_UNSUPPORTED: return "Unsupported algorithm";
#endif
            default: return "Unknown error";
        }
    }
} // namespace
// }}}

// {{{ SshPtySlave
class SshPtySlave final: public PtySlave
{
  public:
    void close() override {}
    [[nodiscard]] bool isClosed() const noexcept override { return false; }
    bool configure() noexcept override { return true; }
    bool login() override { return true; }
    int write(std::string_view) noexcept override { return 0; }
};
// }}}

#if defined(_WIN32)
// Why is it, that Windows API is so much more complicated than POSIX? Extrawurst!
using socket_handle = crispy::native_handle<SOCKET, INVALID_SOCKET>;
#else
using socket_handle = crispy::native_handle<int, -1>;
#endif

std::string SshHostConfig::toString() const
{
    auto result = ""s;
    auto const add = [&](std::string_view text) {
        if (!text.empty())
        {
            if (!result.empty())
                result += ", ";
            result += text;
        }
    };
    if (!username.empty())
        result += fmt::format("{}@", username);
    if (!hostname.empty())
    {
        if (port)
        {
            if (hostname.find(':') != std::string::npos)
                result += fmt::format("[{}]:{}"sv, hostname, port);
            else
                result += fmt::format("{}:{}"sv, hostname, port);
        }
        else
            result += hostname;
    }
    else if (port)
        result += "*:" + std::to_string(port);

    if (!privateKeyFile.empty())
        add(fmt::format("private key: {}", privateKeyFile.string()));

    if (!publicKeyFile.empty())
        add(fmt::format("public key: {}", privateKeyFile.string()));

    if (!knownHostsFile.empty())
        add(fmt::format("known hosts: {}", knownHostsFile.string()));

    add(fmt::format("ForwardAgent: {}", forwardAgent ? "Yes" : "No"));

    return result;
}

std::string SshHostConfig::toConfigString(std::string const& host) const
{
    std::string result;
    auto const prefix = host.empty() ? ""sv : "  "sv;

    if (!host.empty())
        result += fmt::format("Host {}\n", host);

    if (!hostname.empty())
        result += fmt::format("{}HostName {}\n", prefix, hostname);
    if (port != 22)
        result += fmt::format("{}Port {}\n", prefix, port);
    if (!username.empty())
        result += fmt::format("{}User {}\n", prefix, username);
    if (!privateKeyFile.empty())
        result += fmt::format("{}IdentityFile {}\n", prefix, privateKeyFile.string());
    if (!knownHostsFile.empty())
        result += fmt::format("{}KnownHostsFile {}\n", prefix, knownHostsFile.string());
    result += fmt::format("{}ForwardAgent {}\n", prefix, forwardAgent);
    result += fmt::format("\n");
    return result;
}

crispy::result<SshHostConfigMap> loadSshConfig(std::filesystem::path const& configPath)
{
    std::ifstream file(configPath);
    SshHostConfigMap configs;
    SshHostConfig defaultConfig;
    std::string line;
    std::string currentHost;

    if (!file)
        return crispy::failure { std::make_error_code(std::errc::no_such_file_or_directory) };

    while (std::getline(file, line))
    {
        std::istringstream iss(line);
        std::string key;
        if (!(iss >> key))
            continue; // Skip empty lines

        // Remove leading whitespace
        size_t const start = line.find_first_not_of(" \t");
        size_t const end = line.find('#'); // Ignore comments
        if (start == std::string::npos || start == end)
            continue; // Skip comment lines or empty lines

        // Isolate the key and the value
        std::string value = line.substr(line.find(' ', start) + 1, end);
        value.erase(0, value.find_first_not_of(" \t")); // Trim leading space from value

        if (key == "Host")
        {
            // If value is '*', it's a default config
            if (value == "*")
            {
                currentHost.clear();
            }
            else
            {
                currentHost = value;
                configs[currentHost] = defaultConfig; // Copy default values to new host
            }
        }
        else
        {
            // Set individual host's properties or process includes
            auto& config = currentHost.empty() ? defaultConfig : configs[currentHost];
            if (key == "HostName")
                config.hostname = value;
            else if (key == "Port")
                config.port = std::stoi(value);
            else if (key == "User")
                config.username = value;
            else if (key == "IdentityFile")
                config.privateKeyFile = value;
            else if (key == "ForwardAgent")
                config.forwardAgent = (value == "yes");
            else
                errorLog()("Unknown SSH config key: {}", key);
            // Add additional options here as needed
        }
    }

    return configs;
}

crispy::result<SshHostConfigMap> loadSshConfig()
{
    auto const configFilePath = Process::homeDirectory() / ".ssh" / "config";
    return loadSshConfig(configFilePath);
}

struct SshSession::Private
{
    LIBSSH2_SESSION* sshSession = nullptr;
    LIBSSH2_CHANNEL* sshChannel = nullptr;
    LIBSSH2_AGENT* sshAgent = nullptr;
    bool wantsWaitForSocket = false;

    socket_handle sshSocket;
};

SshSession::SshSession(SshHostConfig config):
    _config { std::move(config) },
    _ptySlave { std::make_unique<SshPtySlave>() },
    _p { new Private(), [](Private* p) {
            delete p;
        } }
{
    libssh2_init(0); // TODO: call only once?

#if defined(SSH_SESSION_NB_IO)
    libssh2_session_set_blocking(_p->sshSession, 0);
#endif

    std::atexit([]() { libssh2_exit(); });

    _p->sshSession = libssh2_session_init();
}

SshSession::~SshSession()
{
    close();

    if (_p->sshAgent)
    {
        libssh2_agent_disconnect(_p->sshAgent);
        libssh2_agent_free(_p->sshAgent);
        _p->sshAgent = nullptr;
    }

    if (_p->sshChannel)
    {
        libssh2_channel_send_eof(_p->sshChannel);
        libssh2_channel_close(_p->sshChannel);
        _p->sshChannel = nullptr;
    }

    if (_p->sshSession)
    {
        libssh2_session_disconnect(_p->sshSession, "Normal shutdown");
        libssh2_session_free(_p->sshSession);
        _p->sshSession = nullptr;
    }

#if defined(_WIN32)
    WSACleanup();
#endif
}

void SshSession::setState(State nextState)
{
    if (_state == nextState)
        return;

    sshLog()("({}) State transition from {} to {}.\n", crispy::threadName(), _state, nextState);

    _state = nextState;

    if (_state == State::Closed || _state == State::Failure)
    {
        auto const _ = std::lock_guard { _closedMutex };
        _closedCV.notify_all();
    }
}

void SshSession::processState()
{
    waitForSocket();
    while (true)
    {
        switch (_state)
        {
            case State::Initial:
                //.
                return;
            case State::Started:
                //.
                setState(State::Connect);
                [[fallthrough]];
            case State::Connect:
                if (!connect(_config.hostname, _config.port))
                    return;
                setState(State::Handshake);
                [[fallthrough]];
            case State::Handshake: {
                int const rc = LIBSSH2_HANDSHAKE_FUNCTION(_p->sshSession, _p->sshSocket);
                if (rc == LIBSSH2_ERROR_EAGAIN)
                {
                    _p->wantsWaitForSocket = true;
                    return;
                }
                if (rc != LIBSSH2_ERROR_NONE)
                {
                    logError("Failed to establish SSH session. {}", libssl2ErrorString(rc));
                    close();
                    return;
                }

                setState(State::VerifyHostKey);
                break;
            }
            case State::VerifyHostKey: {
                if (!verifyHostKey())
                    setState(State::Failure);
                else
                    setState(State::AuthenticateAgent);
                break;
            }
            case State::AuthenticateAgent: {
                authenticateWithAgent();
                break;
            }
            case State::AuthenticatePrivateKeyStart:
                _walkIndex = 0;
                // authenticateWithPrivateKey() is taking the password from _injectedWrite
                _injectedWrite = "";
                authenticateWithPrivateKey();
                break;
            case State::AuthenticatePrivateKeyRequest: {
                setState(State::AuthenticatePrivateKeyWaitForInput);
                injectRead("\U0001F511 Private key password: ");
                [[fallthrough]];
            }
            case State::AuthenticatePrivateKeyWaitForInput:
                // See handlePreAuthenticationPasswordInput()
                return;
            case State::AuthenticatePrivateKey: {
                authenticateWithPrivateKey();
                break;
            }
            case State::AuthenticatePasswordStart: {
                setState(State::AuthenticatePasswordWaitForInput);
                injectRead(fmt::format("\U0001F511 Username: {}\r\n", _config.username));
                injectRead("\U0001F511 Password: ");
                [[fallthrough]];
            }
            case State::AuthenticatePasswordWaitForInput:
                // See handlePreAuthenticationPasswordInput()
                return;
            case State::AuthenticatePassword: {
                authenticateWithPassword();
                break;
            }
            case State::OpenChannel: {
                _p->sshChannel = libssh2_channel_open_session(_p->sshSession);
                auto const rc = libssh2_session_last_errno(_p->sshSession);
                if (rc == LIBSSH2_ERROR_EAGAIN)
                {
                    _p->wantsWaitForSocket = true;
                    errno = EAGAIN;
                    return;
                }
                if (rc != LIBSSH2_ERROR_NONE)
                {
                    logError("Failed to open SSH channel. {}", libssl2ErrorString(rc));
                    setState(State::Failure);
                    return;
                }
                assert(_p->sshChannel);
                setState(State::RequestAuthAgent);
                [[fallthrough]];
            }
            case State::RequestAuthAgent: {
#if LIBSSH2_VERSION_AT_LEAST(1, 11, 0)
                if (_config.forwardAgent)
                {
                    // TODO: When having this one working, make sure to update the documentation in
                    //       docs/profiles.md
                    int const rc = libssh2_channel_request_auth_agent(_p->sshChannel);
                    if (rc == LIBSSH2_ERROR_EAGAIN)
                    {
                        _p->wantsWaitForSocket = true;
                        return;
                    }

                    if (rc != LIBSSH2_ERROR_NONE)
                        logError("Failed to request auth agent forwarding. {}", libssl2ErrorString(rc));
                    else
                        logInfo("Enabled SSH agent forwarding.");
                }
#else
                logError("Failed to request auth agent forwarding. libssh2 version too old.");
#endif
                setState(State::RequestPty);
                [[fallthrough]];
            }
            case State::RequestPty: {
                // Mode encoding defined here: https://datatracker.ietf.org/doc/html/rfc4250#section-4.5
                auto const modes = ""sv;
                auto const term = _config.env.count("TERM") ? _config.env.at("TERM") : ""s;
                auto const rc =
                    libssh2_channel_request_pty_ex(_p->sshChannel,
                                                   term.data(),
                                                   term.size(),
                                                   modes.data(),
                                                   modes.size(),
                                                   _pageSize.columns.as<int>(),
                                                   _pageSize.lines.as<int>(),
                                                   _pixels.has_value() ? _pixels->width.as<int>() : 0,
                                                   _pixels.has_value() ? _pixels->height.as<int>() : 0);
                if (rc == LIBSSH2_ERROR_EAGAIN)
                {
                    _p->wantsWaitForSocket = true;
                    return;
                }
                if (rc != LIBSSH2_ERROR_NONE)
                {
                    logError("Failed to request PTY. {}", libssl2ErrorString(rc));
                    setState(State::Failure);
                    return;
                }
                setState(State::SetEnv);
                _walkIndex = 0;
                [[fallthrough]];
            }
            case State::SetEnv: {
                int i = 0;
                for (auto const& [name, value]: _config.env)
                {
                    // Skip already set environment variables
                    if (i < _walkIndex)
                    {
                        ++i;
                        continue;
                    }

                    if (name == "TERM")
                        continue; // passed later via requestPty()

                    int const rc = libssh2_channel_setenv_ex(
                        _p->sshChannel, name.data(), name.size(), value.data(), value.size());
                    if (rc == LIBSSH2_ERROR_EAGAIN)
                    {
                        _walkIndex = i;                // remember where we left off
                        _p->wantsWaitForSocket = true; // and wait for socket to become writable
                        return;
                    }
                    if (rc != LIBSSH2_ERROR_NONE)
                    {
                        logError("Failed to set SSH environment variable \"{}\". {}",
                                 name,
                                 libssl2ErrorString(rc));
                    }
                }
                setState(State::StartShell);
                [[fallthrough]];
            }
            case State::StartShell: {
                auto const rc = libssh2_channel_shell(_p->sshChannel);
                if (rc != LIBSSH2_ERROR_NONE)
                {
                    logError("Failed to start shell. {}", libssl2ErrorString(rc));
                    setState(State::Failure);
                    return;
                }
                auto const _ = std::lock_guard { _injectMutex };
                setState(State::Operational);
                _injectCV.notify_all();
                [[fallthrough]];
            }
            case State::Operational:
                //.
                return;
            case State::ResizeScreen: {
                auto const rc = libssh2_channel_request_pty_size_ex(
                    _p->sshChannel,
                    unbox<int>(_pageSize.columns),
                    unbox<int>(_pageSize.lines),
                    _pixels.has_value() ? unbox<int>(_pixels->width) : 0,
                    _pixels.has_value() ? unbox<int>(_pixels->height) : 0);
                if (rc == LIBSSH2_ERROR_EAGAIN)
                {
                    _p->wantsWaitForSocket = true;
                    return;
                }
                if (rc != LIBSSH2_ERROR_NONE)
                {
                    logError("Failed to request PTY resize. {}", libssl2ErrorString(rc));
                    return;
                }
                setState(State::Operational);
                return;
            }
            case State::Failure:
            case State::Closed:
                //.
                return;
        }
    }
}

void SshSession::start()
{
    if (_config.port == 22)
        logInfo("Starting SSH session to host: {}@{}", _config.username, _config.hostname);
    else
        logInfo("Starting SSH session to host: {}@{}:{}", _config.username, _config.hostname, _config.port);

    assert(_state == State::Initial);
    // auto const _ = std::lock_guard { _mutex };
    setState(State::Started);
    processState();

    /*
        if (!_p->sshClient.connect(_host, _port))
            return;

        if (!_p->sshClient.handshake())
            return;

        if (!_p->sshClient.authenticate(_username))
        {
            sshLog()("SSH agent based authentication failed. Trying again with password.");
            if (!_p->sshClient.authenticate(_username, "winki"))
                return;
        }

        if (!_p->sshClient.openChannel())
            return;

        if (SshClient::supportsAuthAgentForwarding())
            _p->sshClient.requestAuthAgentForwarding(); // Continue on failure

        if (!_p->sshClient.requestPty(_env.count("TERM") ? _env.at("TERM") : "",
                                   _pageSize.columns.as<int>(),
                                   _pageSize.lines.as<int>(),
                                   _pixels.has_value() ? _pixels->width.as<int>() : 0,
                                   _pixels.has_value() ? _pixels->height.as<int>() : 0))
            return;

        _p->sshClient.setEnvironment(_env);

        if (!_p->sshClient.startShell())
            return;

        // _p->sshClient.setBlocking(false);
    */
}

PtySlave& SshSession::slave() noexcept
{
    return *_ptySlave; // Can we find a way to avoid this?
}

void SshSession::close()
{
    setState(State::Closed);

    if (_p->sshChannel)
    {
        libssh2_channel_send_eof(_p->sshChannel);
        libssh2_channel_close(_p->sshChannel);
        libssh2_channel_wait_closed(_p->sshChannel);
    }

    if (_p->sshSocket.is_open())
        _p->sshSocket.close();
}

bool SshSession::isClosed() const noexcept
{
    return _p->sshSocket.is_closed() || _state == State::Closed || _state == State::Failure;
}

void SshSession::waitForClosed()
{
    auto lock = std::unique_lock { _closedMutex };
    _closedCV.wait(lock, [this]() -> bool { return isClosed(); });
}

SshSession::ReadResult SshSession::read(crispy::buffer_object<char>& storage,
                                        std::optional<std::chrono::milliseconds> timeout,
                                        size_t size)
{
    auto injectLock = std::unique_lock { _injectMutex };
    _injectCV.wait(injectLock, [this]() { return _state == State::Operational || !_injectedRead.empty(); });

    if (!_injectedRead.empty())
    {
        auto const nread = std::min(size, _injectedRead.size());
        std::copy_n(_injectedRead.begin(), nread, storage.hotEnd());
        _injectedRead.erase(0, nread);
        return std::tuple { std::string_view { storage.hotEnd(), nread }, false };
    }

    if (_state == State::AuthenticatePasswordWaitForInput)
    {
        errno = EAGAIN;
        return std::nullopt;
    }

    // Below is for state: Operational
    processState();

    if (_state != State::Operational && !isClosed())
    {
        errno = EAGAIN;
        _p->wantsWaitForSocket = true;
        return std::nullopt;
    }

    waitForSocket(timeout);
    // auto _ = std::unique_lock { _mutex };
    auto const rc =
        libssh2_channel_read(_p->sshChannel, storage.hotEnd(), std::min(storage.bytesAvailable(), size));

    if (rc == LIBSSH2_ERROR_EAGAIN)
    {
        _p->wantsWaitForSocket = true;
        errno = EAGAIN;
        return std::nullopt;
    }

    if (rc < 0)
    {
        logError("Failed to read from SSH channel. {}", libssl2ErrorString(rc));
        errno = EIO;
        return std::nullopt;
    }

    auto const target = std::string_view { (char const*) storage.hotEnd(), static_cast<size_t>(rc) };
    auto const isStdFastPipe = false; // can never be, because it's an SSH network connection
    if (ptyInLog)
        ptyInLog()(
            "{} received: \"{}\"", "ssh", crispy::escape(target.data(), target.data() + target.size()));

    return std::tuple { target, isStdFastPipe };
}

void SshSession::wakeupReader()
{
    // TODO: implement
    //
    // wakeupReader() is invoked in TerminalSession's destructor (we could as well just call forceClose()
    // there, which would first wake-up a potential read()-call, if necessary.
    // And it's called in Terminal twice, where I am currently not sure that we even still need these calls.
}

void SshSession::handlePreAuthenticationPasswordInput(std::string_view buf, State next)
{
    sshLog()("({}) Handling pre-authentication input: \"{}\"", crispy::threadName(), crispy::escape(buf));
    if (buf.empty())
        return;

    if (buf == "\x7F" || buf == "\x08") // backspace, delete
    {
        if (!_injectedWrite.empty())
            _injectedWrite.pop_back();
    }
    else if (buf == "\r" || buf == "\n") // enter
    {
        setState(next);
        processState();
    }
    else
    {
        _injectedWrite += buf;
    }
}

int SshSession::write(std::string_view buf)
{
    // auto const _ = std::lock_guard { _mutex };
    if (isClosed())
    {
        errno = EPIPE;
        return -1;
    }

    if (_state == State::AuthenticatePasswordWaitForInput)
    {
        handlePreAuthenticationPasswordInput(buf, State::AuthenticatePassword);
        return static_cast<int>(buf.size()); // Make the caller believe that we have written all bytes.
    }
    else if (_state == State::AuthenticatePrivateKeyWaitForInput)
    {
        handlePreAuthenticationPasswordInput(buf, State::AuthenticatePrivateKey);
        return static_cast<int>(buf.size()); // Make the caller believe that we have written all bytes.
    }
    else if (_state != State::Operational)
    {
        sshLog()("Ignoring write() call in state: {}", _state);
        return static_cast<int>(buf.size()); // Make the caller believe that we have written all bytes.
    }

    waitForSocket();
    auto const rv = libssh2_channel_write(_p->sshChannel, buf.data(), buf.size());

    if (rv == LIBSSH2_ERROR_EAGAIN)
    {
        _p->wantsWaitForSocket = true;
        errno = EAGAIN;
        return -1;
    }

    if (rv < 0)
    {
        logError("Failed to write to SSH channel. {}", libssl2ErrorString(rv));
        errno = EIO;
        return -1;
    }

    if (ptyOutLog)
    {
        if (rv >= 0)
            ptyOutLog()("Sending bytes: \"{}\"", crispy::escape(buf.data(), buf.data() + rv));

        if (0 <= rv && static_cast<size_t>(rv) < buf.size())
            ptyOutLog()("Partial write. {} bytes written and {} bytes left.",
                        rv,
                        buf.size() - static_cast<size_t>(rv));
    }

    return static_cast<int>(rv);
}

PageSize SshSession::pageSize() const noexcept
{
    return _pageSize;
}

void SshSession::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    // auto const _ = std::lock_guard { _mutex };

    _pageSize = cells;
    _pixels = pixels;

    sshLog()("({}) Resizing PTY to {}x{}.", crispy::threadName(), cells.columns, cells.lines);

    if (isOperational())
    {
        setState(State::ResizeScreen);
        processState();
    }
}

bool SshSession::isOperational() const noexcept
{
    // clang-format off
    switch (_state)
    {
        case State::Operational:
            return true;
        default:
            return false;
    }
    // clang-format on
}

std::optional<SshSession::ExitStatus> SshSession::exitStatus() const
{
    auto exitcode = libssh2_channel_get_exit_status(_p->sshChannel);

    char* exitSignalStr = nullptr;
    char* errorMessage = nullptr;
    char* languageTag = nullptr;

    auto const rv = libssh2_channel_get_exit_signal(
        _p->sshChannel, &exitSignalStr, nullptr, &errorMessage, nullptr, &languageTag, nullptr);
    if (rv)
    {
        logError("Failed to get exit signal. {}", libssl2ErrorString(rv));
        return std::nullopt;
    }

    if (exitSignalStr)
        return SignalExit { exitSignalStr, errorMessage ? errorMessage : "", languageTag ? languageTag : "" };

    return NormalExit { exitcode };
}

void SshSession::injectRead(std::string_view buf)
{
    auto const _ = std::lock_guard { _injectMutex };
    _injectedRead += buf;
    _injectCV.notify_all();
}

void SshSession::logInfo(std::string_view message) const
{
    sshLog()("{}", message);
    const_cast<SshSession*>(this)->injectRead(fmt::format("\U0001F511 \033[1;33m{}\033[m\r\n", message));
}

void SshSession::logError(std::string_view message) const
{
    errorLog()("{}", message);
    const_cast<SshSession*>(this)->injectRead(fmt::format("\U0001F511 \033[1;31m{}\033[m\r\n", message));
}

bool SshSession::connect(std::string_view host, int port)
{
#if defined(_WIN32)
    WSADATA wsaData {};
    if (auto const wsaStartupCode = WSAStartup(MAKEWORD(2, 2), &wsaData); wsaStartupCode != 0)
    {
        logError("WSAStartup failed with error: %d", wsaStartupCode);
        return false;
    }
#endif

    try
    {
        auto hints = addrinfo {};
        hints.ai_flags = AI_ADDRCONFIG;
        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* addrList = nullptr;
        if (auto const rc = getaddrinfo(host.data(), nullptr, &hints, &addrList); rc != 0)
        {
            logError("Failed to resolve host \"{}\". {}", host, gai_strerror(rc));
            return false;
        }
        auto const _ = crispy::finally([&]() { freeaddrinfo(addrList); });
        for (addrinfo* addrEntry = addrList; addrEntry != nullptr; addrEntry = addrEntry->ai_next)
        {
            char addrStr[100] = {};
            switch (addrEntry->ai_family)
            {
                case AF_INET:
                    ((struct sockaddr_in*) addrEntry->ai_addr)->sin_port = htons(port);
                    inet_ntop(addrEntry->ai_family,
                              &((struct sockaddr_in*) addrEntry->ai_addr)->sin_addr,
                              addrStr,
                              sizeof(addrStr));
                    break;
                case AF_INET6:
                    ((struct sockaddr_in6*) addrEntry->ai_addr)->sin6_port = htons(port);
                    inet_ntop(addrEntry->ai_family,
                              &((struct sockaddr_in6*) addrEntry->ai_addr)->sin6_addr,
                              addrStr,
                              sizeof(addrStr));
                    break;
                default:
                    // should never happen
                    logInfo("Unknown address family: {}", addrEntry->ai_family);
                    break;
            }

            _p->sshSocket = socket_handle::from_native(
                socket(addrEntry->ai_family, addrEntry->ai_socktype, addrEntry->ai_protocol));

            if (::connect(_p->sshSocket, addrEntry->ai_addr, addrEntry->ai_addrlen) == 0)
            {
                auto const addrAndPort =
                    port == 22 ? std::string(addrStr) : fmt::format("{}:{}", addrStr, port);
                if (host != addrStr)
                    logInfo("Connected to {} ({})", host, addrAndPort);
                else
                    logInfo("Connected to {}", addrAndPort);
                return true;
            }

            logError("Failed to connect to {}:{} ({})", addrStr, port, strerror(errno));
            addrEntry = addrEntry->ai_next;
        }
    }
    catch (std::system_error const& e)
    {
        logError("Failed to create socket. {}", e.what());
        return false;
    }

    logError("Failed to connect to {}:{}", host, port);
    _p->sshSocket.close(); // Explicitly close socket, to indicate that we're not connected
    return false;
}

bool SshSession::verifyHostKey()
{
    if (_config.knownHostsFile.empty())
    {
        logInfo("Skipping host key verification, because no known_hosts file was specified.");
        return true;
    }

    LIBSSH2_KNOWNHOSTS* knownHosts = libssh2_knownhost_init(_p->sshSession);
    if (!knownHosts)
    {
        logError("Failed to initialize known_hosts file.");
        return false;
    }

    auto const _ = crispy::finally([&]() { libssh2_knownhost_free(knownHosts); });

    int rc = libssh2_knownhost_readfile(
        knownHosts, _config.knownHostsFile.string().c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    if (rc < 0)
    {
        auto const filePath = _config.knownHostsFile.string();
        logError("Failed to read known_hosts file \"{}\". {}", filePath, libssl2ErrorString(rc));
        return false;
    }

    int hostkeyType = 0;
    size_t hostkeyLength = 0;
    char const* hostkeyRaw = libssh2_session_hostkey(_p->sshSession, &hostkeyLength, &hostkeyType);
    int knownhostType = LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    switch (hostkeyType)
    {
#if defined(LIBSSH2_HOSTKEY_TYPE_RSA)
        case LIBSSH2_HOSTKEY_TYPE_RSA: knownhostType = LIBSSH2_KNOWNHOST_KEY_SSHRSA; break;
#endif
#if defined(LIBSSH2_HOSTKEY_TYPE_DSS)
        case LIBSSH2_HOSTKEY_TYPE_DSS: knownhostType = LIBSSH2_KNOWNHOST_KEY_SSHDSS; break;
#endif
#if defined(LIBSSH2_HOSTKEY_TYPE_ECDSA_256)
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: knownhostType = LIBSSH2_KNOWNHOST_KEY_ECDSA_256; break;
#endif
#if defined(LIBSSH2_HOSTKEY_TYPE_ECDSA_384)
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: knownhostType = LIBSSH2_KNOWNHOST_KEY_ECDSA_384; break;
#endif
#if defined(LIBSSH2_HOSTKEY_TYPE_ECDSA_521)
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: knownhostType = LIBSSH2_KNOWNHOST_KEY_ECDSA_521; break;
#endif
#if defined(LIBSSH2_HOSTKEY_TYPE_ED25519)
        case LIBSSH2_HOSTKEY_TYPE_ED25519: knownhostType = LIBSSH2_KNOWNHOST_KEY_ED25519; break;
#endif
        default: logError("Unknown host key type: {}", hostkeyType); return false;
    }

    libssh2_knownhost* knownHost = nullptr;
    rc = libssh2_knownhost_checkp(knownHosts,
                                  _config.hostname.c_str(),
                                  0, // _port,
                                  hostkeyRaw,
                                  hostkeyLength,
                                  LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhostType,
                                  &knownHost);

    switch (rc)
    {
        case LIBSSH2_KNOWNHOST_CHECK_MATCH:
            logInfo("Host key verification succeeded ({}).", knownHost->key);
            return true;
        case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
            logError("Host key verification failed. Host key mismatch.");
            return false;
        case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: {
            // TODO: Ask user whether to add host key to known_hosts file
            auto const comment =
                fmt::format("{}@{}:{} (added by Contour)", _config.username, _config.hostname, _config.port);
            auto const typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhostType;
            libssh2_knownhost_addc(knownHosts,
                                   _config.hostname.c_str(),
                                   nullptr /* salt */,
                                   hostkeyRaw,
                                   hostkeyLength,
                                   comment.data(),
                                   comment.size(),
                                   typeMask,
                                   nullptr /* store */);
            rc = libssh2_knownhost_writefile(
                knownHosts, _config.knownHostsFile.string().c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
            if (rc != LIBSSH2_ERROR_NONE)
            {
                logErrorWithDetails(rc, "Failed to write known_hosts file");
                return false;
            }
            return true;
        }
        case LIBSSH2_KNOWNHOST_CHECK_FAILURE: {
            logErrorWithDetails(rc, "Host key verification failed");
            return false;
        }
        default: {
            logErrorWithDetails(rc, "Unhandled error code in host key verification");
            return false;
        }
    }
}

void SshSession::logErrorWithDetails(int libssl2ErrorCode, std::string_view message) const
{
    char* errorMessageBuffer = nullptr;
    int errorMessageLength = 0;
    libssh2_session_last_error(_p->sshSession, &errorMessageBuffer, &errorMessageLength, 0);
    auto libssl2Message = std::string_view { errorMessageBuffer, static_cast<size_t>(errorMessageLength) };

    logError("{}: {}", message, libssl2ErrorString(libssl2ErrorCode));
    logError("Details: {}", libssl2Message);
}

int SshSession::waitForSocket(std::optional<std::chrono::milliseconds> timeout)
{
    if (!_p->wantsWaitForSocket)
        return 0;

    _p->wantsWaitForSocket = false;

#if defined(SSH_SESSION_NB_IO)
    fd_set fd;
    fd_set* writefd = nullptr;
    fd_set* readfd = nullptr;

    auto tv = timeval {};
    if (timeout)
    {
        tv.tv_sec = timeout->count() / 1000;
        tv.tv_usec = (timeout->count() % 1000) * 1000;
    }

    // TODO: also watch for break signal, so we can abort waiting for socket

    FD_ZERO(&fd);
    FD_SET(_p->sshSocket, &fd);

    assert(_p->sshSession);

    auto const dir =
        libssh2_session_block_directions(_p->sshSession); // now make sure we wait in the correct direction

    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
    {
        readfd = &fd;
        fmt::print("({}) SshSession: waiting for socket to become readable\n", crispy::threadName());
    }

    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
    {
        writefd = &fd;
        fmt::print("({}) SshSession: waiting for socket to become readable\n", crispy::threadName());
    }

    auto const rc = ::select((int) (_p->sshSocket + 1), readfd, writefd, nullptr, timeout ? &tv : nullptr);
    fmt::print("({}) SshSession: select() returned {}{}{}\n",
               crispy::threadName(),
               rc,
               readfd && FD_ISSET(_p->sshSocket, readfd) ? " [readable]" : "",
               writefd && FD_ISSET(_p->sshSocket, writefd) ? " [writable]" : "");
    return rc;
#else
    crispy::ignore_unused(timeout);
    return 0;
#endif
}

void SshSession::authenticateWithPrivateKey()
{
    auto const password = _injectedWrite;
    auto const rc = libssh2_userauth_publickey_fromfile_ex(
        _p->sshSession,
        _config.username.data(),
        _config.username.size(),
        _config.publicKeyFile.empty() ? nullptr : _config.publicKeyFile.string().data(),
        _config.privateKeyFile.string().data(),
        password.data());

    if (rc == LIBSSH2_ERROR_EAGAIN)
    {
        _p->wantsWaitForSocket = true;
        return;
    }

    injectRead("\r\n");
    _injectedWrite.clear();

    if (rc != LIBSSH2_ERROR_NONE)
    {
        // Only log error if we havee tried to authenticate with password,
        // as the first attempt is always with an empty password.
        if (_walkIndex != 0)
            logError("Private key based authentication failed. {}", libssl2ErrorString(rc));

        if (_walkIndex <= MaxPasswordTries)
        {
            setState(State::AuthenticatePrivateKeyRequest);
            ++_walkIndex;
            return;
        }
        setState(State::AuthenticatePasswordStart);
        _walkIndex = 0;
        return;
    }

    logInfo("Successfully authenticated with private key.");

    setState(State::OpenChannel);
}

void SshSession::authenticateWithPassword()
{
    auto const password = std::move(_injectedWrite);
    _injectedWrite = {};

    int const rc = libssh2_userauth_password_ex(_p->sshSession,
                                                _config.username.data(),
                                                _config.username.size(),
                                                password.data(),
                                                password.size(),
                                                nullptr);

    if (rc == LIBSSH2_ERROR_EAGAIN)
    {
        _p->wantsWaitForSocket = true;
        return;
    }

    injectRead("\r\n");

    if (rc != LIBSSH2_ERROR_NONE)
    {
        logError("Authentication failed. {}", libssl2ErrorString(rc));
        ++_walkIndex;
        if (_walkIndex < MaxPasswordTries)
        {
            setState(State::AuthenticatePasswordStart);
            return;
        }
        setState(State::Failure);
        return;
    }

    logInfo("Successfully authenticated with password.");

    setState(State::OpenChannel);
}

bool SshSession::authenticateWithAgent()
{
    if (!_p->sshAgent)
    {
        _p->sshAgent = libssh2_agent_init(_p->sshSession);
        if (!_p->sshAgent)
        {
            logError("Failed to initialize SSH agent.");
            return false;
        }

        int rc = libssh2_agent_connect(_p->sshAgent);
        if (rc != LIBSSH2_ERROR_NONE)
        {
            logError("Failed to connect to SSH agent. {}", libssl2ErrorString(rc));
            return false;
        }

        rc = libssh2_agent_list_identities(_p->sshAgent);
        if (rc != LIBSSH2_ERROR_NONE)
        {
            logError("Failed to list SSH identities. {}", libssl2ErrorString(rc));
            return false;
        }

        _walkIndex = 0;
    }

    libssh2_agent_publickey* identity = nullptr;
    libssh2_agent_publickey* prevIdentity = nullptr;
    int rc = 0;
    int i = 0;
    while ((rc = libssh2_agent_get_identity(_p->sshAgent, &identity, prevIdentity)) == 0)
    {
        prevIdentity = identity;
        if (i < _walkIndex)
        {
            ++i;
            continue;
        }

        rc = libssh2_agent_userauth(_p->sshAgent, _config.username.data(), identity);
        if (rc == LIBSSH2_ERROR_EAGAIN)
        {
            _p->wantsWaitForSocket = true;
            _walkIndex = i;
            return false;
        }
        if (rc == LIBSSH2_ERROR_NONE)
        {
            logInfo("Successfully authenticated with SSH agent with identity: {}", identity->comment);
            setState(State::OpenChannel);
            return true;
        }
        logInfo("Could not authenticate with SSH agent with identity: {}", identity->comment);
    }

    logError("Failed to authenticate with SSH agent. No more identities available.");
    if (!_config.privateKeyFile.empty())
        setState(State::AuthenticatePrivateKeyStart);
    else
    {
        setState(State::AuthenticatePasswordStart);
        _walkIndex = 0;
    }
    return false;
}

} // namespace vtpty
