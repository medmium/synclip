package synclip.pair;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/** Receives reminders from the module even when the pairing UI is closed. */
public final class SyncAlertProvider extends ContentProvider {
    private static final int ROOT_UID = 0;
    private static final int SHELL_UID = 2000;
    private static final int MAX_PREVIEW_UNITS = 480;
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override public boolean onCreate() {
        return true;
    }

    @Override public Bundle call(String method, String argument, Bundle extras) {
        final int caller = Binder.getCallingUid();
        if (caller != ROOT_UID && caller != SHELL_UID) {
            throw new SecurityException("SynClip alerts are restricted to the module");
        }
        final Context context = getContext();
        if (context == null) {
            throw new IllegalStateException("SynClip alert provider is unavailable");
        }
        // ContentProvider.call does not enforce read/write permissions on behalf of the provider.
        context.enforceCallingPermission("android.permission.DUMP", "SynClip alert sender");
        if (argument != null) {
            throw new IllegalArgumentException("Unexpected SynClip receive event argument");
        }
        final String text;
        if ("received".equals(method) && (extras == null || extras.isEmpty())) {
            text = null;
        } else if ("received_preview".equals(method) && extras != null && extras.size() == 1) {
            text = extras.getString("text");
            if (text == null || text.isEmpty() || text.length() > MAX_PREVIEW_UNITS) {
                throw new IllegalArgumentException("Invalid SynClip notification preview");
            }
        } else {
            throw new IllegalArgumentException("Unsupported SynClip receive event");
        }
        Bundle response = new Bundle();
        response.putBoolean("accepted", main.post(() -> SyncAlerts.deliver(context, text)));
        return response;
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] arguments, String order) {
        throw new UnsupportedOperationException();
    }

    @Override public String getType(Uri uri) {
        throw new UnsupportedOperationException();
    }

    @Override public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override public int delete(Uri uri, String selection, String[] arguments) {
        throw new UnsupportedOperationException();
    }

    @Override public int update(Uri uri, ContentValues values, String selection, String[] arguments) {
        throw new UnsupportedOperationException();
    }
}
