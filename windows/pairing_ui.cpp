#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include "pairing_ui.hpp"
#include "synclip/hub.hpp"
#include "qrcodegen.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace synclip::windows {
namespace {

constexpr wchar_t panel_class[] = L"SynClip.PairingPanel";
constexpr UINT_PTR update_timer = 1;
constexpr int address_control = 100;
constexpr int refresh_command = 101;
constexpr int devices_control = 102;
constexpr int revoke_command = 103;
constexpr RECT qr_area{20, 84, 370, 398};

std::wstring widen_utf8(std::string_view value) {
    if (value.empty() || value.size() > 4096) {
        return {};
    }
    const auto size = static_cast<int>(value.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, nullptr, 0);
    if (length == 0) {
        return {};
    }
    std::wstring wide(length, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, wide.data(), length) != length) {
        return {};
    }
    return wide;
}

std::string computer_name() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &length) || length == 0) {
        return "Windows";
    }
    char utf8[(MAX_COMPUTERNAME_LENGTH + 1) * 4]{};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, static_cast<int>(length),
                                         utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    return size > 0 ? std::string(utf8, size) : "Windows";
}

struct Address {
    std::string host;
    std::wstring label;
    int priority = 0;
};

bool usable_ipv4(const IN_ADDR& address) {
    const auto numeric = ntohl(address.s_addr);
    const auto prefix = numeric >> 24;
    return prefix != 0 && prefix != 127 && prefix < 224;
}

bool usable_ipv6(const IN6_ADDR& address, ULONG scope_id) {
    return scope_id == 0 && !IN6_IS_ADDR_UNSPECIFIED(&address) && !IN6_IS_ADDR_LOOPBACK(&address) &&
           !IN6_IS_ADDR_LINKLOCAL(&address) && !IN6_IS_ADDR_MULTICAST(&address) && !IN6_IS_ADDR_V4MAPPED(&address);
}

std::vector<Address> lan_addresses(const std::string& bind) {
    // An explicit bind can only advertise that same address.
    if (bind != "0.0.0.0" && bind != "::") {
        IN_ADDR ipv4{};
        IN6_ADDR ipv6{};
        if ((InetPtonA(AF_INET, bind.c_str(), &ipv4) == 1 && usable_ipv4(ipv4)) ||
            (InetPtonA(AF_INET6, bind.c_str(), &ipv6) == 1 && usable_ipv6(ipv6, 0))) {
            return {{bind, widen_utf8(bind), 0}};
        }
        return {};
    }

    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    const ULONG family = bind == "::" ? AF_UNSPEC : AF_INET;
    ULONG bytes = 15 * 1024;
    std::vector<std::max_align_t> storage;
    IP_ADAPTER_ADDRESSES* first = nullptr;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        storage.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(family, flags, nullptr, first, &bytes);
    }
    if (result != NO_ERROR) {
        return {};
    }

    std::vector<Address> addresses;
    for (const auto* adapter = first; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (const auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            const auto* socket = entry->Address.lpSockaddr;
            if (!socket || entry->DadState != IpDadStatePreferred) {
                continue;
            }
            char ip[INET6_ADDRSTRLEN]{};
            int priority = adapter->FirstGatewayAddress ? 20 : 0;
            if (adapter->IfType == IF_TYPE_IEEE80211 || adapter->IfType == IF_TYPE_ETHERNET_CSMACD) {
                priority += 10;
            }
            if (socket->sa_family == AF_INET && entry->Address.iSockaddrLength >= sizeof(SOCKADDR_IN)) {
                const auto& address = reinterpret_cast<const SOCKADDR_IN*>(socket)->sin_addr;
                if (!usable_ipv4(address) || !InetNtopA(AF_INET, &address, ip, sizeof(ip))) {
                    continue;
                }
                if ((ntohl(address.s_addr) >> 16) != 0xA9FE) {
                    priority += 40;
                }
            } else if (socket->sa_family == AF_INET6 && entry->Address.iSockaddrLength >= sizeof(SOCKADDR_IN6)) {
                const auto* address = reinterpret_cast<const SOCKADDR_IN6*>(socket);
                if (!usable_ipv6(address->sin6_addr, address->sin6_scope_id) ||
                    !InetNtopA(AF_INET6, &address->sin6_addr, ip, sizeof(ip))) {
                    continue;
                }
            } else {
                continue;
            }
            std::wstring label;
            if (adapter->FriendlyName && *adapter->FriendlyName) {
                label = std::wstring(adapter->FriendlyName) + L" · ";
            }
            label += widen_utf8(ip);
            addresses.push_back({ip, std::move(label), priority});
        }
    }
    std::sort(addresses.begin(), addresses.end(), [](const Address& a, const Address& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.label < b.label;
    });
    std::vector<Address> unique;
    for (auto& address : addresses) {
        if (std::none_of(unique.begin(), unique.end(), [&](const Address& item) { return item.host == address.host; })) {
            unique.push_back(std::move(address));
        }
    }
    return unique;
}

std::wstring phone_name(const Device& device) {
    auto name = widen_utf8(device.name);
    for (auto& ch : name) {
        if (ch < L' ') {
            ch = L' ';
        }
    }
    return name.empty() ? L"Android 设备" : name;
}

HWND control(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
             int x, int y, int width, int height, int id = 0, DWORD ex_style = 0) {
    const auto window = CreateWindowExW(ex_style, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                         parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    if (!window) {
        throw std::runtime_error("Could not create a pairing control.");
    }
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return window;
}

void draw_qr(HWND window, const std::optional<qrcodegen::QrCode>& qr) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    if (qr) {
        constexpr int quiet_zone = 4;
        const int total = qr->getSize() + quiet_zone * 2;
        const int unit = std::max(1, static_cast<int>(std::min(qr_area.right - qr_area.left, qr_area.bottom - qr_area.top)) / total);
        const int size = total * unit;
        const LONG x = qr_area.left + (qr_area.right - qr_area.left - size) / 2;
        const LONG y = qr_area.top + (qr_area.bottom - qr_area.top - size) / 2;
        const RECT paper{x, y, x + size, y + size};
        FillRect(dc, &paper, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        const auto black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        for (int row = 0; row < qr->getSize(); ++row) {
            for (int column = 0; column < qr->getSize(); ++column) {
                if (qr->getModule(column, row)) {
                    const LONG left = x + (column + quiet_zone) * unit;
                    const LONG top = y + (row + quiet_zone) * unit;
                    const RECT module{left, top, left + unit, top + unit};
                    FillRect(dc, &module, black);
                }
            }
        }
    }
    EndPaint(window, &paint);
}

} // namespace

struct PairingUi::Impl {
    struct Panel {
        Impl& ui;
        bool is_qr;
        HWND window = nullptr;
    };

    Impl(HWND owner, Hub& hub, std::string bind, std::uint16_t port)
        : owner_(owner), hub_(hub), bind_(std::move(bind)), port_(port), qr_panel_{*this, true}, devices_panel_{*this, false} {}

    ~Impl() {
        for (const auto window : {qr_panel_.window, devices_panel_.window}) {
            if (window) {
                DestroyWindow(window);
            }
        }
    }

    void show(Panel& panel, const wchar_t* title, int width, int height) {
        if (panel.window) {
            ShowWindow(panel.window, SW_SHOW);
            SetForegroundWindow(panel.window);
            return;
        }
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner_, GWLP_HINSTANCE));
        WNDCLASSW type{};
        type.lpfnWndProc = window_proc;
        type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        type.lpszClassName = panel_class;
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Could not register the pairing window.");
        }

        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        constexpr DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
        RECT client{0, 0, width, height};
        AdjustWindowRectEx(&client, style, FALSE, ex_style);
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &monitor.rcWork, 0);
        }
        const int full_width = client.right - client.left;
        const int full_height = client.bottom - client.top;
        const int x = monitor.rcWork.left + std::max(0, (static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - full_width) / 2);
        const int y = monitor.rcWork.top + std::max(0, (static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - full_height) / 2);
        const auto window = CreateWindowExW(ex_style, panel_class, title, style, x, y, full_width, full_height,
                                             owner_, nullptr, instance, &panel);
        if (!window) {
            throw std::runtime_error("Could not open the pairing window.");
        }
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        SetFocus(panel.is_qr ? addresses_control_ : list_);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* panel = static_cast<Panel*>(create->lpCreateParams);
            panel->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
        }
        auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (panel) {
            try {
                return panel->ui.handle(*panel, message, wparam, lparam);
            } catch (const std::exception&) {
                if (message == WM_CREATE) {
                    return -1;
                }
                MessageBoxW(window, L"操作失败。请稍后重试。", L"SynClip", MB_OK | MB_ICONERROR);
                return 0;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT handle(Panel& panel, UINT message, WPARAM wparam, LPARAM lparam) {
        const auto window = panel.window;
        switch (message) {
        case WM_CREATE:
            if (panel.is_qr) {
                create_qr_controls(window);
            } else {
                create_device_controls(window);
            }
            if (SetTimer(window, update_timer, 1000, nullptr) == 0) {
                throw std::runtime_error("Could not create a pairing refresh timer.");
            }
            return 0;
        case WM_TIMER:
            if (panel.is_qr) {
                tick_qr();
            } else {
                update_devices();
            }
            return 0;
        case WM_PAINT:
            if (panel.is_qr) {
                draw_qr(window, qr_);
                return 0;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case IDCANCEL:
                DestroyWindow(window);
                return 0;
            case address_control:
                if (HIWORD(wparam) == CBN_SELCHANGE) {
                    refresh_qr();
                }
                return 0;
            case refresh_command:
                refresh_qr();
                return 0;
            case devices_control:
                EnableWindow(revoke_, selected_device() != LB_ERR);
                return 0;
            case revoke_command:
                revoke_selected(window);
                return 0;
            default:
                break;
            }
            break;
        case WM_NCDESTROY:
            panel.window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            if (panel.is_qr) {
                hub_.cancel_pairing();
                offer_.reset();
                qr_.reset();
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void create_qr_controls(HWND window) {
        control(window, L"STATIC", L"在 Android 端扫描此二维码", 0, 20, 17, 350, 24);
        addresses_control_ = control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                      20, 48, 350, 200, address_control);
        for (const auto& address : addresses_) {
            if (SendMessageW(addresses_control_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(address.label.c_str())) < 0) {
                throw std::runtime_error("Could not list local addresses.");
            }
        }
        SendMessageW(addresses_control_, CB_SETCURSEL, 0, 0);
        status_ = control(window, L"STATIC", L"", 0, 20, 413, 240, 24);
        control(window, L"BUTTON", L"换一个码", BS_PUSHBUTTON | WS_TABSTOP, 266, 408, 104, 30, refresh_command);
        refresh_qr();
    }

    void refresh_qr() {
        const auto selected = SendMessageW(addresses_control_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR || static_cast<std::size_t>(selected) >= addresses_.size()) {
            return;
        }
        hub_.cancel_pairing();
        offer_.reset();
        qr_.reset();
        InvalidateRect(qr_panel_.window, &qr_area, TRUE);
        try {
            auto offer = hub_.begin_pairing(addresses_[selected].host, port_, computer_name());
            qr_ = qrcodegen::QrCode::encodeText(offer.uri.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            offer_.emplace(std::move(offer));
        } catch (...) {
            hub_.cancel_pairing();
            SetWindowTextW(status_, L"二维码生成失败");
            throw;
        }
        tick_qr();
    }

    void tick_qr() {
        if (!offer_) {
            return;
        }
        const auto remaining = offer_->expires - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero() || !hub_.pairing_active(offer_->id)) {
            SetWindowTextW(status_, remaining <= std::chrono::steady_clock::duration::zero() ?
                                   L"二维码已过期" : L"此二维码已使用");
            offer_.reset();
            qr_.reset();
            InvalidateRect(qr_panel_.window, &qr_area, TRUE);
            return;
        }
        const auto seconds = std::chrono::ceil<std::chrono::seconds>(remaining).count();
        const auto text = L"有效期剩余 " + std::to_wstring(seconds) + L" 秒";
        SetWindowTextW(status_, text.c_str());
    }

    void create_device_controls(HWND window) {
        list_ = control(window, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                        16, 16, 438, 192, devices_control, WS_EX_CLIENTEDGE);
        summary_ = control(window, L"STATIC", L"", 0, 16, 224, 192, 26);
        revoke_ = control(window, L"BUTTON", L"取消配对", BS_PUSHBUTTON | WS_TABSTOP, 224, 219, 112, 32, revoke_command);
        control(window, L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP, 342, 219, 112, 32, IDCANCEL);
        devices_.reset();
        update_devices();
    }

    LRESULT selected_device() const {
        return SendMessageW(list_, LB_GETCURSEL, 0, 0);
    }

    void update_devices() {
        auto devices = hub_.devices();
        std::sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) { return a.id < b.id; });
        if (devices_ && std::equal(devices.begin(), devices.end(), devices_->begin(), devices_->end(),
            [](const Device& a, const Device& b) { return a.id == b.id && a.name == b.name && a.connected == b.connected; })) {
            return;
        }
        std::string old_selection;
        const auto selection = selected_device();
        if (devices_ && selection != LB_ERR && static_cast<std::size_t>(selection) < devices_->size()) {
            old_selection = (*devices_)[selection].id;
        }
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (const auto& device : devices) {
            std::wstring line = phone_name(device) + (device.connected ? L"  ·  在线" : L"  ·  离线");
            if (device.id.size() >= 8) {
                line += L"  ·  " + widen_utf8(std::string_view(device.id).substr(device.id.size() - 6));
            }
            const auto row = SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
            if (row < 0) {
                SendMessageW(list_, LB_RESETCONTENT, 0, 0);
                devices_.reset();
                EnableWindow(revoke_, FALSE);
                throw std::runtime_error("Could not display paired devices.");
            }
            if (device.id == old_selection) {
                SendMessageW(list_, LB_SETCURSEL, row, 0);
            }
        }
        const auto text = devices.empty() ? L"尚未配对手机" : L"已配对 " + std::to_wstring(devices.size()) + L" 台手机";
        SetWindowTextW(summary_, text.c_str());
        EnableWindow(revoke_, selected_device() != LB_ERR);
        devices_ = std::move(devices);
    }

    void revoke_selected(HWND window) {
        const auto selection = selected_device();
        if (!devices_ || selection == LB_ERR || static_cast<std::size_t>(selection) >= devices_->size()) {
            return;
        }
        const auto device = (*devices_)[selection];
        const auto question = L"取消与“" + phone_name(device) + L"”的配对？此设备需要重新扫码连接。";
        if (MessageBoxW(window, question.c_str(), L"取消配对", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            hub_.revoke(device.id);
            update_devices();
        }
    }

    HWND owner_;
    Hub& hub_;
    std::string bind_;
    std::uint16_t port_;
    Panel qr_panel_;
    Panel devices_panel_;
    HWND addresses_control_ = nullptr;
    HWND status_ = nullptr;
    HWND list_ = nullptr;
    HWND summary_ = nullptr;
    HWND revoke_ = nullptr;
    std::vector<Address> addresses_;
    std::optional<PairingOffer> offer_;
    std::optional<qrcodegen::QrCode> qr_;
    std::optional<std::vector<Device>> devices_;
};

PairingUi::PairingUi(HWND owner, Hub& hub, std::string bind_address, std::uint16_t port)
    : impl_(std::make_unique<Impl>(owner, hub, std::move(bind_address), port)) {}

PairingUi::~PairingUi() = default;

void PairingUi::show_qr() {
    if (!impl_->qr_panel_.window) {
        impl_->addresses_ = lan_addresses(impl_->bind_);
        if (impl_->addresses_.empty()) {
            MessageBoxW(impl_->owner_, L"没有可用于配对的局域网地址。请检查网络和监听地址。", L"扫码配对",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    impl_->show(impl_->qr_panel_, L"SynClip · 扫码配对", 390, 456);
}

void PairingUi::show_devices() {
    impl_->show(impl_->devices_panel_, L"SynClip · 已配对手机", 470, 268);
}

bool PairingUi::dispatch(MSG& message) const {
    for (const auto window : {impl_->qr_panel_.window, impl_->devices_panel_.window}) {
        if (window && (message.hwnd == window || IsChild(window, message.hwnd)) && IsDialogMessageW(window, &message)) {
            return true;
        }
    }
    return false;
}

} // namespace synclip::windows
