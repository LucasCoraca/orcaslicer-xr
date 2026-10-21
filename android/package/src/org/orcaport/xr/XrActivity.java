package org.orcaport.xr;

import android.app.Presentation;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.SurfaceTexture;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * OrcaSlicer XR: the whole OrcaSlicer app in a headset (Meta Quest 3). Orca's own interface (wxWidgets on Qt) is moved
 * off the screen onto a virtual display, whose image the headset shows as a window in front of you; the plate and the
 * objects on it stand in the room, drawn for each eye by Orca's own 3D view. The controllers are a mouse on the window
 * and point into the scene (see android/orca_xr.cpp and core/).
 *
 * Without an OpenXR runtime (phone, emulator) the same scene is shown on the screen as a touch preview, so everything
 * can be tried without a headset.
 */
public class XrActivity extends org.qtproject.qt.android.bindings.QtActivity {
    static final String TAG = "orcaxr";
    /** The window's size (dp) and density: 2 px per dp, sharper than the headset's own pixels. */
    static final int DP_W = 1600, DP_H = 1000;
    static final float PX_PER_DP = 2.0f;
    static final int PX_W = (int) (DP_W * PX_PER_DP), PX_H = (int) (DP_H * PX_PER_DP);

    private View qtRoot;
    private VirtualDisplay display;
    private Presentation presentation;
    private SurfaceTexture panelTexture;
    private volatile boolean panelFrame = false;
    private boolean attached = false;

    static native void nativeCreate(XrActivity activity, int w, int h, float pxPerDp);
    static native void nativeDestroy();
    static native void nativePreviewSurface(Surface surface, int w, int h);
    static native void nativePreviewTouch(int action, float x, float y);
    static native void nativePreviewButton(int button, boolean down);

    private boolean previewing = false;

    /** In the preview, a gamepad (or adb's key events) stands in for the controller's buttons: B / Y right click, A menu,
     *  the D-pad the thumbstick, L1 the grip. */
    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent e) {
        if (previewing) {
            int b = -1;
            switch (e.getKeyCode()) {
                case android.view.KeyEvent.KEYCODE_BUTTON_B: case android.view.KeyEvent.KEYCODE_BUTTON_Y: b = 0; break;
                case android.view.KeyEvent.KEYCODE_BUTTON_A: case android.view.KeyEvent.KEYCODE_BUTTON_X: b = 1; break;
                case android.view.KeyEvent.KEYCODE_DPAD_UP: b = 2; break;
                case android.view.KeyEvent.KEYCODE_DPAD_DOWN: b = 3; break;
                case android.view.KeyEvent.KEYCODE_DPAD_LEFT: b = 4; break;
                case android.view.KeyEvent.KEYCODE_DPAD_RIGHT: b = 5; break;
                case android.view.KeyEvent.KEYCODE_BUTTON_L1: b = 6; break;
            }
            if (b >= 0) {
                if (e.getAction() == android.view.KeyEvent.ACTION_DOWN || e.getAction() == android.view.KeyEvent.ACTION_UP)
                    nativePreviewButton(b, e.getAction() == android.view.KeyEvent.ACTION_DOWN);
                return true;
            }
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        // Qt reads the density from the activity's resources: that of the virtual display the interface is drawn on
        Configuration c = new Configuration();
        c.densityDpi = (int) (160 * PX_PER_DP);
        c.screenWidthDp = DP_W; c.screenHeightDp = DP_H; c.smallestScreenWidthDp = DP_H;
        c.orientation = Configuration.ORIENTATION_LANDSCAPE;
        applyOverrideConfiguration(c);
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        // Qt's shader binary disk cache crashed saving a program binary (window composition on a virtual display)
        try {
            android.system.Os.setenv("QT_DISABLE_SHADER_DISK_CACHE", "1", true);
            // the controller presses text fields as a finger (see send()): no touch selection handles and their menu
            android.system.Os.setenv("QT_QPA_NO_TEXT_HANDLES", "1", true);
        } catch (Exception e) { Log.w(TAG, "env", e); }
        super.onCreate(savedInstanceState);
        nativeCreate(this, PX_W, PX_H, PX_PER_DP);
    }

    @Override
    protected void onDestroy() {
        nativeDestroy();
        if (presentation != null) presentation.dismiss();
        if (display != null) display.release();
        super.onDestroy();
    }

    /** Qt puts its root layout (every Qt window, dialog and menu) here: it goes onto the virtual display instead. */
    @Override
    public void setContentView(View view, ViewGroup.LayoutParams params) {
        if (view == qtRoot) return;                             // (already on the panel)
        if (qtRoot == null && view.getClass().getName().endsWith(".QtRootLayout")) {
            qtRoot = view;
            showOnPanel();
            return;
        }
        super.setContentView(view, params);
    }

    /** Qt sizes its screen by this activity's window manager: for Qt, that of the virtual display (so its windows are
     *  made for it from the start); the framework keeps the activity's own. */
    @Override
    public WindowManager getWindowManager() {
        if (presentation != null) {
            StackTraceElement[] st = Thread.currentThread().getStackTrace();   // [0] getStackTrace, [1] here, [2] the caller
            if (st.length > 2 && st[2].getClassName().equals("org.qtproject.qt.android.QtDisplayManager")
                && st[2].getMethodName().equals("setApplicationDisplayMetrics"))
                return (WindowManager) presentation.getContext().getSystemService(Context.WINDOW_SERVICE);
        }
        return super.getWindowManager();
    }

    private void showOnPanel() {
        panelTexture = new SurfaceTexture(false);              // detached: the headset's GL context takes it (attachPanel)
        panelTexture.setDefaultBufferSize(PX_W, PX_H);
        panelTexture.setOnFrameAvailableListener(t -> panelFrame = true);
        DisplayManager dm = getSystemService(DisplayManager.class);
        display = dm.createVirtualDisplay("OrcaSlicer XR", PX_W, PX_H, (int) (160 * PX_PER_DP), new Surface(panelTexture),
                                          DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY);
        presentation = new Presentation(this, display.getDisplay());
        presentation.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.BLACK));
        // the headset's keyboard types into Qt directly: the system's input method (on a Quest, its own keyboard) is
        // never attached to Orca's text fields (it would hold what is typed in a composition, committed only on leaving)
        presentation.getWindow().addFlags(WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM);
        presentation.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM);
        presentation.setContentView(qtRoot, new ViewGroup.LayoutParams(PX_W, PX_H));
        // Orca's web pages (Home, the setup wizard) go there too
        org.orcaport.wx.WebViewBridge.contentRoot = (FrameLayout) presentation.findViewById(android.R.id.content);
        // the screen itself: a note, until the headset takes over (or the preview, without one)
        TextView t = new TextView(this);
        t.setBackgroundColor(Color.rgb(24, 27, 31)); t.setTextColor(Color.WHITE); t.setTextSize(20); t.setGravity(android.view.Gravity.CENTER);
        t.setText("OrcaSlicer XR\nPut on your headset");
        super.setContentView(t, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // shown once the activity's window is laid out: Qt reads its insets when its layout gets a size
        View decor = getWindow().getDecorView();
        decor.getViewTreeObserver().addOnGlobalLayoutListener(new android.view.ViewTreeObserver.OnGlobalLayoutListener() {
            @Override public void onGlobalLayout() {
                if (decor.getRootWindowInsets() == null || presentation.isShowing()) return;
                decor.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                presentation.show();
                decor.postDelayed(viewDumper, 2000);
            }
        });
        // Qt takes its screen's available area from the activity's window insets: here there are none
        qtRoot.addOnLayoutChangeListener((v, l, tp, r, b, ol, ot, or, ob) -> v.post(this::fixScreenMetrics));
    }

    private void fixScreenMetrics() {
        try {
            Class<?> c = Class.forName("org.qtproject.qt.android.QtDisplayManager");
            java.lang.reflect.Method m = c.getDeclaredMethod("setDisplayMetrics", int.class, int.class, int.class, int.class, int.class, int.class,
                                                              double.class, double.class, double.class, double.class, float.class);
            m.setAccessible(true);
            double dpi = 160 * PX_PER_DP;
            m.invoke(null, PX_W, PX_H, 0, 0, PX_W, PX_H, dpi, dpi, (double) PX_PER_DP, (double) PX_PER_DP, 60f);
        } catch (Exception e) { Log.w(TAG, "screen metrics", e); }
    }

    // ---------------------------------------------------------------- called from the XR loop (Qt's GUI thread, GL current)
    /** The window's image becomes texture `tex` (GL_TEXTURE_EXTERNAL_OES) of the current context. */
    @SuppressWarnings("unused")
    void attachPanel(int tex) {
        if (attached) panelTexture.detachFromGLContext();
        panelTexture.attachToGLContext(tex); attached = true;
    }

    /** Latches the newest image of the window; false when there is none. */
    @SuppressWarnings("unused")
    boolean updatePanel() {
        if (!panelFrame || !attached) return false;
        panelFrame = false;
        try { panelTexture.updateTexImage(); } catch (Exception e) { Log.w(TAG, "panel", e); return false; }
        return true;
    }

    private long downTime = 0;
    private int buttons = 0;
    /** The controller on the window, as a mouse: 0 press 1 drag 2 release 3 hover 4 leave 5 wheel 6 right click (pixels of
     *  the virtual display). Dispatched into the window's view tree, so Qt's windows and the web views over them (Home,
     *  the setup wizard) get it alike. */
    @SuppressWarnings("unused")
    void panelMouse(int action, float x, float y, float amount) {
        runOnUiThread(() -> {
            if (presentation == null || presentation.getWindow() == null) return;
            View root = presentation.getWindow().getDecorView();
            long now = android.os.SystemClock.uptimeMillis();
            switch (action) {
                case 0: downTime = now; buttons = MotionEvent.BUTTON_PRIMARY; send(root, MotionEvent.ACTION_DOWN, x, y, 0, now, true); break;
                case 1: send(root, MotionEvent.ACTION_MOVE, x, y, 0, now, true); break;
                case 2: buttons = 0; send(root, MotionEvent.ACTION_UP, x, y, 0, now, true); break;
                case 3: send(root, MotionEvent.ACTION_HOVER_MOVE, x, y, 0, now, false); break;
                case 4: send(root, MotionEvent.ACTION_HOVER_EXIT, x, y, 0, now, false); break;
                case 5: send(root, MotionEvent.ACTION_SCROLL, x, y, amount, now, false); break;
                case 6:
                    downTime = now; buttons = MotionEvent.BUTTON_SECONDARY;
                    send(root, MotionEvent.ACTION_DOWN, x, y, 0, now, true);
                    buttons = 0; send(root, MotionEvent.ACTION_UP, x, y, 0, now, true);
                    break;
            }
        });
    }

    private boolean fingerPress = false;

    private void send(View root, int action, float x, float y, float vscroll, long now, boolean touch) {
        // the left button is a finger: web views take no mouse clicks dispatched this way, and Qt routes them to its main
        // window but not to its popups (menus); a touch it turns into the same mouse press everywhere. Hover, the wheel
        // and the right button are mouse events.
        if (action == MotionEvent.ACTION_DOWN) fingerPress = buttons == MotionEvent.BUTTON_PRIMARY;
        boolean finger = touch && fingerPress;
        MotionEvent.PointerProperties p = new MotionEvent.PointerProperties(); p.id = 0;
        p.toolType = finger ? MotionEvent.TOOL_TYPE_FINGER : MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords c = new MotionEvent.PointerCoords(); c.x = x; c.y = y; c.pressure = touch ? 1f : 0f;
        if (vscroll != 0) c.setAxisValue(MotionEvent.AXIS_VSCROLL, vscroll);
        MotionEvent ev = MotionEvent.obtain(touch ? downTime : now, now, action, 1, new MotionEvent.PointerProperties[] {p},
                                            new MotionEvent.PointerCoords[] {c}, 0, finger ? 0 : buttons, 1f, 1f, 0, 0,
                                            finger ? android.view.InputDevice.SOURCE_TOUCHSCREEN : android.view.InputDevice.SOURCE_MOUSE, 0);
        if (touch) root.dispatchTouchEvent(ev); else root.dispatchGenericMotionEvent(ev);
        ev.recycle();
    }

    /** Development: `adb shell run-as org.orcaport.slicer.xr touch files/dump_views` logs the window's view tree. */
    private final Runnable viewDumper = new Runnable() {
        @Override public void run() {
            java.io.File f = new java.io.File(getFilesDir(), "dump_views");
            if (f.exists() && f.delete() && presentation != null) dumpViews(presentation.getWindow().getDecorView(), "");
            if (presentation != null) presentation.getWindow().getDecorView().postDelayed(this, 2000);
        }
    };
    private void dumpViews(View v, String ind) {
        int[] o = new int[2]; v.getLocationOnScreen(o);
        Log.i(TAG, ind + v.getClass().getSimpleName() + " " + o[0] + "," + o[1] + " " + v.getWidth() + "x" + v.getHeight()
                   + (v.getVisibility() != View.VISIBLE ? " hidden" : "") + (v.isAttachedToWindow() ? "" : " detached"));
        if (v instanceof ViewGroup) { ViewGroup g = (ViewGroup) v; for (int i = 0; i < g.getChildCount(); ++i) dumpViews(g.getChildAt(i), ind + "  "); }
    }

    @SuppressWarnings("unused")
    void onXrResult(boolean ok, String message) {
        Log.i(TAG, "OpenXR: " + (ok ? "running on " + message : "unavailable (" + message + ")"));
        if (!ok) runOnUiThread(() -> showPreview(message));
    }

    @SuppressWarnings("unused")
    void onXrExit() { runOnUiThread(this::finish); }

    // ---------------------------------------------------------------- no headset: a touch preview of the XR scene
    private void showPreview(String reason) {
        previewing = true;
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { }
            @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hh) { nativePreviewSurface(h.getSurface(), w, hh); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { nativePreviewSurface(null, 0, 0); }
        });
        // one finger: the controller with its trigger pressed; two fingers: look around
        view.setOnTouchListener((v, e) -> {
            float cx = 0, cy = 0;
            for (int i = 0; i < e.getPointerCount(); ++i) { cx += e.getX(i); cy += e.getY(i); }
            cx /= e.getPointerCount(); cy /= e.getPointerCount();
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: nativePreviewTouch(0, e.getX(), e.getY()); break;
                case MotionEvent.ACTION_POINTER_DOWN: nativePreviewTouch(4, cx, cy); break;
                case MotionEvent.ACTION_MOVE: if (e.getPointerCount() >= 2) nativePreviewTouch(3, cx, cy); else nativePreviewTouch(1, e.getX(), e.getY()); break;
                case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL: nativePreviewTouch(2, e.getX(), e.getY()); break;
            }
            return true;
        });
        TextView note = new TextView(this);
        note.setTextColor(Color.rgb(200, 208, 216)); note.setTextSize(13); note.setPadding(28, 18, 28, 18); note.setBackgroundColor(Color.argb(140, 0, 0, 0));
        note.setText("No headset (" + reason + "): preview of the XR scene. One finger is the controller (on the window it is the mouse; on the plate it picks, drags and uses the gizmos); two fingers look around.");
        FrameLayout f = new FrameLayout(this);
        f.addView(view);
        f.addView(note, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, android.view.Gravity.TOP));
        super.setContentView(f, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }
}
