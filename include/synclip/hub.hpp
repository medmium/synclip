#pragma once

#include "synclip/transport.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace synclip {

struct Device {
    std::string id;
    std::string name;
    bool connected;
};

struct PairingOffer {
    std::string id;
    std::string uri;
    std::chrono::steady_clock::time_point expires;
};

class Hub {
public:
    explicit Hub(std::filesystem::path devices_file, std::optional<Key> legacy_key = {});
    ~Hub();

    PairingOffer begin_pairing(std::string_view host, std::uint16_t port, std::string_view name);
    bool pairing_active(std::string_view id) const;
    void cancel_pairing();

    std::vector<Device> devices() const;
    bool revoke(std::string_view id);
    void submit(std::string text);

private:
    friend struct HubAccess;
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace synclip
