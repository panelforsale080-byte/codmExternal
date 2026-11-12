package com.xorel.codm.external;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.Toast;

public class OverlayService extends Service {
    private static final String TAG = "CodmOverlay";
    public static OverlayService instance;

    private WindowManager wm;
    private FrameLayout root;
    private GlImGuiView glView;
    private ImageButton fabLogo;
    private WindowManager.LayoutParams surfaceLp;
    private WindowManager.LayoutParams fabLp;
    private boolean menuInteractive;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile boolean glReady;

    private static final int SURFACE_FLAGS_PASS =
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_SPLIT_TOUCH;
    private static final int SURFACE_FLAGS_MENU =
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                    | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;

    private float fabTouchLastRawX;
    private float fabTouchLastRawY;
    private float fabTouchDownRawX;
    private float fabTouchDownRawY;
    private boolean fabDragging;
    private boolean fabHoldExitTriggered;
    private Runnable fabHoldExitTask;
    private static final long FAB_HOLD_EXIT_MS = 700L;
    private DisplayManager displayManager;
    private DisplayManager.DisplayListener displayListener;

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            stopSelf();
            return;
        }
        try {
            NativeBridge.nativeInit();
            startForegroundNotif();
            wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            attachWindows();
            registerDisplayListener();
        } catch (Throwable t) {
            Log.e(TAG, "onCreate", t);
            stopSelf();
        }
    }

    void postToast(String msg) {
        main.post(() -> Toast.makeText(getApplicationContext(), msg, Toast.LENGTH_SHORT).show());
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void getScreenSize(int[] out) {
        DisplayMetrics dm = new DisplayMetrics();
        wm.getDefaultDisplay().getRealMetrics(dm);
        out[0] = Math.max(dm.widthPixels, 1);
        out[1] = Math.max(dm.heightPixels, 1);
    }

    private void registerDisplayListener() {
        displayManager = (DisplayManager) getSystemService(DISPLAY_SERVICE);
        if (displayManager == null) return;
        displayListener =
                new DisplayManager.DisplayListener() {
                    @Override
                    public void onDisplayAdded(int displayId) {}

                    @Override
                    public void onDisplayRemoved(int displayId) {}

                    @Override
                    public void onDisplayChanged(int displayId) {
                        if (displayId == Display.DEFAULT_DISPLAY) {
                            main.post(OverlayService.this::applyDisplayGeometry);
                        }
                    }
                };
        displayManager.registerDisplayListener(displayListener, main);
    }

    void applyDisplayGeometry() {
        if (wm == null || glView == null) return;
        if (surfaceLp != null && root != null) {
            surfaceLp.width = WindowManager.LayoutParams.MATCH_PARENT;
            surfaceLp.height = WindowManager.LayoutParams.MATCH_PARENT;
            try {
                wm.updateViewLayout(root, surfaceLp);
            } catch (Throwable t) {
                Log.w(TAG, "geometry", t);
            }
        }
        if (fabLp != null && fabLogo != null) {
            clampFabToScreen();
            try {
                wm.updateViewLayout(fabLogo, fabLp);
            } catch (Throwable ignored) {
            }
        }
        glView.requestLayout();
        glView.requestRender();
    }

    private void clampFabToScreen() {
        if (fabLp == null || fabLogo == null || wm == null) return;
        int[] wh = new int[2];
        getScreenSize(wh);
        int fw = fabLogo.getWidth() > 0 ? fabLogo.getWidth() : dp(56);
        int fh = fabLogo.getHeight() > 0 ? fabLogo.getHeight() : dp(56);
        if (fabLp.x < 0) fabLp.x = 0;
        if (fabLp.y < 0) fabLp.y = 0;
        if (fabLp.x > wh[0] - fw) fabLp.x = Math.max(0, wh[0] - fw);
        if (fabLp.y > wh[1] - fh) fabLp.y = Math.max(0, wh[1] - fh);
    }

    private void attachWindows() {
        root = new OverlayRootLayout(this);
        glView = new GlImGuiView(this);
        glView.configureOverlaySurface();
        glView.setOnGlReadyListener(
                v ->
                        main.post(
                                () -> {
                                    glReady = true;
                                    try {
                                        NativeBridge.nativeMinimizeMenu();
                                    } catch (Throwable t) {
                                        Log.w(TAG, "minimize", t);
                                    }
                                    Log.i(TAG, "GL ready — tap icon for menu");
                                }));
        root.addView(
                glView,
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        final int overlayType =
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                        ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                        : WindowManager.LayoutParams.TYPE_PHONE;

        surfaceLp =
                new WindowManager.LayoutParams(
                        WindowManager.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.MATCH_PARENT,
                        overlayType,
                        SURFACE_FLAGS_PASS,
                        PixelFormat.TRANSLUCENT);
        surfaceLp.gravity = Gravity.TOP | Gravity.START;

        fabLogo = new ImageButton(this);
        fabLogo.setBackgroundResource(R.drawable.bg_fab_xorel);
        fabLogo.setImageResource(R.drawable.ic_xorel_logo);
        fabLogo.setImageTintList(null);
        fabLogo.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
        fabLogo.setAdjustViewBounds(true);
        fabLogo.setPadding(dp(8), dp(8), dp(8), dp(8));
        fabLogo.setElevation(dp(6));
        fabLogo.setContentDescription(getString(R.string.fab_open_menu));
        int fabSize = dp(56);
        fabLogo.setMinimumWidth(fabSize);
        fabLogo.setMinimumHeight(fabSize);

        fabLp =
                new WindowManager.LayoutParams(
                        fabSize,
                        fabSize,
                        overlayType,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
        fabLp.gravity = Gravity.TOP | Gravity.START;
        int[] wh = new int[2];
        getScreenSize(wh);
        fabLp.x = wh[0] - fabSize - dp(16);
        fabLp.y = dp(52);

        final int dragSlopPx = dp(10);
        fabLogo.setOnTouchListener(
                (v, event) -> {
                    switch (event.getActionMasked()) {
                        case MotionEvent.ACTION_DOWN:
                            fabTouchDownRawX = event.getRawX();
                            fabTouchDownRawY = event.getRawY();
                            fabTouchLastRawX = fabTouchDownRawX;
                            fabTouchLastRawY = fabTouchDownRawY;
                            fabDragging = false;
                            fabHoldExitTriggered = false;
                            scheduleFabHoldExit();
                            return true;
                        case MotionEvent.ACTION_MOVE:
                            float dx = event.getRawX() - fabTouchLastRawX;
                            float dy = event.getRawY() - fabTouchLastRawY;
                            fabTouchLastRawX = event.getRawX();
                            fabTouchLastRawY = event.getRawY();
                            float dist =
                                    (float)
                                            Math.hypot(
                                                    event.getRawX() - fabTouchDownRawX,
                                                    event.getRawY() - fabTouchDownRawY);
                            if (dist > dragSlopPx) {
                                fabDragging = true;
                                cancelFabHoldExit();
                            }
                            fabLp.x += (int) dx;
                            fabLp.y += (int) dy;
                            clampFabToScreen();
                            try {
                                wm.updateViewLayout(fabLogo, fabLp);
                            } catch (Throwable ignored) {
                            }
                            return fabDragging;
                        case MotionEvent.ACTION_UP:
                            cancelFabHoldExit();
                            if (fabHoldExitTriggered) return true;
                            if (!fabDragging) {
                                applyDisplayGeometry();
                                if (glView != null) {
                                    glView.queueEvent(NativeBridge::nativeOpenMenu);
                                    bumpGlFrames();
                                }
                            }
                            return true;
                        case MotionEvent.ACTION_CANCEL:
                            cancelFabHoldExit();
                            return true;
                        default:
                            return false;
                    }
                });

        root.setClickable(false);
        root.setFocusable(false);
        glView.setClickable(false);
        glView.setFocusable(false);

        wm.addView(root, surfaceLp);
        wm.addView(fabLogo, fabLp);
        menuInteractive = false;
        applyMenuTouchFlags(false);
    }

    private void scheduleFabHoldExit() {
        cancelFabHoldExit();
        fabHoldExitTask =
                () -> {
                    fabHoldExitTriggered = true;
                    fabDragging = true;
                    try {
                        NativeBridge.nativeStopEsp();
                        NativeBridge.nativeMinimizeMenu();
                    } catch (Throwable t) {
                        Log.w(TAG, "hold exit", t);
                    }
                    stopSelf();
                };
        main.postDelayed(fabHoldExitTask, FAB_HOLD_EXIT_MS);
    }

    private void cancelFabHoldExit() {
        if (fabHoldExitTask != null) {
            main.removeCallbacks(fabHoldExitTask);
            fabHoldExitTask = null;
        }
    }

    boolean isMenuInteractive() {
        return menuInteractive;
    }

    void setMenuTouch(boolean interactive) {
        if (Looper.myLooper() == Looper.getMainLooper()) applyMenuTouchFlags(interactive);
        else main.post(() -> applyMenuTouchFlags(interactive));
    }

    private void bumpGlFrames() {
        if (glView == null) return;
        glView.requestRender();
        main.post(glView::requestRender);
        main.postDelayed(glView::requestRender, 32);
        main.postDelayed(glView::requestRender, 96);
        main.postDelayed(glView::requestRender, 160);
    }

    private void applyMenuTouchFlags(boolean interactive) {
        if (surfaceLp == null || wm == null || root == null) return;
        menuInteractive = interactive;
        if (!interactive) {
            try {
                if (NativeBridge.nativeIsMenuInteractive()) NativeBridge.nativeMinimizeMenu();
            } catch (Throwable t) {
                Log.w(TAG, "minimize", t);
            }
        }
        surfaceLp.flags = interactive ? SURFACE_FLAGS_MENU : SURFACE_FLAGS_PASS;
        root.setClickable(interactive);
        root.setFocusable(interactive);
        root.setFocusableInTouchMode(interactive);
        if (glView != null) {
            glView.setClickable(interactive);
            glView.setFocusable(interactive);
            glView.setFocusableInTouchMode(interactive);
            glView.setRenderMode(
                    interactive
                            ? android.opengl.GLSurfaceView.RENDERMODE_CONTINUOUSLY
                            : android.opengl.GLSurfaceView.RENDERMODE_WHEN_DIRTY);
        }
        try {
            wm.updateViewLayout(root, surfaceLp);
        } catch (Throwable t) {
            Log.w(TAG, "touch flags", t);
        }
        if (fabLogo != null) {
            fabLogo.setVisibility(interactive ? View.GONE : View.VISIBLE);
            if (!interactive && fabLp != null) {
                clampFabToScreen();
                try {
                    wm.updateViewLayout(fabLogo, fabLp);
                } catch (Throwable ignored) {
                }
            }
        }
        if (glView != null) {
            if (interactive) bumpGlFrames();
            else glView.requestRender();
        }
    }

    private void startForegroundNotif() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                    new NotificationChannel(
                            "codm", getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW));
            Notification n =
                    new Notification.Builder(this, "codm")
                            .setContentTitle(getString(R.string.notif_title))
                            .setContentText(getString(R.string.notif_text))
                            .setSmallIcon(android.R.drawable.ic_menu_compass)
                            .build();
            try {
                if (Build.VERSION.SDK_INT >= 34) {
                    startForeground(7, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
                } else {
                    startForeground(7, n);
                }
            } catch (Throwable t) {
                startForeground(7, n);
            }
        }
    }

    @Override
    public void onDestroy() {
        cancelFabHoldExit();
        if (displayManager != null && displayListener != null) {
            try {
                displayManager.unregisterDisplayListener(displayListener);
            } catch (Throwable ignored) {
            }
        }
        try {
            if (glView != null) glView.releaseGl();
        } catch (Throwable t) {
            Log.e(TAG, "releaseGl", t);
        }
        try {
            if (fabLogo != null) wm.removeView(fabLogo);
        } catch (Throwable ignored) {
        }
        try {
            if (root != null) wm.removeView(root);
        } catch (Throwable ignored) {
        }
        instance = null;
        super.onDestroy();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
