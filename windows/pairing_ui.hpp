#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace synclip {
class Hub;
}

namespace synclip::windows {

class PairingUi {
public:
    PairingUi(HWND owner, Hub& hub, std::string bind_address, std::uint16_t port);
    ~PairingUi();

    void show_qr();
    void show_devices();
    bool dispatch(MSG& message) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace synclip::windows
