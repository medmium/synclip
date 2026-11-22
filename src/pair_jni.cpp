#include "synclip/transport.hpp"

#include <jni.h>

#include <stdexcept>

namespace {

std::string argument(JNIEnv* env, jstring value) {
    if (!value || env->GetStringLength(value) > 128) {
        throw std::invalid_argument("Invalid pairing link");
    }
    const auto release = [env, value](const char* text) { env->ReleaseStringUTFChars(value, text); };
    const std::unique_ptr<const char, decltype(release)> text(env->GetStringUTFChars(value, nullptr), release);
    if (!text) {
        throw std::runtime_error("Could not read pairing link");
    }
    return text.get();
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_synclip_pair_PairActivity_nativeEnroll(
        JNIEnv* env, jclass, jstring host, jint port, jstring device, jstring ticket) {
    try {
        if (port < 1 || port > 65535) {
            throw std::invalid_argument("Invalid pairing port");
        }
        const auto key = synclip::enroll_device(argument(env, host), static_cast<std::uint16_t>(port),
                                                argument(env, device), synclip::parse_key(argument(env, ticket)));
        return env->NewStringUTF(synclip::format_key(key).c_str());
    } catch (const std::exception&) {
        if (!env->ExceptionCheck()) {
            if (const auto error = env->FindClass("java/io/IOException")) {
                env->ThrowNew(error, "Could not pair with Windows. Check the address, network and QR expiration.");
            }
        }
        return nullptr;
    }
}
