# SynClip 0.3

SynClip 在局域网内同步 Windows 和 Android 的纯文本剪贴板。一台 Windows 电脑可同时连接多台手机。从一台手机复制的文本会经电脑同步到其余已配对的手机。

## 安装与扫码配对

从 GitHub 下载源码后，先按下文的构建步骤生成 `dist/` 中的安装文件。以下路径以项目根目录为基准。

1. 退出 Windows 托盘的旧版 SynClip，运行 `dist\windows-v0.3.5\SynClip.exe`。若防火墙询问，允许它访问专用网络。
2. 用 Magisk 或 KernelSU 管理器安装 `dist\synclip-android-v0.3.5.zip`，然后重启手机。模块会在启动后安装内置的 SynClip 配对应用。模块支持 Android 10 及以上的 `arm64-v8a` 和 `x86_64` 设备。KernelSU 无需系统挂载元模块。
3. 电脑和手机接入同一可互通的局域网。在 Windows 托盘选「扫码配对」，选择手机可访问的电脑 IP。
4. 在手机的模块管理器里点 SynClip 的「操作」，或从应用列表打开 **SynClip 配对**。扫描电脑上的二维码，确认地址并按提示授予 Root 权限。Windows 的设备管理会显示连接状态。配对后可以关闭应用，由模块继续同步。
5. 添加更多手机时重复扫码流程，每台手机使用单独生成的二维码。最多可添加 16 台设备，可在 Windows 托盘逐台移除。

二维码三分钟内有效，成功配对后即失效。扫码配对无需 ADB。新版模块已经启动时，扫码更换配对无需再次重启。每台手机配对到一个 Windows 主机，重新扫码会切换该手机的主机。

从旧版升级时，用新版 ZIP 覆盖安装原模块即可保留手机配置。由 Windows 0.2 保留的配对会显示为「旧版手机」。要为它分配独立密钥，可重新扫码，再在 Windows 移除「旧版手机」。

## 手机同步提醒

打开 **SynClip 配对 → 同步提醒**，选择关闭、Toast、系统通知或振动。默认关闭，选项保存在本机。提醒在手机写入经 Windows 同步来的文本后触发。选择系统通知时，可另行勾选 **在系统通知中显示复制的内容**。正文预览默认关闭，开启后会截短较长的文本。

可以关闭手机上的配对界面。后台模块负责保持同步连接和直接触发振动。Toast 和系统通知通过受权限保护的本地接口，以 SynClip 的应用身份显示。更改提醒方式后，模块会在几秒内读取新设置，无需重新连接。Windows 端请保留托盘程序运行。

Toast 和系统通知需要允许 SynClip 配对显示通知。选择对应模式时按系统提示授权。部分 ROM 会拦截长时间处于后台的应用发出的 Toast，在运行 Android 16 的 ColorOS 真机上观察到了这一行为。系统通知会更新同一条通知，振动使用系统通知振动设置。 [Android 通知权限](https://developer.android.com/develop/ui/compose/notifications/notification-permission) · [Android 振动 API](https://developer.android.com/reference/android/os/Vibrator)

## Windows 托盘选项

右键单击 SynClip 托盘图标。勾选 **登录 Windows 时启动**，当前账号登录后 SynClip 会自动在托盘运行。勾选 **通知中显示手机剪贴板**，电脑写入从手机同步来的文本后会显示一段通知预览。预览默认关闭。

## 同步行为

- 支持中文、表情和多行文本。单条 UTF-8 传输上限为 1 MiB，Android 剪贴板还受 Binder 事务大小限制。
- Android 标记为敏感的文本、Windows 标记为排除同步或监视的内容、图片和文件会被忽略。两端会识别同步写入，防止回传。
- 各设备断线后自动重连。每台设备最多保留最近一条待发文本，系统自带的剪贴板历史设置继续生效。
- 每台扫码配对的手机有独立密钥，通过 TLS 1.3 加密。剪贴板内容保存在内存中，移除设备会断开它并取消后续访问。

## 从源码构建

Windows 需要 CMake 3.24+、Ninja 和支持 C++17 的 MSVC 或 MinGW-w64。CMake 首次配置会下载并验证 Mbed TLS 和 QR 编码器源码。

```powershell
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows
cmake --install build/windows --prefix dist/windows-v0.3.5 --component SynClipRuntime
```

Android 构建需要 SDK Platform 29 和 34、Android Build Tools 35 或以上、NDK r27 或以上、JDK 17 或以上、CMake 3.24+ 和 Ninja。可用 `-SdkRoot` 和 `-NdkRoot` 指定本机工具链。脚本会签名配对 APK，并把它嵌入模块 ZIP。

首次构建会在 `build/pair-app/` 生成本地 APK 签名密钥。要覆盖安装自己构建的旧版配对应用，请使用同一签名密钥。

```powershell
.\scripts\build-android.ps1
```

双端同步、二维码编码和配对认证使用 C++。Android 通过 Java/JNI 调用相机和系统剪贴板接口。不同 ROM 的 Shell 权限可能有差异。 [AOSP ClipboardService](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/services/core/java/com/android/server/clipboard/ClipboardService.java) · [Magisk 模块文档](https://topjohnwu.github.io/Magisk/guides.html) · [KernelSU 模块文档](https://kernelsu.org/guide/module.html) · [QR Code generator](https://github.com/nayuki/QR-Code-generator/tree/v1.8.0)

## 排查

- 二维码过期时，在 Windows 重新生成二维码。电脑有虚拟网卡时，选择与手机在同一局域网的网卡地址。
- 配对后离线时，检查模块已启用并在安装后重启过，检查 Windows 防火墙许可和局域网互通。默认 TCP 端口为 `45873`，Windows 可通过 `%LOCALAPPDATA%\SynClip\config.ini` 修改 `bind=` 或 `port=`。
- Android 运行日志可以用 `adb logcat -d -s SynClip` 查看。
