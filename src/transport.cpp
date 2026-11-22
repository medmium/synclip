#include "synclip/transport.hpp"
#include "hub_internal.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <psa/crypto.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace synclip {

struct OutboxAccess {
    static std::shared_ptr<const std::string> peek(Outbox& outbox) {
        std::lock_guard guard(outbox.mutex_);
        return outbox.latest_;
    }

    static void acknowledge(Outbox& outbox, const std::shared_ptr<const std::string>& text) {
        std::lock_guard guard(outbox.mutex_);
        if (outbox.latest_ == text) {
            outbox.latest_.reset();
        }
    }
};

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr auto poll_interval = 200ms;
constexpr auto connect_timeout = 4s;
constexpr auto frame_timeout = 6s;
constexpr auto keepalive_interval = 15s;
constexpr auto idle_timeout = 45s;
constexpr std::array<unsigned char, 4> frame_magic{'S', 'C', 'L', '2'};

enum class FrameType : unsigned char { text = 1, available, pull, ack, ping, pair };

struct Frame {
    FrameType type;
    std::string text;
};

class LinkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#ifdef _WIN32
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;

int socket_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
bool interrupted(int error) { return error == WSAEINTR; }
bool connect_pending(int error) { return would_block(error) || error == WSAEINPROGRESS; }

class Network {
public:
    Network() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw LinkError("Could not initialize Winsock");
        }
    }
    ~Network() { WSACleanup(); }
};
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr NativeSocket invalid_socket = -1;

int socket_error() { return errno; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool interrupted(int error) { return error == EINTR; }
bool connect_pending(int error) { return error == EINPROGRESS || interrupted(error); }

class Network {};
#endif

class Socket {
public:
    explicit Socket(NativeSocket socket) : socket_(socket) {}
    ~Socket() {
        if (!*this) {
            return;
        }
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : socket_(std::exchange(other.socket_, invalid_socket)) {}

    explicit operator bool() const { return socket_ != invalid_socket; }
    NativeSocket get() const { return socket_; }

    void nonblocking() const {
#ifdef _WIN32
        u_long enabled = 1;
        const int result = ioctlsocket(socket_, FIONBIO, &enabled);
#else
        const int flags = fcntl(socket_, F_GETFL);
        const int result = flags == -1 ? -1 : fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
#endif
        if (result != 0) {
            throw LinkError("Could not configure TCP socket");
        }
    }

    void low_latency() const {
        const int enabled = 1;
        setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    }

private:
    NativeSocket socket_;
};

struct Address {
    sockaddr_storage data{};
    SocketLength size{};

    const sockaddr* get() const { return reinterpret_cast<const sockaddr*>(&data); }
};

Address parse_address(std::string_view host, std::uint16_t port) {
    if (host.empty() || host.find('\0') != std::string_view::npos || port == 0) {
        throw std::invalid_argument("An IP address and nonzero port are required");
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* result = nullptr;
    const auto port_text = std::to_string(port);
    if (getaddrinfo(std::string(host).c_str(), port_text.c_str(), &hints, &result) != 0) {
        throw std::invalid_argument("Expected a numeric IPv4 or IPv6 address");
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(result, freeaddrinfo);
    Address address;
    address.size = static_cast<SocketLength>(result->ai_addrlen);
    std::copy_n(reinterpret_cast<const unsigned char*>(result->ai_addr), address.size,
                reinterpret_cast<unsigned char*>(&address.data));
    return address;
}

Socket tcp_socket(const Address& address) {
    Socket socket(::socket(address.data.ss_family, SOCK_STREAM, IPPROTO_TCP));
    if (!socket) {
        throw LinkError("Could not open TCP socket");
    }
    socket.nonblocking();
    return socket;
}

bool poll_socket(const Socket& socket, bool writing, std::chrono::milliseconds timeout) {
    const short events = writing ? POLLOUT : POLLIN;
#ifdef _WIN32
    WSAPOLLFD state{socket.get(), events, 0};
    const int result = WSAPoll(&state, 1, static_cast<int>(timeout.count()));
#else
    pollfd state{socket.get(), events, 0};
    const int result = poll(&state, 1, static_cast<int>(timeout.count()));
#endif
    if (result < 0 && !interrupted(socket_error())) {
        throw LinkError("Could not poll TCP socket");
    }
    return result > 0;
}

void check_deadline(const std::atomic_bool& stop, Clock::time_point deadline) {
    if (stop || Clock::now() >= deadline) {
        throw LinkError("Connection stopped or timed out");
    }
}

void wait_socket(const Socket& socket, bool writing, const std::atomic_bool& stop,
                 Clock::time_point deadline) {
    for (;;) {
        check_deadline(stop, deadline);
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (poll_socket(socket, writing, std::clamp(remaining, 0ms, poll_interval))) {
            return;
        }
    }
}

Socket connect_to(const Address& address, const std::atomic_bool& stop) {
    auto socket = tcp_socket(address);
    if (connect(socket.get(), address.get(), address.size) != 0) {
        if (!connect_pending(socket_error())) {
            throw LinkError("Could not connect to Windows");
        }
        wait_socket(socket, true, stop, Clock::now() + connect_timeout);
        int error = 0;
        SocketLength size = sizeof(error);
        if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0
                || error != 0) {
            throw LinkError("Could not connect to Windows");
        }
    }
    socket.low_latency();
    return socket;
}

int send_socket(void* state, const unsigned char* data, std::size_t size) {
    const auto socket = static_cast<Socket*>(state)->get();
#ifdef _WIN32
    const auto result = send(socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
#else
    const auto result = send(socket, data, size, MSG_NOSIGNAL);
#endif
    if (result >= 0) {
        return static_cast<int>(result);
    }
    const int error = socket_error();
    return would_block(error) || interrupted(error) ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                    : MBEDTLS_ERR_NET_SEND_FAILED;
}

int receive_socket(void* state, unsigned char* data, std::size_t size) {
    const auto socket = static_cast<Socket*>(state)->get();
#ifdef _WIN32
    const auto result = recv(socket, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
#else
    const auto result = recv(socket, data, size, 0);
#endif
    if (result >= 0) {
        return static_cast<int>(result);
    }
    const int error = socket_error();
    return would_block(error) || interrupted(error) ? MBEDTLS_ERR_SSL_WANT_READ
                                                    : MBEDTLS_ERR_NET_RECV_FAILED;
}

void initialize_crypto() {
    static std::once_flag flag;
    std::call_once(flag, [] {
#ifdef _WIN32
        mbedtls_threading_set_alt(
            [](mbedtls_threading_mutex_t* mutex) { InitializeSRWLock(&mutex->lock); },
            [](mbedtls_threading_mutex_t*) {},
            [](mbedtls_threading_mutex_t* mutex) { AcquireSRWLockExclusive(&mutex->lock); return 0; },
            [](mbedtls_threading_mutex_t* mutex) { ReleaseSRWLockExclusive(&mutex->lock); return 0; });
#endif
        if (psa_crypto_init() != PSA_SUCCESS) {
            throw std::runtime_error("Could not initialize TLS cryptography");
        }
    });
}

void check_crypto(int result, const char* operation) {
    if (result != 0) {
        char message[160]{};
        mbedtls_strerror(result, message, sizeof(message));
        throw std::runtime_error(std::string(operation) + ": " + message);
    }
}

class Random {
public:
    Random() {
        initialize_crypto();
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&rng_);
        const int result = mbedtls_ctr_drbg_seed(&rng_, mbedtls_entropy_func, &entropy_, nullptr, 0);
        if (result != 0) {
            free();
            check_crypto(result, "Could not seed random generator");
        }
    }
    ~Random() { free(); }

    mbedtls_ctr_drbg_context* get() { return &rng_; }

private:
    void free() {
        mbedtls_ctr_drbg_free(&rng_);
        mbedtls_entropy_free(&entropy_);
    }
    mbedtls_entropy_context entropy_{};
    mbedtls_ctr_drbg_context rng_{};
};

struct Handshake {
    Hub& hub;
    std::optional<PeerIdentity> identity;
};

int select_psk(void* context, mbedtls_ssl_context* ssl, const unsigned char* identity, std::size_t size) {
    auto& handshake = *static_cast<Handshake*>(context);
    try {
        auto match = HubAccess::lookup(handshake.hub, {reinterpret_cast<const char*>(identity), size});
        if (!match) {
            return -1;
        }
        const int result = mbedtls_ssl_set_hs_psk(ssl, match->key.data(), match->key.size());
        if (result == 0) {
            handshake.identity = std::move(match);
        }
        return result;
    } catch (...) {
        return -1;
    }
}

class TlsConfig {
public:
    TlsConfig(const Key& key, std::string_view identity) : TlsConfig(MBEDTLS_SSL_IS_CLIENT) {
        check_crypto(mbedtls_ssl_conf_psk(&config_, key.data(), key.size(),
                                         reinterpret_cast<const unsigned char*>(identity.data()), identity.size()),
                     "Could not configure pairing key");
    }

    explicit TlsConfig(Handshake& handshake) : TlsConfig(MBEDTLS_SSL_IS_SERVER) {
        mbedtls_ssl_conf_psk_cb(&config_, select_psk, &handshake);
    }

    ~TlsConfig() { mbedtls_ssl_config_free(&config_); }

    const mbedtls_ssl_config* get() const { return &config_; }

private:
    explicit TlsConfig(int endpoint) : config_{} {
        mbedtls_ssl_config_init(&config_);
        try {
            check_crypto(mbedtls_ssl_config_defaults(&config_, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                     MBEDTLS_SSL_PRESET_DEFAULT),
                         "Could not configure TLS");
            mbedtls_ssl_conf_min_tls_version(&config_, MBEDTLS_SSL_VERSION_TLS1_3);
            mbedtls_ssl_conf_max_tls_version(&config_, MBEDTLS_SSL_VERSION_TLS1_3);
            mbedtls_ssl_conf_tls13_key_exchange_modes(&config_, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL);
            mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, random_.get());
            mbedtls_ssl_conf_session_tickets(&config_, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
            mbedtls_ssl_conf_new_session_tickets(&config_, 0);
        } catch (...) {
            mbedtls_ssl_config_free(&config_);
            throw;
        }
    }
    Random random_;
    mbedtls_ssl_config config_;
};

class Session {
public:
    Session(Socket& socket, const TlsConfig& config, const std::atomic_bool& stop)
        : socket_(socket), stop_(stop) {
        mbedtls_ssl_init(&ssl_);
        const int result = mbedtls_ssl_setup(&ssl_, config.get());
        if (result != 0) {
            mbedtls_ssl_free(&ssl_);
            check_crypto(result, "Could not initialize TLS session");
        }
        mbedtls_ssl_set_bio(&ssl_, &socket_, send_socket, receive_socket, nullptr);
    }
    ~Session() { mbedtls_ssl_free(&ssl_); }

    void handshake() {
        const auto deadline = Clock::now() + connect_timeout;
        for (;;) {
            check_deadline(stop_, deadline);
            const int result = mbedtls_ssl_handshake(&ssl_);
            if (result == 0) {
                return;
            }
            retry(result, deadline);
        }
    }

    bool readable() {
        return mbedtls_ssl_check_pending(&ssl_) != 0 || poll_socket(socket_, false, poll_interval);
    }

    void read(unsigned char* buffer, std::size_t size, Clock::time_point deadline) {
        while (size > 0) {
            check_deadline(stop_, deadline);
            const int count = mbedtls_ssl_read(&ssl_, buffer, size);
            if (count <= 0) {
                retry(count, deadline);
                continue;
            }
            buffer += count;
            size -= count;
        }
    }

    void write(const unsigned char* buffer, std::size_t size, Clock::time_point deadline) {
        while (size > 0) {
            check_deadline(stop_, deadline);
            const int count = mbedtls_ssl_write(&ssl_, buffer, size);
            if (count <= 0) {
                retry(count, deadline);
                continue;
            }
            buffer += count;
            size -= count;
        }
    }

private:
    void retry(int result, Clock::time_point deadline) {
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            throw LinkError("TLS session ended");
        }
        wait_socket(socket_, result == MBEDTLS_ERR_SSL_WANT_WRITE, stop_, deadline);
    }

    Socket& socket_;
    const std::atomic_bool& stop_;
    mbedtls_ssl_context ssl_{};
};

void send_frame(Session& session, FrameType type, std::string_view text = {}) {
    std::array<unsigned char, 9> header{};
    std::copy(frame_magic.begin(), frame_magic.end(), header.begin());
    header[4] = static_cast<unsigned char>(type);
    const auto size = static_cast<std::uint32_t>(text.size());
    header[5] = static_cast<unsigned char>(size >> 24);
    header[6] = static_cast<unsigned char>(size >> 16);
    header[7] = static_cast<unsigned char>(size >> 8);
    header[8] = static_cast<unsigned char>(size);
    const auto deadline = Clock::now() + frame_timeout;
    session.write(header.data(), header.size(), deadline);
    session.write(reinterpret_cast<const unsigned char*>(text.data()), text.size(), deadline);
}

Frame receive_frame(Session& session, Clock::time_point deadline) {
    std::array<unsigned char, 9> header{};
    session.read(header.data(), header.size(), deadline);
    if (!std::equal(frame_magic.begin(), frame_magic.end(), header.begin())) {
        throw LinkError("Unexpected protocol header");
    }
    const auto type = static_cast<FrameType>(header[4]);
    std::uint32_t size = 0;
    for (std::size_t i = 5; i < header.size(); ++i) {
        size = (size << 8) | header[i];
    }
    const bool has_payload = type == FrameType::text || type == FrameType::pair;
    if (type < FrameType::text || type > FrameType::pair || size > max_text_bytes ||
            has_payload != (size != 0) || (type == FrameType::pair && size != 64)) {
        throw LinkError("Invalid clipboard frame");
    }
    std::string text(size, '\0');
    session.read(reinterpret_cast<unsigned char*>(text.data()), text.size(), deadline);
    return {type, std::move(text)};
}

Frame request(Session& session, FrameType type, std::string_view text = {}) {
    send_frame(session, type, text);
    const auto deadline = Clock::now() + frame_timeout;
    auto reply = receive_frame(session, deadline);
    // An advertisement can already be in the TCP stream when Android starts a request.
    if (reply.type == FrameType::available) {
        reply = receive_frame(session, deadline);
    }
    return reply;
}

void require_ack(FrameType type) {
    if (type != FrameType::ack) {
        throw LinkError("Expected clipboard acknowledgement");
    }
}

void receive_clipboards(Session& session, const std::atomic_bool& stop, Hub& hub, std::string_view id,
                        std::mutex& dispatch,
                        const std::function<void(std::string)>& on_text) {
    auto deadline = Clock::now() + idle_timeout;
    bool advertised = false;
    while (!stop) {
        check_deadline(stop, deadline);
        if (!advertised && HubAccess::peek(hub, id)) {
            // Windows advertises first and sends payloads only in response to a pull.
            // This prevents simultaneous large writes from filling both TCP buffers.
            send_frame(session, FrameType::available);
            advertised = true;
        }
        if (!session.readable()) {
            continue;
        }
        auto frame = receive_frame(session, Clock::now() + frame_timeout);
        switch (frame.type) {
        case FrameType::text: {
            // The hub forwards a phone's copy to the other phones in Windows' receive order.
            std::lock_guard lock(dispatch);
            if (!HubAccess::publish(hub, id, stop, frame.text)) {
                throw LinkError("Phone is no longer paired");
            }
            advertised = false;
            on_text(std::move(frame.text));
            break;
        }
        case FrameType::ping:
            send_frame(session, FrameType::ack);
            break;
        case FrameType::pull: {
            advertised = false;
            const auto text = HubAccess::peek(hub, id);
            if (!text) {
                send_frame(session, FrameType::ack);
                break;
            }
            send_frame(session, FrameType::text, *text);
            require_ack(receive_frame(session, Clock::now() + frame_timeout).type);
            HubAccess::acknowledge(hub, id, text);
            break;
        }
        default:
            throw LinkError("Unexpected clipboard request");
        }
        if (frame.type == FrameType::text) {
            send_frame(session, FrameType::ack);
        }
        deadline = Clock::now() + idle_timeout;
    }
}

int digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    throw std::invalid_argument("The pairing key must contain 64 hexadecimal digits");
}

std::string clipboard_identity(std::string_view id) {
    if (id.empty()) {
        return std::string(legacy_identity);
    }
    if (!valid_device_id(id)) {
        throw std::invalid_argument("Invalid paired device identifier");
    }
    return std::string(device_identity) + std::string(id);
}

struct Worker {
    std::atomic_bool stop{false};
    std::atomic_bool finished{false};
    std::thread thread;
};

class Workers {
public:
    ~Workers() {
        for (const auto& worker : entries) {
            worker->stop = true;
        }
        for (const auto& worker : entries) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
    }

    void reap() {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& worker) {
            if (!worker->finished) {
                return false;
            }
            worker->thread.join();
            return true;
        }), entries.end());
    }

    std::vector<std::unique_ptr<Worker>> entries;
};

void run_peer(Socket& peer, Hub& hub, Worker& worker, std::mutex& dispatch,
              const std::function<void(ServerStatus)>& status,
              const std::function<void(std::string)>& on_text) {
    Handshake handshake{hub, {}};
    bool attached = false;
    try {
        peer.nonblocking();
        peer.low_latency();
        TlsConfig config(handshake);
        Session session(peer, config, worker.stop);
        session.handshake();
        if (handshake.identity) {
            const auto& auth = *handshake.identity;
            if (auth.invitation) {
                const auto frame = receive_frame(session, Clock::now() + frame_timeout);
                if (frame.type == FrameType::pair && HubAccess::enroll(hub, auth, parse_key(frame.text))) {
                    send_frame(session, FrameType::ack);
                }
            } else if (HubAccess::attach(hub, auth, worker.stop)) {
                attached = true;
                status(HubAccess::status(hub));
                receive_clipboards(session, worker.stop, hub, auth.id, dispatch, on_text);
            }
        }
    } catch (const std::exception&) {
        // A failed handshake, dropped peer or failed enrollment won't stop other phones.
    }
    if (attached) {
        HubAccess::detach(hub, handshake.identity->id, worker.stop);
        status(HubAccess::status(hub));
    }
}

} // namespace

void Outbox::submit(std::string text) {
    std::shared_ptr<const std::string> pending;
    if (!text.empty() && text.size() <= max_text_bytes) {
        pending = std::make_shared<const std::string>(std::move(text));
    }
    std::lock_guard guard(mutex_);
    latest_ = std::move(pending);
}

Key generate_key() {
    Random random;
    Key key{};
    check_crypto(mbedtls_ctr_drbg_random(random.get(), key.data(), key.size()), "Could not generate a pairing key");
    return key;
}

std::string format_key(const Key& key) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(key.size() * 2);
    for (const auto byte : key) {
        output.push_back(hex[byte >> 4]);
        output.push_back(hex[byte & 15]);
    }
    return output;
}

Key parse_key(std::string_view hex) {
    Key result{};
    if (hex.size() != result.size() * 2) {
        throw std::invalid_argument("The pairing key must contain 64 hexadecimal digits");
    }
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<std::uint8_t>(digit(hex[i * 2]) * 16 + digit(hex[i * 2 + 1]));
    }
    if (result == Key{}) {
        throw std::invalid_argument("The pairing key cannot be all zeros");
    }
    return result;
}

bool valid_device_id(std::string_view id) {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

Key enroll_device(std::string_view host, std::uint16_t port, std::string_view id, const Key& invitation_key) {
    if (!valid_device_id(id)) {
        throw std::invalid_argument("Invalid pairing identifier");
    }
    [[maybe_unused]] Network network;
    const auto address = parse_address(host, port);
    const auto permanent = generate_key();
    const std::atomic_bool stop{false};
    bool submitted = false;
    try {
        const TlsConfig config(invitation_key, std::string(invite_identity) + std::string(id));
        auto socket = connect_to(address, stop);
        Session session(socket, config, stop);
        session.handshake();
        submitted = true;
        require_ack(request(session, FrameType::pair, format_key(permanent)).type);
    } catch (const LinkError&) {
        if (!submitted) {
            throw;
        }
        // A missing acknowledgement can mean Windows saved the new key before the link closed.
        const TlsConfig config(permanent, clipboard_identity(id));
        auto socket = connect_to(address, stop);
        Session session(socket, config, stop);
        session.handshake();
        require_ack(request(session, FrameType::ping).type);
    }
    return permanent;
}

void run_server(std::string_view bind_address, std::uint16_t port, const std::atomic_bool& stop,
                const std::function<void(ServerStatus)>& status,
                const std::function<void(std::string)>& on_text, Hub& hub) {
    [[maybe_unused]] Network network;
    const auto address = parse_address(bind_address, port);
    auto listener = tcp_socket(address);
    if (bind_address == "::") {
        const int dual_stack = 0;
        if (setsockopt(listener.get(), IPPROTO_IPV6, IPV6_V6ONLY,
                       reinterpret_cast<const char*>(&dual_stack), sizeof(dual_stack)) != 0) {
            throw LinkError("Could not enable IPv4 and IPv6 on this address");
        }
    }
#ifdef _WIN32
    const int enabled = 1;
    if (setsockopt(listener.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) {
        throw LinkError("Could not configure exclusive listen socket");
    }
#endif
    if (bind(listener.get(), address.get(), address.size) != 0 || listen(listener.get(), 8) != 0) {
        throw LinkError("Could not listen on the configured IP and port");
    }
    initialize_crypto();
    std::mutex dispatch;
    Workers workers;
    status(ServerStatus::listening);
    while (!stop) {
        workers.reap();
        if (!poll_socket(listener, false, poll_interval)) {
            continue;
        }
        Socket peer(accept(listener.get(), nullptr, nullptr));
        if (!peer) {
            const int error = socket_error();
            if (interrupted(error) || would_block(error)) {
                continue;
            }
            throw LinkError("Could not accept TCP connection");
        }
        if (workers.entries.size() >= max_paired_devices + 8) {
            continue;
        }
        workers.entries.push_back(std::make_unique<Worker>());
        auto& worker = *workers.entries.back();
        worker.thread = std::thread([&, client = std::move(peer), task = &worker]() mutable {
            try {
                run_peer(client, hub, *task, dispatch, status, on_text);
            } catch (const std::exception&) {
                // A callback failure on this connection must not terminate the listener.
            }
            task->finished = true;
        });
    }
}

class Sender::Impl {
public:
    Impl(std::string_view host, std::uint16_t port, const Key& key,
         std::function<void(std::string)> on_text, std::string_view id)
        : address_(parse_address(host, port)), config_(key, clipboard_identity(id)),
          on_text_(std::move(on_text)),
          thread_([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    void submit(std::string value) { outgoing_.submit(std::move(value)); }

private:
    void exchange_clipboards(Session& session, std::chrono::milliseconds& retry_delay) {
        auto next_ping = Clock::now() + keepalive_interval;
        bool windows_has_text = false;
        while (!stop_) {
            if (const auto text = OutboxAccess::peek(outgoing_)) {
                require_ack(request(session, FrameType::text, *text).type);
                OutboxAccess::acknowledge(outgoing_, text);
                windows_has_text = false;
            } else if (windows_has_text) {
                auto reply = request(session, FrameType::pull);
                if (reply.type == FrameType::text) {
                    // A local copy during the pull stays on the phone and is sent next.
                    if (!OutboxAccess::peek(outgoing_) && on_text_ && !stop_) {
                        on_text_(std::move(reply.text));
                    }
                    send_frame(session, FrameType::ack);
                } else {
                    require_ack(reply.type);
                }
                windows_has_text = false;
            } else if (Clock::now() >= next_ping) {
                // A ping can race an advertisement. Keep that advertisement for the next pull.
                send_frame(session, FrameType::ping);
                const auto deadline = Clock::now() + frame_timeout;
                auto reply = receive_frame(session, deadline);
                if (reply.type == FrameType::available) {
                    windows_has_text = true;
                    reply = receive_frame(session, deadline);
                }
                require_ack(reply.type);
            } else {
                if (session.readable()) {
                    if (receive_frame(session, Clock::now() + frame_timeout).type != FrameType::available) {
                        throw LinkError("Unexpected clipboard notification");
                    }
                    windows_has_text = true;
                }
                continue;
            }
            retry_delay = 300ms;
            next_ping = Clock::now() + keepalive_interval;
        }
    }

    void run() {
        auto delay = 300ms;
        while (!stop_) {
            try {
                auto socket = connect_to(address_, stop_);
                Session session(socket, config_, stop_);
                session.handshake();
                exchange_clipboards(session, delay);
            } catch (const LinkError&) {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, delay, [&] { return stop_.load(); });
                delay = std::min(delay * 2, 5'000ms);
            }
        }
    }

    [[maybe_unused]] Network network_;
    Address address_;
    TlsConfig config_;
    std::mutex mutex_;
    std::condition_variable wake_;
    Outbox outgoing_;
    std::function<void(std::string)> on_text_;
    std::atomic_bool stop_{false};
    std::thread thread_;
};

Sender::Sender(std::string host, std::uint16_t port, Key key, std::function<void(std::string)> on_text, std::string id)
    : impl_(std::make_unique<Impl>(host, port, key, std::move(on_text), id)) {}

Sender::~Sender() = default;

void Sender::submit(std::string text) {
    impl_->submit(std::move(text));
}

} // namespace synclip
