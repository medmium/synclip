#include "synclip/transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct Configuration {
    std::string host;
    std::string device;
    uint16_t port = synclip::default_port;
    synclip::Key key{};
};

std::string_view trim(std::string_view value) {
    constexpr std::string_view spaces = " \t\r";
    const auto first = value.find_first_not_of(spaces);
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(spaces) - first + 1);
}

uint16_t parse_port(std::string_view value) {
    if (value.empty()) {
        throw std::runtime_error("port is missing");
    }
    unsigned int port = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        port == 0 || port > 65535) {
        throw std::runtime_error("port must be an integer in 1–65535");
    }
    return static_cast<uint16_t>(port);
}

Configuration read_configuration() {
    std::array<char, 4097> buffer{};
    size_t size = 0;
    while (size < buffer.size()) {
        const auto count = read(STDIN_FILENO, buffer.data() + size, buffer.size() - size);
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(STDIN_FILENO);
            throw std::runtime_error("could not read the configuration");
        }
        size += static_cast<size_t>(count);
    }
    close(STDIN_FILENO);
    if (size == buffer.size()) {
        throw std::runtime_error("configuration exceeds 4096 bytes");
    }

    Configuration config;
    bool has_key = false;
    bool has_port = false;
    std::string_view contents(buffer.data(), size);
    if (contents.substr(0, 3) == "\xEF\xBB\xBF") {
        contents.remove_prefix(3);
    }
    while (!contents.empty()) {
        const size_t newline = contents.find('\n');
        const auto line = trim(contents.substr(0, newline));
        contents.remove_prefix(newline == std::string_view::npos ? contents.size() : newline + 1);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            throw std::runtime_error("configuration entries require name=value syntax");
        }
        const auto name = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (name == "host" && config.host.empty() && !value.empty()) {
            config.host.assign(value);
        } else if (name == "device" && config.device.empty() && synclip::valid_device_id(value)) {
            config.device.assign(value);
        } else if (name == "port" && !has_port) {
            config.port = parse_port(value);
            has_port = true;
        } else if (name == "key" && !has_key) {
            try {
                config.key = synclip::parse_key(value);
            } catch (const std::invalid_argument&) {
                throw std::runtime_error("key must contain 64 hexadecimal characters");
            }
            has_key = true;
        } else {
            throw std::runtime_error("empty, unknown or duplicate configuration entry");
        }
    }

    in_addr ipv4{};
    in6_addr ipv6{};
    if (config.host.find('\0') != std::string::npos ||
        (inet_pton(AF_INET, config.host.c_str(), &ipv4) != 1 &&
         inet_pton(AF_INET6, config.host.c_str(), &ipv6) != 1)) {
        throw std::runtime_error("host must be a numeric Windows IPv4 or IPv6 address");
    }
    if (!has_key) {
        throw std::runtime_error("configuration requires a key");
    }
    return config;
}

std::string utf8_text(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const size_t length = static_cast<size_t>(env->GetStringLength(value));
    if (length > synclip::max_text_bytes) {
        return {};
    }
    std::string text;
    text.reserve(std::min(length * 3, synclip::max_text_bytes));
    const jchar* units = env->GetStringChars(value, nullptr);
    if (units == nullptr) {
        return {};
    }

    // JNI's GetStringUTFChars produces modified UTF-8. The wire format uses UTF-8.
    bool valid = true;
    for (size_t i = 0; i < length; ++i) {
        uint32_t code = units[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < length &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (units[++i] - 0xDC00);
        }
        if (code == 0 || (code >= 0xD800 && code <= 0xDFFF)) {
            valid = false;
            break;
        }
        if (code <= 0x7F) {
            text.push_back(static_cast<char>(code));
        } else {
            const unsigned int bytes = code <= 0x7FF ? 2 : code <= 0xFFFF ? 3 : 4;
            if (text.size() + bytes > synclip::max_text_bytes) {
                valid = false;
                break;
            }
            constexpr uint32_t prefix[] = {0, 0, 0xC0, 0xE0, 0xF0};
            text.push_back(static_cast<char>(prefix[bytes] | (code >> (6 * (bytes - 1)))));
            for (unsigned int trailing = bytes - 1; trailing != 0; --trailing) {
                text.push_back(static_cast<char>(0x80 | ((code >> (6 * (trailing - 1))) & 0x3F)));
            }
        }
        if (text.size() == synclip::max_text_bytes && i + 1 < length) {
            valid = false;
            break;
        }
    }
    env->ReleaseStringChars(value, units);
    if (!valid) {
        return {};
    }
    return text;
}

jstring java_text(JNIEnv* env, std::string_view text) {
    if (text.empty() || text.size() > synclip::max_text_bytes) {
        return nullptr;
    }
    std::vector<jchar> units;
    units.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead == 0 || (lead >= 0x80 && lead < 0xC2) || lead > 0xF4) {
            return nullptr;
        }
        const unsigned int count = lead <= 0x7F ? 1 : lead <= 0xDF ? 2 : lead <= 0xEF ? 3 : 4;
        if (text.size() - i < count) {
            return nullptr;
        }
        constexpr uint32_t mask[] = {0, 0x7F, 0x1F, 0x0F, 0x07};
        constexpr uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        uint32_t code = lead & mask[count];
        for (unsigned int byte = 1; byte < count; ++byte) {
            const auto next = static_cast<unsigned char>(text[i + byte]);
            if ((next & 0xC0) != 0x80) {
                return nullptr;
            }
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum[count] || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) {
            return nullptr;
        }
        if (code <= 0xFFFF) {
            units.push_back(static_cast<jchar>(code));
        } else {
            code -= 0x10000;
            units.push_back(static_cast<jchar>(0xD800 | (code >> 10)));
            units.push_back(static_cast<jchar>(0xDC00 | (code & 0x3FF)));
        }
        i += count;
    }
    return env->NewString(units.data(), static_cast<jsize>(units.size()));
}

enum class ReceiveAlert { off, provider, preview, vibration };

ReceiveAlert receive_alert_mode(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd == -1) {
        return ReceiveAlert::off;
    }

    std::array<char, 16> text{};
    struct stat file{};
    ssize_t size = -1;
    if (fstat(fd, &file) == 0 && S_ISREG(file.st_mode) && file.st_uid == 0 &&
        (file.st_mode & (S_IWGRP | S_IWOTH)) == 0) {
        do {
            size = read(fd, text.data(), text.size());
        } while (size < 0 && errno == EINTR);
    }
    close(fd);
    if (size <= 0 || static_cast<size_t>(size) == text.size()) {
        return ReceiveAlert::off;
    }

    auto mode = std::string_view(text.data(), static_cast<size_t>(size));
    if (mode.back() == '\n') {
        mode.remove_suffix(1);
    }
    if (mode == "VIBRATION") {
        return ReceiveAlert::vibration;
    }
    if (mode == "TOAST" || mode == "NOTIFICATION") {
        return ReceiveAlert::provider;
    }
    if (mode == "NOTIFY_PREVIEW") {
        return ReceiveAlert::preview;
    }
    return ReceiveAlert::off;
}

struct ClipboardEvents {
    std::mutex mutex;
    std::condition_variable ready;
    bool changed = false;
    bool service_lost = false;
    std::string remote_text;
};

// Binder worker threads can deliver notifications until app_process exits.
ClipboardEvents& events() {
    static auto* lifetime_events = new ClipboardEvents;
    return *lifetime_events;
}

int run(JNIEnv* env, jobject bridge, bool check, const std::string& alert_mode_path) {
    if (prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0) {
        throw std::runtime_error("could not protect the Android runtime's memory");
    }
    auto& signal = events();
    const jclass type = env->GetObjectClass(bridge);
    const jmethodID attach = env->GetMethodID(type, "attach", "()Z");
    const jmethodID read_text = env->GetMethodID(type, "readText", "()Ljava/lang/String;");
    const jmethodID write_text = env->GetMethodID(type, "writeText", "(Ljava/lang/String;)Z");
    const jmethodID timestamp = env->GetMethodID(type, "timestamp", "()J");
    const jmethodID was_remote = env->GetMethodID(type, "wasRemote", "()Z");
    const jmethodID provider_alert = env->GetMethodID(type, "notifyReceiveAlertAsync", "()V");
    const jmethodID preview_alert = env->GetMethodID(type, "notifyReceivePreviewAsync", "(Ljava/lang/String;)V");
    const jmethodID vibration_alert = env->GetMethodID(type, "vibrateReceiveAlertAsync", "()V");
    if (preview_alert == nullptr || attach == nullptr || read_text == nullptr || write_text == nullptr ||
        timestamp == nullptr || was_remote == nullptr || provider_alert == nullptr ||
        vibration_alert == nullptr) {
        return 1;
    }
    const auto service_deadline = std::chrono::steady_clock::now() + 10s;
    while (!env->CallBooleanMethod(bridge, attach)) {
        if (env->ExceptionCheck()) {
            return 1;
        }
        if (std::chrono::steady_clock::now() >= service_deadline) {
            throw std::runtime_error("Android clipboard service unavailable");
        }
        std::this_thread::sleep_for(500ms);
    }
    std::fputs("SynClip: listening for Android clipboard changes\n", stderr);

    std::unique_ptr<synclip::Sender> sender;
    if (!check) {
        auto config = read_configuration();
        sender = std::make_unique<synclip::Sender>(std::move(config.host), config.port, config.key,
            [&signal](std::string text) {
                {
                    std::lock_guard lock(signal.mutex);
                    signal.remote_text = std::move(text);
                }
                signal.ready.notify_one();
            }, std::move(config.device));
    }

    const auto check_deadline = std::chrono::steady_clock::now() + 45s;
    std::string previous_text;
    jlong previous_timestamp = 0;
    while (true) {
        std::string remote;
        {
            std::unique_lock lock(signal.mutex);
            const auto notified = [&signal] {
                return signal.changed || signal.service_lost || !signal.remote_text.empty();
            };
            if (check) {
                if (!signal.ready.wait_until(lock, check_deadline, notified)) {
                    std::fputs("SynClip: no clipboard event received within 45 seconds\n", stderr);
                    return 3;
                }
            } else {
                signal.ready.wait(lock, notified);
            }
            if (signal.service_lost) {
                std::fputs("SynClip: Android clipboard service disconnected\n", stderr);
                return 1;
            }
            if (signal.changed) {
                signal.changed = false;
            } else {
                remote.swap(signal.remote_text);
            }
        }

        if (check) {
            std::fputs("SynClip: clipboard callback verified\n", stderr);
            return 0;
        }

        if (!remote.empty()) {
            const jstring value = java_text(env, remote);
            if (env->ExceptionCheck()) {
                return 1;
            }
            if (value == nullptr) {
                continue;
            }
            bool write = false;
            {
                std::lock_guard lock(signal.mutex);
                if (signal.remote_text.empty() && !signal.service_lost) {
                    if (signal.changed) {
                        signal.remote_text = std::move(remote);
                    } else {
                        write = true;
                    }
                }
            }
            const bool applied = write && env->CallBooleanMethod(bridge, write_text, value) == JNI_TRUE;
            if (env->ExceptionCheck()) {
                env->DeleteLocalRef(value);
                return 1;
            }
            if (applied) {
                switch (receive_alert_mode(alert_mode_path)) {
                case ReceiveAlert::provider:
                    env->CallVoidMethod(bridge, provider_alert);
                    break;
                case ReceiveAlert::preview:
                    env->CallVoidMethod(bridge, preview_alert, value);
                    break;
                case ReceiveAlert::vibration:
                    env->CallVoidMethod(bridge, vibration_alert);
                    break;
                case ReceiveAlert::off:
                    break;
                }
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    std::fputs("SynClip: could not queue a receive alert\n", stderr);
                }
            }
            env->DeleteLocalRef(value);
            continue;
        }

        auto value = static_cast<jstring>(env->CallObjectMethod(bridge, read_text));
        if (env->ExceptionCheck()) {
            return 1;
        }
        const bool is_echo = env->CallBooleanMethod(bridge, was_remote);
        if (env->ExceptionCheck()) {
            return 1;
        }
        if (is_echo) {
            env->DeleteLocalRef(value);
            continue;
        }
        auto text = utf8_text(env, value);
        env->DeleteLocalRef(value);
        if (env->ExceptionCheck()) {
            return 1;
        }
        const jlong changed_at = env->CallLongMethod(bridge, timestamp);
        if (env->ExceptionCheck()) {
            return 1;
        }
        if (text == previous_text && changed_at == previous_timestamp) {
            continue;
        }
        previous_text = text;
        previous_timestamp = changed_at;
        if (!text.empty() || changed_at != 0) {
            std::lock_guard lock(signal.mutex);
            signal.remote_text.clear();
        }
        sender->submit(std::move(text));
    }
}

}  // namespace

extern "C" JNIEXPORT void JNICALL Java_synclip_ClipboardBridge_nativeNotify(
        JNIEnv*, jclass, jboolean service_lost) {
    auto& signal = events();
    {
        std::lock_guard lock(signal.mutex);
        signal.changed = true;
        signal.service_lost |= service_lost == JNI_TRUE;
    }
    signal.ready.notify_one();
}

extern "C" JNIEXPORT jint JNICALL Java_synclip_ClipboardBridge_nativeRun(
        JNIEnv* env, jobject bridge, jboolean check, jstring alert_mode_path) {
    try {
        const auto mode_path = utf8_text(env, alert_mode_path);
        return env->ExceptionCheck() ? 1 : run(env, bridge, check == JNI_TRUE, mode_path);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SynClip: %s\n", error.what());
        return 1;
    }
}
