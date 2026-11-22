#include "hub_internal.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>

namespace synclip {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view legacy_id = "legacy";
constexpr std::string_view file_header = "SynClip devices 1";
constexpr std::string_view disable_legacy = "legacy disabled";

struct Record {
    Key key;
    std::string name;
    std::atomic_bool* stop = nullptr;
    std::shared_ptr<const std::string> pending;
};

using Records = std::map<std::string, Record, std::less<>>;

struct Invitation {
    std::string id;
    std::string name;
    Key key;
    Clock::time_point expires;
};

bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= 128 &&
           std::none_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 32 || ch == 127; });
}

std::string encode_host(std::string_view host) {
    if (host.empty() || host.size() > 45 ||
        host.find_first_not_of("0123456789abcdefABCDEF.:") != std::string_view::npos ||
        host == "0.0.0.0" || host == "::") {
        throw std::invalid_argument("Choose a reachable IPv4 or IPv6 address");
    }
    std::string encoded;
    for (const char ch : host) {
        if (ch == ':') {
            encoded += "%3A";
        } else {
            encoded.push_back(ch);
        }
    }
    return encoded;
}

void replace_file(const std::filesystem::path& source, const std::filesystem::path& dest) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Could not save paired devices");
    }
#else
    std::filesystem::rename(source, dest);
#endif
}

} // namespace

struct Hub::State {
    explicit State(std::filesystem::path file) : path(std::move(file)) {}

    void load(const std::optional<Key>& legacy_key) {
        if (std::filesystem::exists(path)) {
            if (std::filesystem::file_size(path) > 8192) {
                throw std::runtime_error("Paired devices file is too large");
            }
            std::ifstream input(path, std::ios::binary);
            std::string line;
            if (!std::getline(input, line) || line != file_header) {
                throw std::runtime_error("Invalid paired devices file");
            }
            while (std::getline(input, line)) {
                if (line == disable_legacy && !legacy_disabled) {
                    legacy_disabled = true;
                    continue;
                }
                const auto first = line.find('\t');
                const auto second = line.find('\t', first == std::string::npos ? first : first + 1);
                if (first == std::string::npos || second == std::string::npos ||
                        !valid_device_id(std::string_view(line).substr(0, first)) ||
                        !valid_name(std::string_view(line).substr(second + 1)) || records.size() >= max_paired_devices) {
                    throw std::runtime_error("Invalid paired device entry");
                }
                auto key = parse_key(std::string_view(line).substr(first + 1, second - first - 1));
                if (!records.emplace(line.substr(0, first), Record{key, line.substr(second + 1)}).second) {
                    throw std::runtime_error("Duplicate paired device entry");
                }
            }
            if (!input.eof()) {
                throw std::runtime_error("Could not read paired devices");
            }
        }
        if (legacy_key && !legacy_disabled) {
            records.emplace(legacy_id, Record{*legacy_key, u8"旧版手机"});
        }
    }

    void save(const Records& next, bool suppress_legacy) const {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        auto temp = path;
        temp += "." + format_key(generate_key()).substr(0, 16) + ".tmp";
        try {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output << file_header << '\n';
            if (suppress_legacy) {
                output << disable_legacy << '\n';
            }
            for (const auto& [id, record] : next) {
                if (id != legacy_id) {
                    output << id << '\t' << format_key(record.key) << '\t' << record.name << '\n';
                }
            }
            output.close();
            replace_file(temp, path);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            throw;
        }
    }

    bool has_invite(std::string_view id) const {
        return invite && invite->id == id && Clock::now() < invite->expires;
    }

    void distribute(std::shared_ptr<const std::string> text, std::string_view sender = {}) {
        for (auto& [id, record] : records) {
            record.pending = id == sender ? nullptr : text;
        }
    }

    std::filesystem::path path;
    std::mutex mutex;
    Records records;
    std::optional<Invitation> invite;
    bool legacy_disabled = false;
};

Hub::Hub(std::filesystem::path file, std::optional<Key> legacy_key)
    : state_(std::make_unique<State>(std::move(file))) {
    state_->load(legacy_key);
}

Hub::~Hub() = default;

PairingOffer Hub::begin_pairing(std::string_view host, std::uint16_t port, std::string_view name) {
    if (!valid_name(name) || port == 0) {
        throw std::invalid_argument("Enter a phone name of up to 128 UTF-8 bytes and a valid port");
    }
    const auto address = encode_host(host);
    std::lock_guard lock(state_->mutex);
    if (state_->records.size() >= max_paired_devices) {
        throw std::runtime_error("The maximum of 16 paired phones has been reached");
    }
    auto id = format_key(generate_key()).substr(0, 32);
    if (state_->records.count(id)) {
        throw std::runtime_error("Could not allocate a pairing identifier");
    }
    Invitation invite{id, std::string(name), generate_key(), Clock::now() + std::chrono::minutes(3)};
    auto uri = "synclip://pair?v=3&host=" + address + "&port=" + std::to_string(port) +
               "&id=" + id + "&key=" + format_key(invite.key);
    const auto expires = invite.expires;
    state_->invite = std::move(invite);
    return {std::move(id), std::move(uri), expires};
}

bool Hub::pairing_active(std::string_view id) const {
    std::lock_guard lock(state_->mutex);
    return state_->has_invite(id);
}

void Hub::cancel_pairing() {
    std::lock_guard lock(state_->mutex);
    state_->invite.reset();
}

std::vector<Device> Hub::devices() const {
    std::lock_guard lock(state_->mutex);
    std::vector<Device> devices;
    devices.reserve(state_->records.size());
    for (const auto& [id, record] : state_->records) {
        devices.push_back({id, record.name, record.stop && !*record.stop});
    }
    return devices;
}

bool Hub::revoke(std::string_view id) {
    std::lock_guard lock(state_->mutex);
    const auto entry = state_->records.find(id);
    if (entry == state_->records.end()) {
        return false;
    }
    auto next = state_->records;
    next.erase(entry->first);
    const bool suppress = state_->legacy_disabled || id == legacy_id;
    state_->save(next, suppress);
    if (entry->second.stop) {
        *entry->second.stop = true;
    }
    state_->records = std::move(next);
    state_->legacy_disabled = suppress;
    return true;
}

void Hub::submit(std::string text) {
    std::shared_ptr<const std::string> pending;
    if (!text.empty() && text.size() <= max_text_bytes) {
        pending = std::make_shared<const std::string>(std::move(text));
    }
    std::lock_guard lock(state_->mutex);
    state_->distribute(std::move(pending));
}

std::optional<PeerIdentity> HubAccess::lookup(Hub& hub, std::string_view identity) {
    auto& state = *hub.state_;
    std::lock_guard lock(state.mutex);
    if (identity.substr(0, invite_identity.size()) == invite_identity) {
        const auto id = identity.substr(invite_identity.size());
        if (state.has_invite(id)) {
            return PeerIdentity{std::string(id), state.invite->key, true};
        }
        return std::nullopt;
    }
    const auto id = identity == legacy_identity ? legacy_id : identity.substr(0, device_identity.size()) == device_identity
                    ? identity.substr(device_identity.size()) : std::string_view{};
    if (identity != legacy_identity && !valid_device_id(id)) {
        return std::nullopt;
    }
    const auto found = state.records.find(id);
    if (found == state.records.end()) {
        return std::nullopt;
    }
    return PeerIdentity{found->first, found->second.key, false};
}

bool HubAccess::enroll(Hub& hub, const PeerIdentity& auth, const Key& device_key) {
    auto& state = *hub.state_;
    std::lock_guard lock(state.mutex);
    if (!auth.invitation || !state.has_invite(auth.id) || auth.key != state.invite->key ||
            state.records.size() >= max_paired_devices || device_key == auth.key || device_key == Key{}) {
        return false;
    }
    auto next = state.records;
    if (!next.emplace(auth.id, Record{device_key, state.invite->name}).second) {
        return false;
    }
    state.save(next, state.legacy_disabled);
    state.records = std::move(next);
    state.invite.reset();
    return true;
}

bool HubAccess::attach(Hub& hub, const PeerIdentity& auth, std::atomic_bool& stop) {
    auto& state = *hub.state_;
    std::lock_guard lock(state.mutex);
    const auto found = state.records.find(auth.id);
    if (auth.invitation || found == state.records.end() || auth.key != found->second.key || stop) {
        return false;
    }
    if (found->second.stop) {
        *found->second.stop = true;
    }
    found->second.stop = &stop;
    return true;
}

void HubAccess::detach(Hub& hub, std::string_view id, const std::atomic_bool& stop) {
    auto& state = *hub.state_;
    std::lock_guard lock(state.mutex);
    const auto found = state.records.find(id);
    if (found != state.records.end() && found->second.stop == &stop) {
        found->second.stop = nullptr;
    }
}

std::shared_ptr<const std::string> HubAccess::peek(Hub& hub, std::string_view id) {
    auto& state = *hub.state_;
    std::lock_guard lock(state.mutex);
    const auto found = state.records.find(id);
    return found == state.records.end() ? nullptr : found->second.pending;
}

void HubAccess::acknowledge(Hub& hub, std::string_view id, const std::shared_ptr<const std::string>& text) {
    auto& state = *hub.state_;
    std::lock_guard lock(state.mutex);
    const auto found = state.records.find(id);
    if (found != state.records.end() && found->second.pending == text) {
        found->second.pending.reset();
    }
}

bool HubAccess::publish(Hub& hub, std::string_view sender, const std::atomic_bool& stop, const std::string& text) {
    auto& state = *hub.state_;
    std::lock_guard lock(state.mutex);
    const auto found = state.records.find(sender);
    if (found == state.records.end() || found->second.stop != &stop || stop) {
        return false;
    }
    state.distribute(std::make_shared<const std::string>(text), sender);
    return true;
}

ServerStatus HubAccess::status(Hub& hub) {
    auto& state = *hub.state_;
    std::lock_guard lock(state.mutex);
    const bool online = std::any_of(state.records.begin(), state.records.end(), [](const auto& entry) {
        return entry.second.stop && !*entry.second.stop;
    });
    return online ? ServerStatus::connected : ServerStatus::listening;
}

} // namespace synclip
