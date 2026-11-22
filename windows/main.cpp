#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include "pairing_ui.hpp"
#include "synclip/hub.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

constexpr wchar_t app_name[] = L"SynClip";
constexpr wchar_t window_class[] = L"SynClip.Receiver.Window";
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT worker_message = WM_APP + 2;
constexpr UINT_PTR write_timer = 1;
constexpr UINT_PTR tray_timer = 2;
constexpr UINT_PTR capture_timer = 3;
constexpr UINT clipboard_retry_ms = 50;
constexpr unsigned clipboard_retry_limit = 20;
constexpr UINT command_config = 100;
constexpr UINT command_error = 101;
constexpr UINT command_exit = 102;
constexpr UINT command_pair = 103;
constexpr UINT command_devices = 104;

struct Config {
    std::string bind_address = "0.0.0.0";
    uint16_t port = synclip::default_port;
    std::optional<synclip::Key> key;
};

std::optional<std::wstring> decode_utf8(std::string_view text) {
    if (text.empty()) {
        return std::wstring{};
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        text.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto size = static_cast<int>(text.size());
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (length == 0) {
        return std::nullopt;
    }
    std::wstring wide(length, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, wide.data(), length) == 0) {
        return std::nullopt;
    }
    return wide;
}

std::wstring windows_line_endings(std::wstring_view text) {
    std::wstring converted;
    converted.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (ch == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
            converted += L'\r';
        }
        converted += ch;
    }
    return converted;
}

std::filesystem::path default_config_path() {
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0) {
        throw std::runtime_error("LOCALAPPDATA is unavailable. Use --config PATH.");
    }
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size);
    if (length == 0 || length >= size) {
        throw std::runtime_error("Could not read LOCALAPPDATA. Use --config PATH.");
    }
    value.resize(length);
    return std::filesystem::path(value) / L"SynClip" / L"config.ini";
}

void create_config(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());

    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << "bind=0.0.0.0\r\nport=" << synclip::default_port << "\r\n";
        output.close();

        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
                throw std::runtime_error("Could not save the initial config.ini (Windows error " +
                                         std::to_string(error) + ").");
            }
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::string_view trim(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

Config load_config(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        create_config(path);
    }

    Config config;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open config.ini.");
    }
    bool have_key = false;
    bool have_bind = false;
    bool have_port = false;
    size_t line_number = 0;
    for (std::string line; std::getline(input, line);) {
        ++line_number;
        std::string_view record(line);
        if (line_number == 1 && record.substr(0, 3) == "\xEF\xBB\xBF") {
            record.remove_prefix(3);
        }
        record = trim(record);
        if (record.empty() || record.front() == '#' || record.front() == ';') {
            continue;
        }
        const auto separator = record.find('=');
        const auto name = trim(record.substr(0, separator));
        const auto value = separator == std::string_view::npos ? std::string_view{} : trim(record.substr(separator + 1));
        if (value.empty() || value.find('\0') != std::string_view::npos) {
            throw std::runtime_error("Missing or invalid value at config.ini line " + std::to_string(line_number) + '.');
        }
        if (name == "bind" && !have_bind) {
            config.bind_address = value;
            have_bind = true;
        } else if (name == "port" && !have_port) {
            unsigned port = 0;
            const auto last = value.data() + value.size();
            const auto result = std::from_chars(value.data(), last, port);
            if (result.ec != std::errc{} || result.ptr != last || port == 0 || port > 65535) {
                throw std::runtime_error("config.ini port must be an integer between 1 and 65535.");
            }
            config.port = static_cast<uint16_t>(port);
            have_port = true;
        } else if (name == "key" && !have_key) {
            config.key = synclip::parse_key(value);
            have_key = true;
        } else {
            throw std::runtime_error("Unknown or repeated setting at config.ini line " + std::to_string(line_number) + '.');
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Could not read config.ini.");
    }
    return config;
}

struct ClipboardRevision {
    DWORD sequence = 0;
    HWND owner = nullptr;

    bool operator==(const ClipboardRevision& other) const {
        return sequence == other.sequence && owner == other.owner;
    }
};

ClipboardRevision clipboard_revision() {
    return {GetClipboardSequenceNumber(), GetClipboardOwner()};
}

class ClipboardAccess {
public:
    explicit ClipboardAccess(HWND window) : opened_(OpenClipboard(window) != FALSE) {}
    ~ClipboardAccess() {
        if (opened_) {
            CloseClipboard();
        }
    }

    ClipboardAccess(const ClipboardAccess&) = delete;
    ClipboardAccess& operator=(const ClipboardAccess&) = delete;

    explicit operator bool() const { return opened_; }

private:
    bool opened_;
};

class LockedClipboardMemory {
public:
    explicit LockedClipboardMemory(HGLOBAL memory) : memory_(memory), data_(memory ? GlobalLock(memory) : nullptr) {}
    ~LockedClipboardMemory() {
        if (data_) {
            GlobalUnlock(memory_);
        }
    }

    LockedClipboardMemory(const LockedClipboardMemory&) = delete;
    LockedClipboardMemory& operator=(const LockedClipboardMemory&) = delete;

    const void* data() const { return data_; }

private:
    HGLOBAL memory_;
    void* data_;
};

struct PrivacyFormats {
    UINT exclude = 0;
    UINT history = 0;
    UINT cloud = 0;
    UINT viewer_ignore = 0;
};

bool clipboard_flag_disabled(UINT format) {
    if (!IsClipboardFormatAvailable(format)) {
        return false;
    }
    const auto memory = GetClipboardData(format);
    const LockedClipboardMemory locked(memory);
    DWORD value = 0;
    if (locked.data() && GlobalSize(memory) >= sizeof(value)) {
        std::memcpy(&value, locked.data(), sizeof(value));
    }
    return value != 1;
}

struct ClipboardCapture {
    std::string text;
    bool sensitive = false;
};

ClipboardCapture read_clipboard(const PrivacyFormats& formats) {
    if (IsClipboardFormatAvailable(formats.exclude) || IsClipboardFormatAvailable(formats.viewer_ignore) ||
        clipboard_flag_disabled(formats.history) || clipboard_flag_disabled(formats.cloud)) {
        return {{}, true};
    }
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return {};
    }

    const auto memory = GetClipboardData(CF_UNICODETEXT);
    const LockedClipboardMemory locked(memory);
    const auto* first = static_cast<const wchar_t*>(locked.data());
    if (!first) {
        return {};
    }
    const auto units = std::min<std::size_t>(GlobalSize(memory) / sizeof(*first), synclip::max_text_bytes + 1);
    const auto* terminator = std::find(first, first + units, L'\0');
    if (terminator == first || terminator == first + units) {
        return {};
    }

    const auto length = static_cast<int>(terminator - first);
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, first, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0 || static_cast<size_t>(size) > synclip::max_text_bytes) {
        return {};
    }
    std::string text(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, first, length, text.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return {std::move(text)};
}

struct PendingWrite {
    std::wstring text;
    ClipboardRevision before;
};

enum class ClipboardResult { written, busy, superseded, failed };

ClipboardResult write_clipboard(HWND window, const PendingWrite& pending, DWORD& written_sequence) {
    const ClipboardAccess clipboard(window);
    if (!clipboard) {
        return ClipboardResult::busy;
    }
    if (!(clipboard_revision() == pending.before)) {
        return ClipboardResult::superseded;
    }

    const auto& text = pending.text;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        if (auto* buffer = GlobalLock(memory)) {
            std::memcpy(buffer, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) {
                written_sequence = GetClipboardSequenceNumber();
                return ClipboardResult::written;
            }
        }
        GlobalFree(memory);
    }
    return ClipboardResult::failed;
}

class Receiver {
public:
    explicit Receiver(std::filesystem::path path) : path_(std::move(path)) {}

    int run(HINSTANCE instance, Config config) {
        hub_ = std::make_unique<synclip::Hub>(path_.parent_path() / L"devices.ini", config.key);
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSW window_type{};
        window_type.lpfnWndProc = window_proc;
        window_type.hInstance = instance;
        window_type.lpszClassName = window_class;
        if (!RegisterClassW(&window_type)) {
            throw std::runtime_error("Could not register the receiver window.");
        }
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, window_class, app_name, WS_OVERLAPPED,
                                  0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!window_) {
            throw std::runtime_error("Could not create the receiver window.");
        }

        privacy_formats_ = {
            RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"),
            RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"),
            RegisterClipboardFormatW(L"CanUploadToCloudClipboard"),
            RegisterClipboardFormatW(L"Clipboard Viewer Ignore"),
        };
        last_capture_ = clipboard_revision();
        if (!privacy_formats_.exclude || !privacy_formats_.history || !privacy_formats_.cloud ||
            !privacy_formats_.viewer_ignore || !AddClipboardFormatListener(window_)) {
            DestroyWindow(window_);
            throw std::runtime_error("Could not initialize the Windows clipboard listener.");
        }
        add_tray_icon();
        std::thread worker;
        try {
            pairing_ui_ = std::make_unique<synclip::windows::PairingUi>(window_, *hub_, config.bind_address, config.port);
            worker = std::thread([this, config] {
                try {
                    synclip::run_server(config.bind_address, config.port, stop_,
                        [this](synclip::ServerStatus status) {
                            std::lock_guard lock(mailbox_mutex_);
                            mailbox_.status = status;
                            notify_window();
                        }, [this](std::string text) {
                            const auto revision = clipboard_revision();
                            std::lock_guard lock(mailbox_mutex_);
                            mailbox_.clipboard = IncomingText{std::move(text), revision};
                            notify_window();
                        }, *hub_);
                } catch (const std::exception& error) {
                    std::lock_guard lock(mailbox_mutex_);
                    mailbox_.error = decode_utf8(error.what()).value_or(L"未知错误");
                    notify_window();
                }
            });
        } catch (...) {
            DestroyWindow(window_);
            throw;
        }

        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!pairing_ui_->dispatch(message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        stop_.store(true);
        worker.join();
        if (result == -1) {
            if (window_) {
                DestroyWindow(window_);
            }
            throw std::runtime_error("The receiver message loop failed.");
        }
        return exit_code_;
    }

private:
    struct IncomingText {
        std::string text;
        ClipboardRevision at_arrival;
    };

    struct Update {
        std::optional<synclip::ServerStatus> status;
        std::optional<IncomingText> clipboard;
        std::optional<std::wstring> error;
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* receiver = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!receiver) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        try {
            return receiver->handle_message(window, message, wparam, lparam);
        } catch (const std::exception&) {
            receiver->exit_code_ = 1;
            MessageBoxW(window, L"接收器遇到内部错误，即将退出。", app_name, MB_OK | MB_ICONERROR);
            DestroyWindow(window);
            return 0;
        }
    }

    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbar_created_ != 0 && message == taskbar_created_) {
            add_tray_icon();
            return 0;
        }
        switch (message) {
        case worker_message:
            apply_update();
            return 0;
        case WM_CLIPBOARDUPDATE:
            begin_capture();
            return 0;
        case tray_message: {
            const auto event = LOWORD(lparam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) {
                show_menu();
            }
            return 0;
        }
        case WM_TIMER:
            if (wparam == write_timer) {
                apply_clipboard();
            } else if (wparam == capture_timer) {
                capture_clipboard();
            } else if (wparam == tray_timer) {
                add_tray_icon();
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam) {
                DestroyWindow(window);
            }
            return 0;
        case WM_DESTROY: {
            stop_.store(true);
            RemoveClipboardFormatListener(window);
            {
                std::lock_guard lock(mailbox_mutex_);
                window_ = nullptr;
            }
            NOTIFYICONDATAW icon{};
            icon.cbSize = sizeof(icon);
            icon.hWnd = window;
            icon.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &icon);
            PostQuitMessage(exit_code_);
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    void notify_window() {
        if (window_ && !message_posted_) {
            message_posted_ = PostMessageW(window_, worker_message, 0, 0) != FALSE;
        }
    }

    void apply_update() {
        Update update;
        {
            std::lock_guard lock(mailbox_mutex_);
            update = std::exchange(mailbox_, {});
            message_posted_ = false;
        }
        if (update.status) {
            network_ready_ = true;
            const auto devices = hub_->devices();
            const auto online = std::count_if(devices.begin(), devices.end(), [](const auto& phone) { return phone.connected; });
            connection_status_ = online ? std::to_wstring(online) + L" 台手机在线" : L"等待手机连接";
        }
        if (update.clipboard) {
            pending_clipboard_.reset();
            KillTimer(window_, write_timer);
            if (const auto decoded = decode_utf8(update.clipboard->text)) {
                const auto revision = clipboard_revision();
                if (revision == update.clipboard->at_arrival || is_self_write(revision)) {
                    stop_capture();
                    pending_clipboard_ = PendingWrite{windows_line_endings(*decoded), revision};
                    write_attempts_ = 0;
                    apply_clipboard();
                }
            } else {
                clipboard_status_ = L"已忽略 Windows 无法写入的文本";
            }
        }
        if (update.error) {
            connection_status_ = L"接收器出错";
            network_ready_ = false;
            error_ = std::move(*update.error);
        }
        auto icon = tray_icon();
        icon.uFlags = NIF_TIP | NIF_SHOWTIP;
        Shell_NotifyIconW(NIM_MODIFY, &icon);
        if (update.error) {
            MessageBoxW(window_, error_.c_str(), L"SynClip 接收器出错", MB_OK | MB_ICONERROR);
        }
    }

    void apply_clipboard() {
        if (!pending_clipboard_) {
            KillTimer(window_, write_timer);
            return;
        }
        DWORD written_sequence = 0;
        const auto result = write_clipboard(window_, *pending_clipboard_, written_sequence);
        if (result == ClipboardResult::busy && ++write_attempts_ < clipboard_retry_limit &&
            SetTimer(window_, write_timer, clipboard_retry_ms, nullptr) != 0) {
            return;
        }
        KillTimer(window_, write_timer);
        pending_clipboard_.reset();
        if (result == ClipboardResult::written) {
            written_sequence_ = written_sequence;
            clipboard_status_ = L"已同步手机文本";
        } else if (result != ClipboardResult::superseded) {
            clipboard_status_ = L"手机文本写入失败";
        }
    }

    bool is_self_write(ClipboardRevision revision) const {
        // A delayed renderer can change ownership before the sequence advances.
        return revision.owner == window_ ||
               (!revision.owner && written_sequence_ && revision.sequence == *written_sequence_);
    }

    bool ignore_capture(ClipboardRevision revision) const {
        return is_self_write(revision) || (pending_clipboard_ && revision == pending_clipboard_->before);
    }

    void begin_capture() {
        const auto revision = clipboard_revision();
        if (ignore_capture(revision)) {
            stop_capture();
            return;
        }
        if (!(revision == last_capture_)) {
            hub_->submit({});
        }
        capture_pending_ = true;
        capture_attempts_ = 0;
        capture_clipboard();
    }

    void stop_capture() {
        capture_pending_ = false;
        KillTimer(window_, capture_timer);
    }

    void capture_clipboard() {
        if (!capture_pending_) {
            return;
        }
        ClipboardCapture capture;
        ClipboardRevision revision;
        {
            const ClipboardAccess clipboard(window_);
            if (!clipboard) {
                if (++capture_attempts_ < clipboard_retry_limit &&
                    SetTimer(window_, capture_timer, clipboard_retry_ms, nullptr) != 0) {
                    return;
                }
                stop_capture();
                hub_->submit({});
                clipboard_status_ = L"本机剪贴板读取失败";
                return;
            }
            if (ignore_capture(clipboard_revision())) {
                stop_capture();
                return;
            }
            capture = read_clipboard(privacy_formats_);
            revision = clipboard_revision();
        }
        stop_capture();
        if (revision == last_capture_ && !capture.text.empty()) {
            return;
        }
        last_capture_ = revision;
        if (capture.sensitive) {
            clipboard_status_ = L"已跳过本机标记为私密的剪贴板";
        } else if (capture.text.empty()) {
            clipboard_status_ = L"本机剪贴板没有可同步的文本";
        } else {
            clipboard_status_ = L"本机文本已提交同步";
        }
        hub_->submit(std::move(capture.text));
    }

    NOTIFYICONDATAW tray_icon() const {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window_;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        icon.uCallbackMessage = tray_message;
        icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        const auto tooltip = std::wstring(L"SynClip · ") + connection_status_;
        lstrcpynW(icon.szTip, tooltip.c_str(), static_cast<int>(std::size(icon.szTip)));
        return icon;
    }

    void add_tray_icon() {
        auto icon = tray_icon();
        if (Shell_NotifyIconW(NIM_ADD, &icon)) {
            icon.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &icon);
            KillTimer(window_, tray_timer);
        } else if (Shell_NotifyIconW(NIM_MODIFY, &icon)) {
            KillTimer(window_, tray_timer);
        } else {
            SetTimer(window_, tray_timer, 5000, nullptr);
        }
    }

    void show_menu() {
        using Menu = std::unique_ptr<std::remove_pointer_t<HMENU>, decltype(&DestroyMenu)>;
        Menu menu(CreatePopupMenu(), DestroyMenu);
        if (!menu) {
            return;
        }
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, connection_status_.c_str());
        if (clipboard_status_) {
            AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, clipboard_status_);
        }
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING | (network_ready_ ? MF_ENABLED : MF_GRAYED), command_pair, L"扫码配对…");
        AppendMenuW(menu.get(), MF_STRING, command_devices, L"管理设备…");
        AppendMenuW(menu.get(), MF_STRING, command_config, L"打开网络配置…");
        if (!error_.empty()) {
            AppendMenuW(menu.get(), MF_STRING, command_error, L"查看错误…");
        }
        AppendMenuW(menu.get(), MF_STRING, command_exit, L"退出");

        const HWND window = window_;
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window);
        const auto command = TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                             point.x, point.y, window, nullptr);
        if (!window_) {
            return;
        }
        PostMessageW(window, WM_NULL, 0, 0);
        if (command == command_exit) {
            DestroyWindow(window);
        } else if (command == command_error) {
            MessageBoxW(window, error_.c_str(), L"SynClip 接收器出错", MB_OK | MB_ICONERROR);
        } else if (command == command_config) {
            open_config(window);
        } else if ((command == command_pair || command == command_devices) && pairing_ui_) {
            try {
                if (command == command_pair) {
                    pairing_ui_->show_qr();
                } else {
                    pairing_ui_->show_devices();
                }
            } catch (const std::exception& error) {
                const auto message = decode_utf8(error.what()).value_or(L"无法打开配对工具。");
                MessageBoxW(window, message.c_str(), app_name, MB_OK | MB_ICONERROR);
            }
        }
    }

    void open_config(HWND window) const {
        wchar_t system_directory[MAX_PATH]{};
        const auto length = GetSystemDirectoryW(system_directory, static_cast<UINT>(std::size(system_directory)));
        if (length != 0 && length < std::size(system_directory)) {
            const auto notepad = std::filesystem::path(system_directory) / L"notepad.exe";
            const auto argument = L"\"" + path_.native() + L"\"";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", notepad.c_str(), argument.c_str(),
                                                        nullptr, SW_SHOWNORMAL)) > 32) {
                return;
            }
        }
        const auto message = std::wstring(L"无法打开配置文件。可以用记事本打开以下路径。\n\n") + path_.native();
        MessageBoxW(window, message.c_str(), app_name, MB_OK | MB_ICONINFORMATION);
    }

    std::filesystem::path path_;
    HWND window_ = nullptr;
    UINT taskbar_created_ = 0;
    int exit_code_ = 0;
    std::wstring connection_status_ = L"正在启动";
    bool network_ready_ = false;
    const wchar_t* clipboard_status_ = nullptr;
    std::wstring error_;
    PrivacyFormats privacy_formats_;
    std::optional<PendingWrite> pending_clipboard_;
    std::optional<DWORD> written_sequence_;
    ClipboardRevision last_capture_;
    unsigned write_attempts_ = 0;
    unsigned capture_attempts_ = 0;
    bool capture_pending_ = false;
    std::unique_ptr<synclip::Hub> hub_;
    std::unique_ptr<synclip::windows::PairingUi> pairing_ui_;
    std::atomic_bool stop_{false};
    std::mutex mailbox_mutex_;
    Update mailbox_;
    bool message_posted_ = false;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    std::filesystem::path path;
    try {
        int argc = 0;
        auto argv = std::unique_ptr<wchar_t*, decltype(&LocalFree)>(CommandLineToArgvW(GetCommandLineW(), &argc), LocalFree);
        if (!argv) {
            throw std::runtime_error("Could not read the command line.");
        }
        const auto args = argv.get();
        if (argc == 2 && std::wstring_view(args[1]) == L"--help") {
            MessageBoxW(nullptr, L"SynClip.exe [--config 文件路径]\n\n默认配置文件位于 %LOCALAPPDATA%\\SynClip\\config.ini。",
                        app_name, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (argc == 1) {
            path = default_config_path();
        } else if (argc == 3 && std::wstring_view(args[1]) == L"--config" && *args[2]) {
            path = std::filesystem::absolute(args[2]);
        } else {
            throw std::runtime_error("Usage: SynClip.exe [--config PATH]");
        }

        auto single_instance = std::unique_ptr<void, decltype(&CloseHandle)>(
            CreateMutexW(nullptr, FALSE, L"Local\\SynClip.Receiver"), CloseHandle);
        const auto mutex_error = GetLastError();
        if (!single_instance) {
            throw std::runtime_error("Could not check for an existing receiver.");
        }
        if (mutex_error == ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"SynClip 已在此会话运行。请查看系统托盘。", app_name, MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        return Receiver(path).run(instance, load_config(path));
    } catch (const std::exception& error) {
        auto message = decode_utf8(error.what()).value_or(L"未知错误");
        if (!path.empty()) {
            message += L"\n\n" + path.native();
        }
        MessageBoxW(nullptr, message.c_str(), L"SynClip 启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}
