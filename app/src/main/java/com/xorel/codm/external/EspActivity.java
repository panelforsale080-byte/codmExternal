package com.xorel.codm.external;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.ContextCompat;

/** Launcher — overlay permission, then floating UI for CODM (no license gate). */
public class EspActivity extends Activity {
    private static final int REQ_NOTIF = 1001;
    private static final String CODM_PACKAGE = "com.garena.game.codm";

    private boolean pendingOverlay;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_xorel);

        Button btnOverlay = findViewById(R.id.btnOverlay);
        Button btnStart = findViewById(R.id.btnStart);
        Button btnGame = findViewById(R.id.btnGame);

        btnOverlay.setOnClickListener(v -> openOverlaySettings());
        btnStart.setOnClickListener(v -> tryStartOverlay());
        btnGame.setOnClickListener(v -> openCodm());

        refreshUiState();
    }

    private void refreshUiState() {
        boolean overlayOk =
                Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this);
        boolean notifOk =
                Build.VERSION.SDK_INT < 33
                        || ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                                == PackageManager.PERMISSION_GRANTED;
        Button btnStart = findViewById(R.id.btnStart);
        btnStart.setEnabled(overlayOk && notifOk);
        btnStart.setAlpha(btnStart.isEnabled() ? 1f : 0.45f);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (pendingOverlay && canOverlay()) {
            pendingOverlay = false;
            startOverlay();
        }
        refreshUiState();
    }

    private void openOverlaySettings() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            pendingOverlay = true;
            startActivity(
                    new Intent(
                            Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                            Uri.parse("package:" + getPackageName())));
        }
    }

    private void tryStartOverlay() {
        if (!canOverlay()) {
            openOverlaySettings();
            Toast.makeText(this, R.string.toast_need_overlay, Toast.LENGTH_LONG).show();
            return;
        }
        if (Build.VERSION.SDK_INT >= 33) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIF);
                pendingOverlay = true;
                return;
            }
        }
        startOverlay();
    }

    private void startOverlay() {
        if (OverlayService.instance != null) {
            Toast.makeText(this, R.string.overlay_running, Toast.LENGTH_SHORT).show();
            moveTaskToBack(true);
            return;
        }
        try {
            Intent svc = new Intent(this, OverlayService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc);
            else startService(svc);
            Toast.makeText(this, R.string.toast_overlay_started, Toast.LENGTH_LONG).show();
            moveTaskToBack(true);
        } catch (Throwable t) {
            Toast.makeText(this, t.getMessage() != null ? t.getMessage() : "Failed", Toast.LENGTH_LONG).show();
        }
    }

    private void openCodm() {
        try {
            Intent launch = getPackageManager().getLaunchIntentForPackage(CODM_PACKAGE);
            if (launch != null) {
                launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                startActivity(launch);
            } else {
                Toast.makeText(this, "CODM not installed", Toast.LENGTH_LONG).show();
            }
        } catch (Throwable t) {
            Toast.makeText(this, "Cannot open game", Toast.LENGTH_SHORT).show();
        }
    }

    private boolean canOverlay() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code == REQ_NOTIF && pendingOverlay) tryStartOverlay();
        refreshUiState();
    }
}
