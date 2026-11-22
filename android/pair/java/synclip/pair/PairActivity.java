package synclip.pair;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.provider.Settings;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public final class PairActivity extends Activity implements ScanPreview.Listener {
    private static final int REQUEST_CAMERA = 1;
    private static final int REQUEST_NOTIFICATIONS = 2;
    private static final String PREFLIGHT =
            "[ \"$(id -u)\" = 0 ] || exit 20\n"
            + "module=/data/adb/modules/synclip\n"
            + "[ ! -e \"$module/disable\" ] && [ ! -e \"$module/remove\" ] && "
            + "[ ! -e /data/adb/modules_update/synclip/module.prop ] || exit 21\n"
            + "version=$(sed -n 's/^versionCode=//p' \"$module/module.prop\" 2>/dev/null)\n"
            + "case \"$version\" in ''|*[!0-9]*) exit 21 ;; esac\n"
            + "[ \"$version\" -ge 3 ] || exit 21\n"
            + "umask 077\n"
            + "dir=/data/adb/synclip\n"
            + "[ ! -L \"$dir\" ] && mkdir -p \"$dir\" && chmod 0700 \"$dir\" || exit 22\n"
            + "[ ! -L \"$dir/config\" ] && { [ ! -e \"$dir/config\" ] || [ -f \"$dir/config\" ]; } || exit 22\n";
    private static final String SAVE = PREFLIGHT
            + "temp=$(mktemp \"$dir/.config.XXXXXX\") || exit 22\n"
            + "trap 'rm -f \"$temp\"' EXIT\n"
            + "cat >\"$temp\" && [ -s \"$temp\" ] && chmod 0600 \"$temp\" "
            + "&& mv -f \"$temp\" \"$dir/config\" || exit 22\n";

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private ScanPreview preview;
    private TextView status;
    private Button action;
    private Button alertOptions;
    private boolean scanning;
    private boolean resumed;
    private byte[] pendingConfig;

    private static native String nativeEnroll(String host, int port, String id, String invitationKey)
            throws IOException;

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(22), dp(18), dp(22), dp(16));
        TextView title = new TextView(this);
        title.setText("SynClip 配对");
        title.setTextAppearance(android.R.style.TextAppearance_Material_Headline);
        layout.addView(title);
        status = new TextView(this);
        status.setTextSize(16);
        status.setMinHeight(dp(54));
        status.setPadding(0, dp(8), 0, dp(12));
        status.setText("在 Windows 上打开配对二维码，然后用本机扫描。");
        layout.addView(status);
        preview = new ScanPreview(this, this);
        preview.view.setContentDescription("二维码相机预览");
        layout.addView(preview.view, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        action = new Button(this);
        action.setText("扫描二维码");
        action.setOnClickListener(view -> {
            if (pendingConfig == null) {
                requestScan();
            } else {
                byte[] config = pendingConfig;
                action.setEnabled(false);
                status.setText("正在写入配对配置…");
                worker.execute(() -> save(config));
            }
        });
        layout.addView(action);
        alertOptions = new Button(this);
        alertOptions.setOnClickListener(view -> showAlertOptions());
        layout.addView(alertOptions);
        updateAlertOptions();
        setContentView(layout);
    }

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        updateAlertOptions();
        if (scanning) {
            preview.start();
        }
    }

    @Override protected void onPause() {
        resumed = false;
        preview.stop();
        super.onPause();
    }

    @Override protected void onDestroy() {
        preview.close();
        worker.shutdown();
        super.onDestroy();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void requestScan() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQUEST_CAMERA);
            return;
        }
        scanning = true;
        status.setText("请将 Windows 上的二维码放入取景画面。");
        action.setEnabled(false);
        if (resumed) {
            preview.start();
        }
    }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == REQUEST_CAMERA) {
            if (results.length != 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
                requestScan();
            } else {
                status.setText("请在系统设置中为 SynClip 配对启用相机权限。");
            }
        } else if (request == REQUEST_NOTIFICATIONS) {
            updateAlertOptions();
            SyncAlerts.Mode mode = SyncAlerts.selectedMode(this);
            if (results.length != 0 && mode.usesNotifications && !SyncAlerts.notificationsAvailable(this, mode)) {
                offerNotificationSettings();
            }
        }
    }

    private void updateAlertOptions() {
        SyncAlerts.Mode mode = SyncAlerts.selectedMode(this);
        boolean blocked = mode.usesNotifications && !SyncAlerts.notificationsAvailable(this, mode);
        alertOptions.setText("同步提醒 · " + mode.label + (blocked ? "（通知未开启）" : ""));
    }

    private void showAlertOptions() {
        if (scanning) {
            scanAgain("需要配对时，请扫描 Windows 上的二维码。");
        }
        SyncAlerts.Mode[] modes = SyncAlerts.Mode.values();
        String[] choices = new String[modes.length];
        for (int i = 0; i < modes.length; ++i) {
            choices[i] = modes[i].label;
        }
        CheckBox notificationContent = new CheckBox(this);
        notificationContent.setText("在系统通知中显示复制的内容");
        notificationContent.setChecked(SyncAlerts.showsNotificationContent(this));
        notificationContent.setOnCheckedChangeListener((button, enabled) ->
                SyncAlerts.saveShowNotificationContent(this, enabled));
        LinearLayout previewOptions = new LinearLayout(this);
        previewOptions.setPadding(dp(20), 0, dp(20), 0);
        previewOptions.addView(notificationContent);

        new AlertDialog.Builder(this)
                .setTitle("同步提醒")
                .setSingleChoiceItems(choices, SyncAlerts.selectedMode(this).ordinal(), (dialog, which) -> {
                    dialog.dismiss();
                    SyncAlerts.Mode mode = modes[which];
                    SyncAlerts.saveMode(this, mode);
                    updateAlertOptions();
                    if (mode.usesNotifications) {
                        if (!SyncAlerts.hasNotificationPermission(this)) {
                            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQUEST_NOTIFICATIONS);
                        } else if (!SyncAlerts.notificationsAvailable(this, mode)) {
                            offerNotificationSettings();
                        }
                    }
                })
                .setView(previewOptions)
                .setPositiveButton("完成", null)
                .show();
    }

    private void offerNotificationSettings() {
        new AlertDialog.Builder(this)
                .setTitle("启用通知权限")
                .setMessage("系统通知和后台 Toast 需要允许 SynClip 配对显示通知。")
                .setPositiveButton("打开系统设置", (dialog, which) -> {
                    try {
                        startActivity(new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                                .putExtra(Settings.EXTRA_APP_PACKAGE, getPackageName()));
                    } catch (ActivityNotFoundException error) {
                        Toast.makeText(this, "请在系统设置中开启 SynClip 配对的通知权限。", Toast.LENGTH_LONG).show();
                    }
                })
                .setNegativeButton("暂不开启", null)
                .show();
    }

    @Override public void onCameraFailure() {
        scanAgain("相机暂时无法使用。请关闭正在使用相机的应用后重试。");
    }

    @Override public void onScan(String text) {
        if (!scanning || !resumed) {
            return;
        }
        PairingInvite invite = PairingInvite.parse(text);
        if (invite == null) {
            status.setText("请扫描 SynClip 在 Windows 上生成的配对二维码。");
            return;
        }
        scanning = false;
        preview.stop();
        new AlertDialog.Builder(this)
                .setTitle("确认配对")
                .setMessage("连接 Windows " + invite.address() + "？这会替换本机现有的 SynClip 配对。")
                .setPositiveButton("确认配对", (dialog, which) -> enroll(invite))
                .setNegativeButton("取消", (dialog, which) -> scanAgain("配对已取消。"))
                .setOnCancelListener(dialog -> scanAgain("配对已取消。"))
                .show();
    }

    private void scanAgain(String message) {
        scanning = false;
        preview.stop();
        status.setText(message);
        action.setText("扫描二维码");
        action.setEnabled(true);
    }

    private void enroll(PairingInvite invite) {
        status.setText("正在检查 root 权限和 SynClip 模块…");
        worker.execute(() -> {
            int result = rootCommand(PREFLIGHT, null);
            if (result != 0) {
                show(() -> scanAgain(rootError(result)));
                return;
            }
            show(() -> status.setText("正在连接 Windows…"));
            final String key;
            try {
                System.loadLibrary("synclip_pair_jni");
                key = nativeEnroll(invite.host, invite.port, invite.device, invite.key);
            } catch (IOException | LinkageError | RuntimeException error) {
                show(() -> scanAgain("配对失败。请检查网络并在 Windows 上重新生成二维码。"));
                return;
            }
            if (key == null || !key.matches("[0-9a-f]{64}")) {
                show(() -> scanAgain("配对失败。请在 Windows 上重新生成二维码。"));
                return;
            }
            byte[] config = ("host=" + invite.host + "\nport=" + invite.port
                    + "\ndevice=" + invite.device + "\nkey=" + key + "\n")
                    .getBytes(StandardCharsets.US_ASCII);
            save(config);
        });
    }

    private void save(byte[] config) {
        int result = rootCommand(SAVE, config);
        show(() -> {
            if (result == 0) {
                pendingConfig = null;
                scanAgain("配对完成。SynClip 模块会自动连接 Windows。");
            } else {
                pendingConfig = config;
                status.setText("Windows 已登记此设备。" + rootError(result));
                action.setText("重试写入配置");
                action.setEnabled(true);
            }
        });
    }

    private static String rootError(int result) {
        if (result == 21) {
            return "请安装并启用 SynClip 0.3 或更新的模块，重启手机后再配对。";
        }
        if (result == 22) {
            return "无法写入 SynClip 模块配置。请确认模块可用后重试。";
        }
        return "请在 Magisk 或 KernelSU 中允许 SynClip 配对使用 root。";
    }

    private static int rootCommand(String command, byte[] data) {
        Process process = null;
        try {
            process = new ProcessBuilder("su", "-c", command).redirectErrorStream(true)
                    .redirectOutput(new File("/dev/null")).start();
            try (OutputStream input = process.getOutputStream()) {
                if (data != null) {
                    input.write(data);
                }
            }
            return process.waitFor(45, TimeUnit.SECONDS) ? process.exitValue() : -1;
        } catch (IOException error) {
            return -1;
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return -1;
        } finally {
            if (process != null && process.isAlive()) {
                process.destroyForcibly();
            }
        }
    }

    private void show(Runnable update) {
        runOnUiThread(() -> {
            if (!isFinishing() && !isDestroyed()) {
                update.run();
            }
        });
    }
}
