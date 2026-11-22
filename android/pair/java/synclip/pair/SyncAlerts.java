package synclip.pair;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.widget.Toast;

final class SyncAlerts {
    private static final String PREFERENCES = "sync_alert";
    private static final String MODE = "mode";
    private static final String SHOW_NOTIFICATION_CONTENT = "show_notification_content";
    private static final String CHANNEL = "clipboard_sync";
    private static final String RECEIVED = "收到一条同步文本";
    private static final int NOTIFICATION_ID = 1;
    private static final int MAX_PREVIEW_CHARACTERS = 240;

    enum Mode {
        OFF("关闭", false),
        TOAST("Toast", true),
        NOTIFICATION("系统通知", true),
        VIBRATION("振动", false);

        final String label;
        final boolean usesNotifications;

        Mode(String label, boolean usesNotifications) {
            this.label = label;
            this.usesNotifications = usesNotifications;
        }
    }

    private static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
    }

    static Mode selectedMode(Context context) {
        try {
            return Mode.valueOf(preferences(context).getString(MODE, Mode.OFF.name()));
        } catch (IllegalArgumentException | ClassCastException error) {
            return Mode.OFF;
        }
    }

    static boolean showsNotificationContent(Context context) {
        try {
            return preferences(context).getBoolean(SHOW_NOTIFICATION_CONTENT, false);
        } catch (ClassCastException error) {
            return false;
        }
    }

    static void saveShowNotificationContent(Context context, boolean enabled) {
        preferences(context).edit().putBoolean(SHOW_NOTIFICATION_CONTENT, enabled).apply();
        if (!enabled) {
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            if (manager != null) {
                manager.cancel(NOTIFICATION_ID);
            }
        }
    }

    static void saveMode(Context context, Mode mode) {
        preferences(context).edit().putString(MODE, mode.name()).apply();
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager != null) {
            if (mode == Mode.NOTIFICATION) {
                createChannel(manager);
            } else {
                manager.cancel(NOTIFICATION_ID);
            }
        }
    }

    static boolean hasNotificationPermission(Context context) {
        return Build.VERSION.SDK_INT < 33 || context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED;
    }

    static boolean notificationsAvailable(Context context, Mode mode) {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (!hasNotificationPermission(context) || manager == null || !manager.areNotificationsEnabled()) {
            return false;
        }
        NotificationChannel channel = manager.getNotificationChannel(CHANNEL);
        return mode != Mode.NOTIFICATION || channel == null || channel.getImportance() != NotificationManager.IMPORTANCE_NONE;
    }

    static void deliver(Context context, String text) {
        Mode mode = selectedMode(context);
        if (mode.usesNotifications && !notificationsAvailable(context, mode)) {
            return;
        }
        try {
            switch (mode) {
                case TOAST:
                    Toast.makeText(context, RECEIVED, Toast.LENGTH_SHORT).show();
                    break;
                case NOTIFICATION:
                    showNotification(context, showsNotificationContent(context) ? text : null);
                    break;
                case VIBRATION: // The module daemon delivers vibration directly.
                case OFF:
                    break;
            }
        } catch (SecurityException ignored) {
            // Android may revoke permission while an alert is being delivered.
        }
    }

    private static void createChannel(NotificationManager manager) {
        NotificationChannel channel = new NotificationChannel(CHANNEL, "剪贴板同步提醒",
                NotificationManager.IMPORTANCE_LOW);
        channel.setSound(null, null);
        channel.enableVibration(false);
        manager.createNotificationChannel(channel);
    }

    private static void showNotification(Context context, String text) {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager == null) {
            return;
        }
        createChannel(manager);
        Notification.Builder builder = new Notification.Builder(context, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("SynClip")
                .setShowWhen(true)
                .setOnlyAlertOnce(true)
                .setVisibility(Notification.VISIBILITY_PRIVATE);
        if (text == null || text.isEmpty()) {
            builder.setContentText(RECEIVED);
        } else {
            String preview = text;
            if (text.codePointCount(0, text.length()) > MAX_PREVIEW_CHARACTERS) {
                preview = text.substring(0, text.offsetByCodePoints(0, MAX_PREVIEW_CHARACTERS - 1)) + "…";
            }
            builder.setContentText(preview).setStyle(new Notification.BigTextStyle().bigText(preview));
        }
        manager.notify(NOTIFICATION_ID, builder.build());
    }
}
