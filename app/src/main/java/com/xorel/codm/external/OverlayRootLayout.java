package com.xorel.codm.external;

import android.content.Context;
import android.view.MotionEvent;
import android.widget.FrameLayout;

final class OverlayRootLayout extends FrameLayout {
    OverlayRootLayout(Context context) {
        super(context);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        OverlayService svc = OverlayService.instance;
        if (svc == null || !svc.isMenuInteractive()) return false;
        return super.dispatchTouchEvent(ev);
    }
}
