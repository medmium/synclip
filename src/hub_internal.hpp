#pragma once

#include "synclip/hub.hpp"

namespace synclip {

inline constexpr std::string_view legacy_identity = "synclip/2";
inline constexpr std::string_view device_identity = "synclip/3/";
inline constexpr std::string_view invite_identity = "synclip/pair/";
inline constexpr std::size_t max_paired_devices = 16;

struct PeerIdentity {
    std::string id;
    Key key;
    bool invitation;
};

struct HubAccess {
    static std::optional<PeerIdentity> lookup(Hub& hub, std::string_view identity);
    static bool enroll(Hub& hub, const PeerIdentity& invitation, const Key& device_key);
    static bool attach(Hub& hub, const PeerIdentity& identity, std::atomic_bool& stop);
    static void detach(Hub& hub, std::string_view id, const std::atomic_bool& stop);
    static std::shared_ptr<const std::string> peek(Hub& hub, std::string_view id);
    static void acknowledge(Hub& hub, std::string_view id, const std::shared_ptr<const std::string>& text);
    static bool publish(Hub& hub, std::string_view sender, const std::atomic_bool& stop, const std::string& text);
    static ServerStatus status(Hub& hub);
};

} // namespace synclip
