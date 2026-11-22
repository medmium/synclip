package synclip;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.PersistableBundle;
import android.os.Process;
import android.os.RemoteException;
import android.os.TransactionTooLargeException;
import android.os.VibrationEffect;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Adapts Android system services to the native synchronizer. */
public final class ClipboardBridge {
    private static final int SHELL_UID = 2000;
    private static final int SYSTEM_UID = 1000;
    private static final String SHELL_PACKAGE = "com.android.shell";
    private static final String SERVICE = "android.content.IClipboard";
    private static final String LISTENER = "android.content.IOnPrimaryClipChangedListener";
    private static final String SENSITIVE = "android.content.extra.IS_SENSITIVE";
    private static final String ORIGIN = "synclip.clipboard_origin";
    private static final String RECEIVE_ALERT_AUTHORITY = "synclip.pair.alerts";
    private static final String RECEIVE_ALERT_URI = "content://" + RECEIVE_ALERT_AUTHORITY;
    private static final String RECEIVE_ALERT_ACK = "Result: Bundle[{accepted=true}]";
    private static final String VIBRATION_REASON = "SynClip clipboard received";
    private static final int OWNER_USER = 0;
    private static final int DEFAULT_DEVICE = 0;

    private final String origin = UUID.randomUUID().toString();
    private final IBinder vibrationToken = new Binder();
    private final ThreadPoolExecutor receiveAlerts = new ThreadPoolExecutor(
            0, 1, 30, TimeUnit.SECONDS, new ArrayBlockingQueue<>(1), task -> {
                Thread worker = new Thread(task, "SynClip-receive-alert");
                worker.setDaemon(true);
                return worker;
            }, new ThreadPoolExecutor.DiscardOldestPolicy());
    private Object service;
    private Method getPrimaryClip;
    private Method setPrimaryClip;
    private long lastTimestamp;
    private boolean lastWasRemote;
    private boolean receiveAlertFailed;
    private boolean previewAlertFailed;
    private boolean vibrationFailed;

    private final Binder callback = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == IBinder.FIRST_CALL_TRANSACTION) {
                data.enforceInterface(LISTENER);
                if (Binder.getCallingUid() == SYSTEM_UID) {
                    nativeNotify(false);
                }
                return true;
            }
            return super.onTransact(code, data, reply, flags);
        }
    };

    public static void main(String[] args) {
        if (args.length != 2 || Process.myUid() != SHELL_UID) {
            System.err.println("SynClip: Android runtime requires UID 2000 and launcher arguments");
            System.exit(2);
        }
        try {
            System.load(args[0]);
            File modeFile = new File(new File(args[0]).getParentFile(), "alert.mode");
            System.exit(new ClipboardBridge().nativeRun("--check".equals(args[1]), modeFile.getPath()));
        } catch (ReflectiveOperationException | RemoteException | RuntimeException | LinkageError error) {
            Throwable cause = error;
            if (error instanceof InvocationTargetException && error.getCause() != null) {
                cause = error.getCause();
            }
            System.err.println("SynClip: Android clipboard bridge failed ("
                    + cause.getClass().getSimpleName() + ")");
            System.exit(1);
        }
    }

    private static Object[] clipboardArguments(int length) {
        switch (length) {
            case 2:  // Android 10–11
                return new Object[] {SHELL_PACKAGE, OWNER_USER};
            case 3:  // Android 12–13
                return new Object[] {SHELL_PACKAGE, null, OWNER_USER};
            case 4:  // Android 14+, including the virtual device argument
                return new Object[] {SHELL_PACKAGE, null, OWNER_USER, DEFAULT_DEVICE};
            default:
                throw new IllegalStateException("Unsupported clipboard interface");
        }
    }

    private static Method clipboardMethod(Class<?> binderApi, String name, Class<?>... prefix)
            throws NoSuchMethodException {
        Class<?>[][] signatures = {
            {String.class, String.class, int.class, int.class},
            {String.class, String.class, int.class},
            {String.class, int.class}
        };
        for (Class<?>[] signature : signatures) {
            Class<?>[] parameters = new Class<?>[prefix.length + signature.length];
            System.arraycopy(prefix, 0, parameters, 0, prefix.length);
            System.arraycopy(signature, 0, parameters, prefix.length, signature.length);
            try {
                return binderApi.getMethod(name, parameters);
            } catch (NoSuchMethodException ignored) {
                // The generated Binder interface depends on the device's Android release.
            }
        }
        throw new NoSuchMethodException(name);
    }

    private boolean attach() throws ReflectiveOperationException, RemoteException {
        Class<?> serviceManager = Class.forName("android.os.ServiceManager");
        Method checkService = serviceManager.getMethod("checkService", String.class);
        IBinder binder = (IBinder) checkService.invoke(null, "clipboard");
        IBinder packages = (IBinder) checkService.invoke(null, "package");
        if (binder == null || packages == null) {
            return false;
        }

        Class<?> packageApi = Class.forName("android.content.pm.IPackageManager");
        Object packageManager = asInterface(packageApi, packages);
        int permission = (int) packageApi.getMethod("checkPermission", String.class, String.class,
                int.class).invoke(packageManager, "android.permission.READ_CLIPBOARD_IN_BACKGROUND",
                         SHELL_PACKAGE, OWNER_USER);
        if (permission != PackageManager.PERMISSION_GRANTED) {
            System.err.println("SynClip: this Android ROM does not grant shell background clipboard access");
            throw new SecurityException();
        }

        Class<?> serviceApi = Class.forName(SERVICE);
        Class<?> listenerApi = Class.forName(LISTENER);
        getPrimaryClip = clipboardMethod(serviceApi, "getPrimaryClip");
        setPrimaryClip = clipboardMethod(serviceApi, "setPrimaryClip", ClipData.class);
        Method addListener = clipboardMethod(serviceApi, "addPrimaryClipChangedListener", listenerApi);
        service = asInterface(serviceApi, binder);
        callback.attachInterface(null, LISTENER);
        Object listener = asInterface(listenerApi, callback);
        binder.linkToDeath(() -> nativeNotify(true), 0);
        invokeWithItem(addListener, listener);
        return true;
    }

    private static Object asInterface(Class<?> binderApi, IBinder binder)
            throws ReflectiveOperationException {
        Class<?> stub = Class.forName(binderApi.getName() + "$Stub");
        return stub.getMethod("asInterface", IBinder.class).invoke(null, binder);
    }

    private void invokeWithItem(Method method, Object item) throws ReflectiveOperationException {
        Object[] arguments = clipboardArguments(method.getParameterTypes().length - 1);
        Object[] withItem = new Object[arguments.length + 1];
        withItem[0] = item;
        System.arraycopy(arguments, 0, withItem, 1, arguments.length);
        method.invoke(service, withItem);
    }

    private String readText() throws ReflectiveOperationException {
        lastTimestamp = 0;
        lastWasRemote = false;
        ClipData clip;
        try {
            clip = (ClipData) getPrimaryClip.invoke(service,
                    clipboardArguments(getPrimaryClip.getParameterTypes().length));
        } catch (InvocationTargetException error) {
            if (error.getCause() instanceof TransactionTooLargeException) {
                return null;
            }
            throw error;
        }
        if (clip == null || clip.getItemCount() == 0) {
            return null;
        }
        ClipDescription description = clip.getDescription();
        if (description != null) {
            lastTimestamp = description.getTimestamp();
            PersistableBundle extras = description.getExtras();
            if (extras != null) {
                lastWasRemote = origin.equals(extras.getString(ORIGIN));
                if (lastWasRemote || extras.getBoolean(SENSITIVE, false)) {
                    return null;
                }
            }
        }
        ClipData.Item item = clip.getItemAt(0);
        CharSequence text = item.getText();
        return text == null || item.getIntent() != null || item.getUri() != null
                ? null : text.toString();
    }

    private boolean writeText(String text) throws ReflectiveOperationException {
        ClipData clip = ClipData.newPlainText("SynClip", text);
        PersistableBundle extras = new PersistableBundle();
        extras.putString(ORIGIN, origin);
        clip.getDescription().setExtras(extras);
        try {
            invokeWithItem(setPrimaryClip, clip);
        } catch (InvocationTargetException error) {
            if (error.getCause() instanceof TransactionTooLargeException) {
                System.err.println("SynClip: incoming text exceeds Android's clipboard transaction limit");
                return false;
            }
            throw error;
        }
        return true;
    }

    private void notifyReceiveAlertAsync() {
        enqueueAlert(this::notifyReceiveAlert);
    }

    private void notifyReceivePreviewAsync(String text) {
        enqueueAlert(() -> notifyReceivePreview(text));
    }

    private void notifyReceivePreview(String text) {
        Bundle extras = new Bundle();
        final int limit = 240;
        if (text.codePointCount(0, text.length()) > limit) {
            text = text.substring(0, text.offsetByCodePoints(0, limit - 1)) + "…";
        }
        extras.putString("text", text);
        try {
            if (callPreviewProvider(extras)) {
                previewAlertFailed = false;
                return;
            }
        } catch (ReflectiveOperationException | RuntimeException ignored) {
            // Never include clipboard data in diagnostics or the command-based fallback.
        }
        if (!previewAlertFailed) {
            previewAlertFailed = true;
            System.err.println("SynClip: receive notification preview unavailable");
        }
        notifyReceiveAlert();
    }

    private static boolean callPreviewProvider(Bundle extras) throws ReflectiveOperationException {
        Class<?> activity = Class.forName("android.app.ActivityManager");
        Class<?> managerApi = Class.forName("android.app.IActivityManager");
        Object manager = activity.getMethod("getService").invoke(null);
        Method release = managerApi.getMethod("removeContentProviderExternalAsUser", String.class,
                IBinder.class, int.class);
        IBinder token = new Binder();
        Object holder = managerApi.getMethod("getContentProviderExternal", String.class, int.class,
                IBinder.class, String.class).invoke(manager, RECEIVE_ALERT_AUTHORITY, OWNER_USER, token, "SynClip");
        if (holder == null) {
            return false;
        }
        try {
            Object provider = holder.getClass().getField("provider").get(holder);
            Class<?> providerApi = Class.forName("android.content.IContentProvider");
            final String method = "received_preview";
            Bundle response;
            if (Build.VERSION.SDK_INT >= 31) {
                Class<?> sourceType = Class.forName("android.content.AttributionSource");
                Class<?> builderType = Class.forName("android.content.AttributionSource$Builder");
                Object builder = builderType.getConstructor(int.class).newInstance(SHELL_UID);
                builderType.getMethod("setPackageName", String.class).invoke(builder, SHELL_PACKAGE);
                Object source = builderType.getMethod("build").invoke(builder);
                response = (Bundle) providerApi.getMethod("call", sourceType, String.class, String.class,
                        String.class, Bundle.class).invoke(provider, source, RECEIVE_ALERT_AUTHORITY, method, null, extras);
            } else if (Build.VERSION.SDK_INT == 30) {
                response = (Bundle) providerApi.getMethod("call", String.class, String.class, String.class,
                        String.class, String.class, Bundle.class).invoke(provider, SHELL_PACKAGE, null,
                        RECEIVE_ALERT_AUTHORITY, method, null, extras);
            } else {
                response = (Bundle) providerApi.getMethod("call", String.class, String.class, String.class,
                        String.class, Bundle.class).invoke(provider, SHELL_PACKAGE, RECEIVE_ALERT_AUTHORITY,
                        method, null, extras);
            }
            return response != null && response.getBoolean("accepted");
        } finally {
            release.invoke(manager, RECEIVE_ALERT_AUTHORITY, token, OWNER_USER);
        }
    }

    private void vibrateReceiveAlertAsync() {
        enqueueAlert(this::vibrateReceiveAlert);
    }

    private void enqueueAlert(Runnable alert) {
        try {
            receiveAlerts.execute(alert);
        } catch (SecurityException ignored) {
            // Optional alerts must not interrupt clipboard synchronization.
        }
    }

    private void vibrateReceiveAlert() {
        try {
            requestVibration();
            vibrationFailed = false;
        } catch (ReflectiveOperationException | RuntimeException error) {
            if (!vibrationFailed) {
                vibrationFailed = true;
                Throwable cause = error instanceof InvocationTargetException && error.getCause() != null
                        ? error.getCause() : error;
                System.err.println("SynClip: direct vibration unavailable ("
                        + cause.getClass().getSimpleName() + ")");
            }
        }
    }

    private void requestVibration() throws ReflectiveOperationException {
        Method checkService = Class.forName("android.os.ServiceManager").getMethod("checkService", String.class);
        IBinder manager = (IBinder) checkService.invoke(null, "vibrator_manager");
        VibrationEffect effect = VibrationEffect.createOneShot(180, VibrationEffect.DEFAULT_AMPLITUDE);
        if (manager == null) {
            IBinder legacy = (IBinder) checkService.invoke(null, "vibrator");
            if (legacy == null) {
                throw new IllegalStateException("Android vibrator unavailable");
            }
            Class<?> api = Class.forName("android.os.IVibratorService");
            Object vibrator = asInterface(api, legacy);
            Method vibrate;
            try {
                vibrate = api.getMethod("vibrate", int.class, String.class, VibrationEffect.class,
                        int.class, String.class, IBinder.class);
            } catch (NoSuchMethodException legacyAttributesSignature) {
                Class<?> attributes = Class.forName("android.os.VibrationAttributes");
                vibrate = api.getMethod("vibrate", int.class, String.class, VibrationEffect.class,
                        attributes, String.class, IBinder.class);
                vibrate.invoke(vibrator, SHELL_UID, SHELL_PACKAGE, effect,
                        legacyNotificationVibrationAttributes(attributes, effect), VIBRATION_REASON, vibrationToken);
                return;
            }
            vibrate.invoke(vibrator, SHELL_UID, SHELL_PACKAGE, effect, AudioAttributes.USAGE_NOTIFICATION,
                    VIBRATION_REASON, vibrationToken);
            return;
        }

        Class<?> api = Class.forName("android.os.IVibratorManagerService");
        Object vibrator = asInterface(api, manager);
        Class<?> combined = Class.forName("android.os.CombinedVibration");
        Class<?> attributes = Class.forName("android.os.VibrationAttributes");
        Object vibration = combined.getMethod("createParallel", VibrationEffect.class).invoke(null, effect);
        Object usage = notificationVibrationAttributes(attributes);
        Method vibrate;
        try {
            vibrate = api.getMethod("vibrate", int.class, int.class, String.class, combined,
                    attributes, String.class, IBinder.class);
        } catch (NoSuchMethodException noDeviceIdSignature) {
            vibrate = api.getMethod("vibrate", int.class, String.class, combined,
                    attributes, String.class, IBinder.class);
            vibrate.invoke(vibrator, SHELL_UID, SHELL_PACKAGE, vibration, usage, VIBRATION_REASON, vibrationToken);
            return;
        }
        vibrate.invoke(vibrator, SHELL_UID, DEFAULT_DEVICE, SHELL_PACKAGE, vibration, usage,
                VIBRATION_REASON, vibrationToken);
    }

    private static Object legacyNotificationVibrationAttributes(Class<?> attributes, VibrationEffect effect)
            throws ReflectiveOperationException {
        // Android 11 checks the original audio usage for DND settings.
        AudioAttributes audio = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION).build();
        Class<?> builderType = Class.forName(attributes.getName() + "$Builder");
        Object builder = builderType.getConstructor(AudioAttributes.class, VibrationEffect.class)
                .newInstance(audio, effect);
        return builderType.getMethod("build").invoke(builder);
    }

    private static Object notificationVibrationAttributes(Class<?> attributes) throws ReflectiveOperationException {
        Class<?> builderType = Class.forName(attributes.getName() + "$Builder");
        Object builder = builderType.getConstructor().newInstance();
        // Use notification policy and system-default amplitude, with no override flags.
        int usage = attributes.getField("USAGE_NOTIFICATION").getInt(null);
        builderType.getMethod("setUsage", int.class).invoke(builder, usage);
        return builderType.getMethod("build").invoke(builder);
    }

    private void notifyReceiveAlert() {
        java.lang.Process command = null;
        try {
            // A provider call can deliver to the background without a deferred broadcast.
            File nullDevice = new File("/dev/null");
            command = new ProcessBuilder("/system/bin/content", "call", "--user",
                    Integer.toString(OWNER_USER), "--uri", RECEIVE_ALERT_URI,
                    "--method", "received")
                    .redirectInput(nullDevice)
                    .redirectError(nullDevice)
                    .start();
            if (!command.waitFor(3, TimeUnit.SECONDS)) {
                command.destroyForcibly();
                command.waitFor();
                logReceiveAlertFailure("timed out");
                return;
            }
            try (InputStream response = command.getInputStream()) {
                if (command.exitValue() != 0 || !receiveAlertAccepted(response)) {
                    logReceiveAlertFailure("not acknowledged");
                    return;
                }
            }
            receiveAlertFailed = false;
        } catch (IOException | RuntimeException ignored) {
            logReceiveAlertFailure("unavailable");
        } catch (InterruptedException interrupted) {
            if (command != null) {
                command.destroyForcibly();
            }
            Thread.currentThread().interrupt();
        }
    }

    private static boolean receiveAlertAccepted(InputStream response) throws IOException {
        byte[] buffer = new byte[256];
        int size = 0;
        while (size < buffer.length) {
            int count = response.read(buffer, size, buffer.length - size);
            if (count < 0) {
                return RECEIVE_ALERT_ACK.equals(new String(buffer, 0, size, StandardCharsets.UTF_8).trim());
            }
            size += count;
        }
        return false;
    }

    private void logReceiveAlertFailure(String kind) {
        if (!receiveAlertFailed) {
            receiveAlertFailed = true;
            System.err.println("SynClip: receive alert " + kind);
        }
    }

    private long timestamp() {
        return lastTimestamp;
    }

    private boolean wasRemote() {
        return lastWasRemote;
    }

    private native int nativeRun(boolean check, String alertModePath) throws ReflectiveOperationException, RemoteException;
    private static native void nativeNotify(boolean serviceLost);
}
