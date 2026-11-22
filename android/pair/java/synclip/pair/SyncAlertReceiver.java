package synclip.pair;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.os.Build;
import android.os.VibrationAttributes;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.widget.Toast;

public final class SyncAlertReceiver extends BroadcastReceiver {
    private static final String ACTION = "synclip.pair.action.SYNC_RECEIVED";
    private static final String PREFERENCES = "sync_alert";
    private static final String MODE = "mode";
    private static final String CHANNEL = "clipboard_sync";
    private static final String RECEIVED = "收到一条同步文本";
    private static final int NOTIFICATION_ID = 1;

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

    @Override public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION.equals(intent.getAction())) {
            return;
        }
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
                    showNotification(context);
                    break;
                case VIBRATION:
                    vibrate(context);
                    break;
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

    private static void showNotification(Context context) {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager == null) {
            return;
        }
        createChannel(manager);
        Notification notification = new Notification.Builder(context, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("SynClip")
                .setContentText(RECEIVED)
                .setShowWhen(true)
                .setOnlyAlertOnce(true)
                .setVisibility(Notification.VISIBILITY_PRIVATE)
                .build();
        manager.notify(NOTIFICATION_ID, notification);
    }

    @SuppressWarnings("deprecation")
    private static void vibrate(Context context) {
        Vibrator vibrator = context.getSystemService(Vibrator.class);
        if (vibrator == null || !vibrator.hasVibrator()) {
            return;
        }
        VibrationEffect pulse = VibrationEffect.createOneShot(180, VibrationEffect.DEFAULT_AMPLITUDE);
        if (Build.VERSION.SDK_INT >= 33) {
            vibrator.vibrate(pulse, new VibrationAttributes.Builder()
                    .setUsage(VibrationAttributes.USAGE_NOTIFICATION).build());
        } else {
            vibrator.vibrate(pulse, new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION).build());
        }
    }
}
