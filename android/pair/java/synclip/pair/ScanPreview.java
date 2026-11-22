package synclip.pair;

import android.app.Activity;
import android.graphics.ImageFormat;
import android.hardware.Camera;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import com.google.zxing.BinaryBitmap;
import com.google.zxing.PlanarYUVLuminanceSource;
import com.google.zxing.ReaderException;
import com.google.zxing.common.HybridBinarizer;
import com.google.zxing.qrcode.QRCodeReader;

import java.io.IOException;
import java.util.List;

@SuppressWarnings("deprecation")
final class ScanPreview implements SurfaceHolder.Callback {
    interface Listener {
        void onScan(String text);
        void onCameraFailure();
    }

    final SurfaceView view;
    private final Activity activity;
    private final Listener listener;
    private final HandlerThread cameraThread = new HandlerThread("SynClip camera");
    private final Handler cameraHandler;
    private final QRCodeReader reader = new QRCodeReader();
    private boolean requested;
    private SurfaceHolder surface;
    private volatile int generation;

    // Camera state is confined to cameraThread.
    private Camera camera;
    private long nextScan;

    ScanPreview(Activity activity, Listener listener) {
        this.activity = activity;
        this.listener = listener;
        cameraThread.start();
        cameraHandler = new Handler(cameraThread.getLooper());
        view = new SurfaceView(activity);
        view.getHolder().addCallback(this);
    }

    void start() {
        if (!requested) {
            requested = true;
            startIfReady();
        }
    }

    void stop() {
        requested = false;
        invalidateCamera();
    }

    void close() {
        view.getHolder().removeCallback(this);
        stop();
        cameraThread.quitSafely();
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        surface = holder;
        startIfReady();
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        surface = null;
        invalidateCamera();
    }

    private void invalidateCamera() {
        ++generation;
        cameraHandler.post(this::releaseCamera);
    }

    private void startIfReady() {
        if (!requested || surface == null) {
            return;
        }
        SurfaceHolder holder = surface;
        int rotation = activity.getWindowManager().getDefaultDisplay().getRotation();
        int token = ++generation;
        cameraHandler.post(() -> openCamera(holder, rotation, token));
    }

    private void openCamera(SurfaceHolder holder, int rotation, int token) {
        releaseCamera();
        if (token != generation || !holder.getSurface().isValid()) {
            return;
        }
        try {
            int cameraCount = Camera.getNumberOfCameras();
            if (cameraCount == 0) {
                cameraFailed(token);
                return;
            }
            Camera.CameraInfo info = new Camera.CameraInfo();
            int selected = 0;
            for (int id = 0; id < cameraCount; ++id) {
                Camera.getCameraInfo(id, info);
                if (info.facing == Camera.CameraInfo.CAMERA_FACING_BACK) {
                    selected = id;
                    break;
                }
            }
            Camera.getCameraInfo(selected, info);
            camera = Camera.open(selected);
            camera.setDisplayOrientation(displayOrientation(info, rotation));
            Camera.Parameters parameters = camera.getParameters();
            Camera.Size size = parameters.getPreviewSize();
            long distance = previewDistance(size);
            for (Camera.Size candidate : parameters.getSupportedPreviewSizes()) {
                long candidateDistance = previewDistance(candidate);
                if (candidateDistance < distance) {
                    size = candidate;
                    distance = candidateDistance;
                }
            }
            parameters.setPreviewSize(size.width, size.height);
            parameters.setPreviewFormat(ImageFormat.NV21);
            List<String> focus = parameters.getSupportedFocusModes();
            if (focus != null && focus.contains(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE)) {
                parameters.setFocusMode(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE);
            }
            camera.setParameters(parameters);
            camera.setPreviewDisplay(holder);
            int width = size.width;
            int height = size.height;
            nextScan = 0;
            camera.setErrorCallback((error, source) -> cameraFailed(token));
            camera.setPreviewCallback((data, source) -> decode(data, width, height, token));
            camera.startPreview();
            if (Camera.Parameters.FOCUS_MODE_AUTO.equals(parameters.getFocusMode())) {
                camera.autoFocus(null);
            }
        } catch (RuntimeException | IOException error) {
            cameraFailed(token);
        }
    }

    private static int displayOrientation(Camera.CameraInfo info, int rotation) {
        int degrees = rotation == Surface.ROTATION_90 ? 90 : rotation == Surface.ROTATION_180 ? 180
                : rotation == Surface.ROTATION_270 ? 270 : 0;
        if (info.facing == Camera.CameraInfo.CAMERA_FACING_FRONT) {
            return (360 - (info.orientation + degrees) % 360) % 360;
        }
        return (info.orientation - degrees + 360) % 360;
    }

    private static long previewDistance(Camera.Size size) {
        return Math.abs((long) size.width * size.height - 1280L * 720L);
    }

    private void decode(byte[] data, int width, int height, int token) {
        long now = SystemClock.elapsedRealtime();
        if (token != generation || now < nextScan || data == null || data.length < width * height) {
            return;
        }
        nextScan = now + 120;
        try {
            PlanarYUVLuminanceSource source = new PlanarYUVLuminanceSource(data, width, height,
                    0, 0, width, height, false);
            String text = reader.decode(new BinaryBitmap(new HybridBinarizer(source))).getText();
            nextScan = now + 600;
            activity.runOnUiThread(() -> {
                if (token == generation) {
                    listener.onScan(text);
                }
            });
        } catch (ReaderException ignored) {
            // A preview image usually contains no readable QR code.
        }
    }

    private void cameraFailed(int token) {
        if (token == generation) {
            releaseCamera();
            activity.runOnUiThread(() -> {
                if (token == generation) {
                    listener.onCameraFailure();
                }
            });
        }
    }

    private void releaseCamera() {
        if (camera != null) {
            try {
                camera.setPreviewCallback(null);
                camera.stopPreview();
            } catch (RuntimeException ignored) {
                // The preview surface may already have been destroyed.
            } finally {
                camera.release();
                camera = null;
            }
        }
    }
}
