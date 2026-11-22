package synclip;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.PersistableBundle;
import android.os.Process;
import android.os.RemoteException;
import android.os.TransactionTooLargeException;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Adapts Android's clipboard Binder API to the native synchronizer. */
public final class ClipboardBridge {
    private static final int SHELL_UID = 2000;
    private static final int SYSTEM_UID = 1000;
    private static final String SHELL_PACKAGE = "com.android.shell";
    private static final String SERVICE = "android.content.IClipboard";
    private static final String LISTENER = "android.content.IOnPrimaryClipChangedListener";
    private static final String SENSITIVE = "android.content.extra.IS_SENSITIVE";
    private static final String ORIGIN = "synclip.clipboard_origin";
    private static final String RECEIVE_ALERT_ACTION = "synclip.pair.action.SYNC_RECEIVED";
    private static final String RECEIVE_ALERT_COMPONENT = "synclip.pair/.SyncAlertReceiver";
    private static final int OWNER_USER = 0;
    private static final int DEFAULT_DEVICE = 0;

    private final String origin = UUID.randomUUID().toString();
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
            System.exit(new ClipboardBridge().nativeRun("--check".equals(args[1])));
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

    private void writeText(String text) throws ReflectiveOperationException {
        ClipData clip = ClipData.newPlainText("SynClip", text);
        PersistableBundle extras = new PersistableBundle();
        extras.putString(ORIGIN, origin);
        clip.getDescription().setExtras(extras);
        try {
            invokeWithItem(setPrimaryClip, clip);
        } catch (InvocationTargetException error) {
            if (error.getCause() instanceof TransactionTooLargeException) {
                System.err.println("SynClip: incoming text exceeds Android's clipboard transaction limit");
                return;
            }
            throw error;
        }
        try {
            receiveAlerts.execute(ClipboardBridge::broadcastReceiveAlert);
        } catch (SecurityException ignored) {
            // Optional alerts must not interrupt clipboard synchronization.
        }
    }

    private static void broadcastReceiveAlert() {
        java.lang.Process command = null;
        try {
            // Android 10's broadcast command has no --async. Run it off the clipboard thread with
            // at most one running broadcast and one pending alert.
            command = new ProcessBuilder("/system/bin/cmd", "activity", "broadcast", "--user",
                    Integer.toString(OWNER_USER), "-a", RECEIVE_ALERT_ACTION,
                    "-n", RECEIVE_ALERT_COMPONENT)
                    .redirectErrorStream(true)
                    .redirectOutput(new File("/dev/null"))
                    .start();
            if (!command.waitFor(3, TimeUnit.SECONDS)) {
                command.destroyForcibly();
                command.waitFor();
            }
        } catch (IOException | RuntimeException ignored) {
            // The companion may be missing, disabled, or blocked by the ROM.
        } catch (InterruptedException interrupted) {
            if (command != null) {
                command.destroyForcibly();
            }
            Thread.currentThread().interrupt();
        }
    }

    private long timestamp() {
        return lastTimestamp;
    }

    private boolean wasRemote() {
        return lastWasRemote;
    }

    private native int nativeRun(boolean check) throws ReflectiveOperationException, RemoteException;
    private static native void nativeNotify(boolean serviceLost);
}
