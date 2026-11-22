#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace synclip {

inline constexpr std::uint16_t default_port = 45873;
inline constexpr std::size_t max_text_bytes = 1'048'576;

using Key = std::array<std::uint8_t, 32>;

Key generate_key();
std::string format_key(const Key& key);
Key parse_key(std::string_view hex);
bool valid_device_id(std::string_view id);
Key enroll_device(std::string_view host, std::uint16_t port, std::string_view device_id,
                  const Key& invitation_key);

enum class ServerStatus { listening, connected };

class Outbox {
public:
    // Keep only the most recent unsent copy. An empty value cancels it.
    void submit(std::string text);

private:
    friend struct OutboxAccess;
    std::mutex mutex_;
    std::shared_ptr<const std::string> latest_;
};

class Hub;

// Callbacks execute on background connection threads and must not block.
void run_server(std::string_view bind_address, std::uint16_t port, const std::atomic_bool& stop,
                const std::function<void(ServerStatus)>& status,
                const std::function<void(std::string)>& on_text, Hub& hub);

class Sender {
public:
    // The receive callback executes on the connection thread.
    Sender(std::string host, std::uint16_t port, Key key, std::function<void(std::string)> on_text = {},
           std::string device_id = {});
    ~Sender();

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    // Keeps only the newest pending text. An empty value cancels an unsent update.
    void submit(std::string text);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace synclip
