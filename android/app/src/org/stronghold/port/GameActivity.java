package org.stronghold.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.drawable.GradientDrawable;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Environment;
import android.os.SystemClock;
import android.text.InputFilter;
import android.text.InputType;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.util.HashSet;

public abstract class GameActivity extends Activity implements SurfaceHolder.Callback, InputManager.InputDeviceListener {
    private static String loadedEdition;
    protected abstract String gameEdition();
    private static native boolean nativeStart(String game, String data, String images, String marker, int width, int height, boolean verbose, boolean skipMovies);
    private static native void nativeSurface(Object surface);
    private static native void nativeKey(int scan, boolean down);
    private static native int nativeUiEvent();
    private static native int[] nativeSettings();
    private static native void nativeSetting(int index, int value);
    private static native String[] nativeEditor();
    private static native void nativeEditorRequest();
    private static native boolean nativeEdit(String text, int token);
    private static native void nativeMouseMove(float x, float y, boolean relative);
    private static native void nativeMouseWarp(float x, float y);
    private static native void nativeMouseButton(int button, boolean down);
    private static native void nativeMouseEdges(int edges);
    private static native void nativePause(boolean paused);
    private static native void nativeRenderScale(float scale);
    private static native void nativeFilters(int world, int ui, float sharpness, boolean deband, boolean crispText);
    private static native void nativeMagnifier(boolean active, float x, float y, int size, int zoom);
    private static native int nativeFeedback();
    private static native float[] nativePerfStats(boolean reset);
    private static native boolean nativeTouchDesktop();
    private static native float nativeWorldZoom();
    private static native float[] nativeHudPoint(float x, float y);
    private static native int[] nativeControlState();
    private static native void nativeCommand(int key, int modifiers);
    private static native void nativeTouch(int action, int sequence, float x, float y, float dx, float dy, float scale);
    private static native int nativeTouchStatus(int sequence);
    /* Elements the player may drag in the layout editor; positions are view fractions. */
    private static final String[] MOVABLE = {"tools", "mouse", "switch", "wheel1", "wheel2"};
    /* stronghold_feedback() bits (android/host/feedback.h). */
    private static final int FEEDBACK_ZOOM_SNAP = 1, FEEDBACK_SECTION = 2, FEEDBACK_PLACE = 4;
    /* `setprop log.tag.StrongholdInput DEBUG` traces mouse mode, haptics and the loupe. */
    private static final String TAG = "StrongholdInput";
    private final HashSet<Integer> held = new HashSet<>(), pressed = new HashSet<>(), padKeys = new HashSet<>();
    private boolean ready, started, touchpad, wide, extra, diagnostics, groupWheel, markerWheel;
    private boolean vibration, loupe, deband, crispText, mouseMode, magnifierShown, feedbackPosted, debugInput;
    private int[] controlState = new int[10];
    private int wheel, wheelSector = -1;
    private float wheelX, wheelY, wheelRadius, wheelDownX, wheelDownY;
    private boolean wheelDragged, wheelPinned, wheelAssign;
    private int uiScale, controlsScale, requestedWidth, requestedHeight, worldFilter, uiFilter, sentButtons;
    private int loupeSize, loupeZoom;
    private float sensitivity, opacity, sharpness, magnifierX, magnifierY, mouseX, mouseY;
    private long mouseModeAt, secondaryAt, feedbackUntil, perfAt;
    private boolean perfMatch;
    private String autoPlusToast;
    private int pointerStyle = -1, pointerSize;
    private final java.util.ArrayDeque<int[]> mouseEdges = new java.util.ArrayDeque<>();
    private HardwareInput.Mouse mouse;
    private GamepadControls gamepad;
    private boolean gamepadMode, gamepadConnected;
    private int gamepadDevice = -1;
    private TextView gamepadHint;
    private View gamepadClick;
    private AlertDialog gamepadHelp;
    private InputManager inputManager;
    private final Runnable gamepadFrame = new Runnable() {
        @Override public void run() {
            if (!started || !gamepadMode || !hasWindowFocus()) return;
            gamepad.tick(SystemClock.uptimeMillis());
            touch.invalidate();
            if (gamepad.running()) root.postOnAnimation(this);
        }
    };
    private String edition;
    private String gamePath;
    private boolean skipMovies, rememberedSuccess;
    private SharedPreferences preferences;
    private FrameLayout root;
    private LinearLayout tools, mouseTools, editorBar;
    private TextView zoomIndicator, speedValue, switchButton, hint;
    private LayoutEditor layoutEditor;
    private int displayedZoom = -1, displayedSpeed = -1;
    private boolean textDialog, layoutEdit;
    /* Other installed games this one can switch to, found once in the background. */
    private java.util.List<String> otherEditions = new java.util.ArrayList<>();
    private android.net.wifi.WifiManager.MulticastLock lanDiscovery;
    private PointerView touch;
    private final Runnable wheelHold=()->{
        if(wheel!=0&&!wheelPinned){wheelAssign=wheel==2;haptic(HapticFeedbackConstants.LONG_PRESS);touch.invalidate();}
    };
    private TouchControls gestures;
    private SemanticTouchControls semantic;
    private MouseGate mouseGate;
    private final Runnable longPress = () -> {
        if (touchpad) gestures.longPress(SystemClock.uptimeMillis());
        else semantic.hold(SystemClock.uptimeMillis());
        updateMagnifier();
        if (touch != null) touch.invalidate();
    };
    private final Runnable classification = new Runnable() {
        @Override public void run() {
            if (!started || semantic == null || !semantic.awaitingClassification()) return;
            semantic.classified(semantic.sequence(), nativeTouchStatus(semantic.sequence()));
            semantic.hold(SystemClock.uptimeMillis());
            updateMagnifier();
            touch.invalidate();
            if (semantic.awaitingClassification()) root.postOnAnimation(this);
        }
    };
    private final Runnable magnifierDue = this::updateMagnifier;
    private int exitEdges;
    private final Runnable edgeExit = () -> { if (mouseMode) nativeMouseEdges(exitEdges); };
    /* Engine cues arrive a game frame after the touch that caused them. */
    private final Runnable feedbackPoll = new Runnable() {
        @Override public void run() {
            feedback();
            if (SystemClock.uptimeMillis() < feedbackUntil) root.postOnAnimation(this); else feedbackPosted = false;
        }
    };
    /* Mouse buttons wait until the game thread has taken the desktop path:
     * an edge seen half in direct-touch mode could become a stray click. */
    private final Runnable mouseEdgeFlush = new Runnable() {
        @Override public void run() {
            if (mouseEdges.isEmpty()) return;
            if (!nativeTouchDesktop() && SystemClock.uptimeMillis() - mouseModeAt < 250) { root.postOnAnimation(this); return; }
            while (!mouseEdges.isEmpty()) { int[] edge = mouseEdges.poll(); sendMouseButton(edge[0], edge[1] != 0); }
        }
    };
    private final Runnable zoomRefresh = new Runnable() {
        @Override public void run() {
            updateZoomIndicator();
            if (started) {
                feedback();
                int event = nativeUiEvent();
                if ((event & 1) != 0) settings((event & UI_SETTINGS_GRAPHICS) != 0 ? "graphics" : (event & UI_SETTINGS_AUDIO) != 0 ? "audio" : "game");
                else if ((event & 2) != 0) showEditor();
                if((event&4)!=0) { showTools(!extra);rebuildTools(); }
                if((event&8)!=0) {
                    releaseControls();nativePause(true);
                    new AlertDialog.Builder(GameActivity.this).setTitle("Соединение LAN потеряно")
                            .setMessage("Матч больше не синхронизируется с другими игроками. Для новой сетевой игры вернитесь в лобби.")
                            .setPositiveButton("Открыть меню",(dialog,n)->{nativePause(false);tapKey(0x01);})
                            .setNegativeButton("Остаться",(dialog,n)->nativePause(false))
                            .setOnCancelListener(dialog->nativePause(false)).show();
                }
                int[] next=nativeControlState();
                boolean changed=next[0]!=controlState[0];controlState=next;
                if (!rememberedSuccess && (next[0] & 9) != 0) {
                    try {
                        GameFiles.rememberSuccess(getFilesDir(), edition, gamePath);
                        java.nio.file.Files.deleteIfExists(GameFiles.pending(getFilesDir(), edition).toPath());
                        rememberedSuccess = true;
                    } catch (java.io.IOException error) { Log.e(TAG, "Не удалось сохранить последнюю игру", error); }
                }
                /* Confirm view toggles whose effect may be invisible, e.g. radar colours without rivals. */
                if((event&16)!=0)hint((next[8]&1)!=0?"Радар: окраска «свои / чужие»":"Радар: цвета лордов");
                if((event&32)!=0)hint((next[8]&2)!=0?"Рельеф выровнен":"Рельеф показан");
                if((next[0]&4)!=0&&lanDiscovery==null) {
                    android.net.wifi.WifiManager wifi=(android.net.wifi.WifiManager)getApplicationContext().getSystemService(WIFI_SERVICE);
                    if(wifi!=null){lanDiscovery=wifi.createMulticastLock("stronghold-lan");lanDiscovery.setReferenceCounted(false);lanDiscovery.acquire();}
                } else if((next[0]&4)==0&&lanDiscovery!=null){lanDiscovery.release();lanDiscovery=null;}
                if(changed)rebuildTools();
                updateSpeed();
                positionZoom();
                samplePerformance();
                touch.invalidate();
            }
            root.postDelayed(this, 100);
        }
    };
    private static final android.util.LruCache<String, android.graphics.Bitmap> glyphs =
            new android.util.LruCache<String, android.graphics.Bitmap>(4 * 1024 * 1024) {
                @Override protected int sizeOf(String key, android.graphics.Bitmap value) { return value.getAllocationByteCount(); }
            };

    public static android.graphics.Bitmap textMask(String text, int size, int weight, int italic) {
        String key = size + ":" + weight + ":" + italic + ":" + text;
        android.graphics.Bitmap cached = glyphs.get(key);
        if (cached != null) return cached;
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        paint.setHinting(Paint.HINTING_ON);
        int style = (weight >= 700 ? android.graphics.Typeface.BOLD : 0) | (italic != 0 ? android.graphics.Typeface.ITALIC : 0);
        paint.setTypeface(android.graphics.Typeface.create("sans-serif", style));
        paint.setColor(0xffffffff);
        if (size == 0) size = 16;
        paint.setTextSize(Math.max(1, Math.min(512, Math.abs(size))));
        Paint.FontMetrics fm = paint.getFontMetrics();
        if (size > 0) { paint.setTextSize(size * paint.getTextSize() / (fm.bottom - fm.top)); fm = paint.getFontMetrics(); }
        int width = Math.max(1, (int)Math.ceil(paint.measureText(text)));
        int height = Math.max(1, (int)Math.ceil(fm.bottom - fm.top));
        android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ALPHA_8);
        new Canvas(bitmap).drawText(text, 0, -fm.top, paint);
        glyphs.put(key, bitmap);
        return bitmap;
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        edition = gameEdition();
        gamePath = getIntent().getStringExtra("game");
        if (gamePath == null) gamePath = GameFiles.DEFAULT_GAME;
        skipMovies = getIntent().getBooleanExtra("skipMovies", false);
        String fileError = GameFiles.validate(new File(gamePath), edition, false);
        if (fileError != null) { launchError(fileError); return; }
        if (loadedEdition != null && !loadedEdition.equals(edition)) { chooseEdition(); return; }
        try {
            if (loadedEdition == null) {
                System.loadLibrary(GameFiles.library(edition));
                loadedEdition = edition;
            }
            ready = true;
        } catch (UnsatisfiedLinkError error) {
            launchError("Не удалось загрузить библиотеку игры: " + error.getMessage());
            return;
        }
        try {
            java.nio.file.Files.write(new File(getFilesDir(), "game-pid-" + edition).toPath(),
                    Integer.toString(android.os.Process.myPid()).getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        } catch (java.io.IOException error) { launchError("Не удалось записать состояние процесса игры: " + error.getMessage()); return; }
        preferences = getSharedPreferences("controls-" + edition, MODE_PRIVATE);
        /* The shared pre-0.4 controls belonged to Crusader; Stronghold HD starts fresh. */
        if (!preferences.contains("uiScale") && !GameFiles.original(edition)) {
            SharedPreferences legacy = getSharedPreferences("controls", MODE_PRIVATE);
            SharedPreferences.Editor migrated = preferences.edit();
            for (java.util.Map.Entry<String, ?> item : legacy.getAll().entrySet()) {
                Object value = item.getValue();
                if (value instanceof Integer) migrated.putInt(item.getKey(), (Integer)value);
                else if (value instanceof Float) migrated.putFloat(item.getKey(), (Float)value);
                else if (value instanceof Boolean) migrated.putBoolean(item.getKey(), (Boolean)value);
            }
            migrated.putInt("uiScale", legacy.getInt("uiScale", 120)).commit();
        }
        debugInput = Log.isLoggable(TAG, Log.DEBUG);
        readPreferences();
        forgetOtherBuildsPerformance(this, preferences);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams window = getWindow().getAttributes();
        window.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(window);
        if (android.os.Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(false);
        root = new FrameLayout(this); root.setBackgroundColor(0xff000000);
        SurfaceView surface = new SurfaceView(this); surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(-1, -1));
        touch = new PointerView();
        mouseGate = new MouseGate(new MouseGate.Output() {
            public void button(int button, boolean down) { if (started) nativeMouseButton(button, down); }
            public void move(float x, float y, boolean warp) {
                if (!started || touch.getWidth() == 0 || touch.getHeight() == 0) return;
                if (warp) nativeMouseWarp(x / touch.getWidth(), y / touch.getHeight());
                else nativeMouseMove(x / touch.getWidth(), y / touch.getHeight(), false);
            }
        }, new MouseGate.Scheduler() {
            public void post(Runnable work, long delay) { root.postDelayed(work, delay); }
            public void remove(Runnable work) { root.removeCallbacks(work); }
        });
        gestures = new TouchControls(new TouchControls.Output() {
            public void move(float x, float y, boolean warp) { mouseGate.move(x, y, warp); }
            public void button(int button, boolean down) { if (down) mouseGate.beginDrag(); else mouseGate.cancel(); }
            public void click(int button) { clickMouse(button); }
            public void key(int scan, boolean down) { setKey(scan, down); }
            public void cursor(float x, float y) { touch.x = x; touch.y = y; touch.invalidate(); }
        }, ViewConfiguration.get(this).getScaledTouchSlop());
        semantic = new SemanticTouchControls((action, sequence, x, y, dx, dy, scale) -> {
            if (!started || touch.getWidth() == 0 || touch.getHeight() == 0) return;
            if (action == SemanticTouchControls.HOLD) haptic(HapticFeedbackConstants.LONG_PRESS);
            else if (action == SemanticTouchControls.ROTATE) haptic(HapticFeedbackConstants.CLOCK_TICK);
            nativeTouch(action, sequence, x / touch.getWidth(), y / touch.getHeight(),
                    dx / touch.getWidth(), dy / touch.getHeight(), scale);
            watchFeedback();
        }, ViewConfiguration.get(this).getScaledTouchSlop());
        mouse = new HardwareInput.Mouse(new HardwareInput.Output() {
            public void button(int button, boolean down) { mouseButton(button, down); }
            public void move(float x, float y, boolean warp) {
                if (!started || touch.getWidth() == 0 || touch.getHeight() == 0) return;
                if (warp) nativeMouseWarp(x / touch.getWidth(), y / touch.getHeight());
                else nativeMouseMove(x / touch.getWidth(), y / touch.getHeight(), false);
            }
            public void wheel(float x, float y, int delta, float zoom, boolean burst) {
                if (started && touch.getWidth() > 0 && touch.getHeight() > 0)
                    nativeTouch(SemanticTouchControls.WHEEL, 0, x / touch.getWidth(), y / touch.getHeight(), delta, burst ? 1 : 0, zoom);
            }
            public void grab(float x, float y, float dx, float dy) {
                if (started && touch.getWidth() > 0 && touch.getHeight() > 0)
                    nativeTouch(SemanticTouchControls.GRAB, 0, x / touch.getWidth(), y / touch.getHeight(),
                            dx / touch.getWidth(), dy / touch.getHeight(), 1);
            }
            public void middleClick(float x, float y) {
                if (started && (controlState[0] & 1) != 0) nativeTouch(SemanticTouchControls.ZOOM_RESET, 0, 0, 0, 0, 0, 1);
            }
        }, ViewConfiguration.get(this).getScaledTouchSlop());
        gamepad = new GamepadControls(new GamepadControls.Output() {
            public void move(float dx, float dy) {
                float fit = Math.min((float)touch.getWidth() / requestedWidth, (float)touch.getHeight() / requestedHeight);
                float left = (touch.getWidth() - requestedWidth * fit) / 2, top = (touch.getHeight() - requestedHeight * fit) / 2;
                mouseX = Math.max(left, Math.min(touch.getWidth() - left - 1, mouseX + dx * touch.getHeight() * sensitivity));
                mouseY = Math.max(top, Math.min(touch.getHeight() - top - 1, mouseY + dy * touch.getHeight() * sensitivity));
                touch.x = mouseX; touch.y = mouseY;
                nativeMouseMove(mouseX / touch.getWidth(), mouseY / touch.getHeight(), false);
                nativeMouseEdges(0); // The right stick owns camera scrolling, including at the screen edges.
            }
            public void pan(float dx, float dy) {
                if ((controlState[0] & 1) != 0)
                    nativeTouch(SemanticTouchControls.GRAB, 0, .5f, .5f, -dx * touch.getHeight() / touch.getWidth(), -dy, 1);
            }
            public void button(int button, boolean down) {
                if (button == 0) {
                    if (down) { gamepadClick = controlAt(root, mouseX, mouseY); if (gamepadClick != null) { gamepadClick.setPressed(true); return; } }
                    else if (gamepadClick != null) {
                        View click = gamepadClick; gamepadClick = null; click.setPressed(false);
                        if (click == controlAt(root, mouseX, mouseY)) click.performClick();
                        return;
                    }
                }
                mouseButton(button, down);
            }
            public void key(int scan, boolean down) {
                if (down ? padKeys.add(scan) : padKeys.remove(scan))
                    if (down || (!held.contains(scan) && !pressed.contains(scan))) nativeKey(scan, down);
            }
            public void zoom(int direction) { mouse.scroll(mouseX, mouseY, direction, SystemClock.uptimeMillis()); }
            public void command(int key, int modifiers) {
                if ((controlState[0] & 1) != 0) {
                    nativeCommand(key, modifiers);
                    if (key >= '1' && key <= '9') hint((modifiers % 2 == 1 ? "Записать " : "Вызвать ") +
                            (modifiers >= 2 ? "точку " : "отряд ") + (char)key);
                }
            }
            public void action(int action) {
                switch (action) {
                    case GamepadControls.HELP: showGamepadHelp(); break;
                    case GamepadControls.RESET_ZOOM: nativeTouch(SemanticTouchControls.ZOOM_RESET, 0, 0, 0, 0, 0, 1); break;
                    case GamepadControls.TOOLS: showTools(!extra); rebuildTools(); break;
                    case GamepadControls.KEYBOARD: showTextInput(); break;
                    case GamepadControls.SETTINGS: settings(); break;
                }
            }
        });
        inputManager = (InputManager)getSystemService(INPUT_SERVICE);
        gestures.mode(touchpad, sensitivity);
        semantic.buildingSwipes(preferences.getBoolean("buildingSwipes",true));
        touch.setOnTouchListener((view, event) -> pointer(event));
        /* Hover, wheel and button press/release of a mouse. Consuming the
         * secondary button's press keeps Android from turning it into BACK. */
        touch.setOnGenericMotionListener((view, event) ->
                HardwareInput.isMouse(event.getSource(), event.getToolType(0)) && mouseEvent(event));
        root.addView(touch, new FrameLayout.LayoutParams(-1, -1));
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (r <= l || b <= t) return;
            if (!started) startGame(r - l, b - t);
            updateBounds(r - l, b - t);
            /* Saved positions are fractions of the view: place them once it has a size. */
            if (or - ol != r - l || ob - ot != b - t) root.post(this::rebuildTools);
        });
        rebuildTools(); setContentView(root); immersive();
        if (android.os.Build.VERSION.SDK_INT >= 29) getWindow().getDecorView().setForceDarkAllowed(false);
    }
    private void discoverEditions() {
        new Thread(() -> {
            /* The launcher can change paths in its own process while this game is paused. */
            SharedPreferences launcher = getSharedPreferences("LauncherActivity", MODE_PRIVATE | MODE_MULTI_PROCESS);
            java.util.List<String> available = new java.util.ArrayList<>();
            String[] paths = GameFiles.savedPaths(launcher.getString("game", null), launcher.getString("extremeGame", null),
                    launcher.getString("strongholdGame", null));
            for (int i = 0; i < GameFiles.EDITIONS.length; i++) {
                String other = GameFiles.EDITIONS[i];
                if (other.equals(edition)) continue;
                if (GameFiles.validate(new File(paths[i]), other, false) == null) available.add(other);
            }
            runOnUiThread(() -> { otherEditions = available; if (root != null && !isFinishing()) rebuildTools(); });
        }, "edition-discovery").start();
    }
    private void readPreferences() {
        /* Stronghold HD's panel and dialogs only grow: a smaller panel would uncover map rows it still owns. */
        boolean original = GameFiles.original(edition);
        uiScale = Math.max(original ? 100 : 75, Math.min(200, preferences.getInt("uiScale", original ? 100 : 120)));
        controlsScale = Math.max(75, Math.min(200, preferences.getInt("controlsScale", 100)));
        touchpad = preferences.getBoolean("touchpad", false); wide = preferences.getBoolean("wide", true);
        extra = preferences.getBoolean("extra", false); sensitivity = preferences.getFloat("sensitivity", 1);
        diagnostics = preferences.getBoolean("touchDiagnostics", false);
        groupWheel=preferences.getBoolean("groupWheel",false);markerWheel=preferences.getBoolean("markerWheel",false);
        opacity = Math.max(.25f, Math.min(1, preferences.getFloat("opacity", .75f)));
        SettingsActivity.migrate(preferences);
        worldFilter = filter(preferences.getString("worldFilter", "fsr"));
        uiFilter = filter(preferences.getString("uiFilter", "fsr"));
        sharpness = Math.max(0, Math.min(100, preferences.getInt("sharpness", 25))) / 100f;
        deband = preferences.getBoolean("deband", false); crispText = preferences.getBoolean("crispText", true);
        vibration = preferences.getBoolean("vibration", true); loupe = preferences.getBoolean("loupe", false);
        loupeSize = Math.max(50, Math.min(200, preferences.getInt("loupeSize", 100)));
        loupeZoom = Math.max(150, Math.min(400, preferences.getInt("loupeZoom", 200)));
    }
    /* SH_FILTER_* (display.h): 0 bilinear, 1 sharp pixels, 2 bicubic, 3 FSR 1.0. */
    static int filter(String name) {
        return "bilinear".equals(name) ? 0 : "pixels".equals(name) ? 1 : "bicubic".equals(name) ? 2 : 3;
    }
    private static native void nativeSmoothCamera(boolean enabled);
    private static native void nativePowersScale(float scale);
    private static native void nativeCutout(float left);
    private void applyFilters() {
        nativeFilters(worldFilter, uiFilter, sharpness, deband, crispText);
        nativeSmoothCamera(preferences.getBoolean("smoothCamera",true));
        if ("extreme".equals(edition)) nativePowersScale(preferences.getInt("powersScale",150)/100f);
        applyCutout();
    }
    private void applyCutout() {
        if (!ready || root == null || root.getWidth() <= 0) return;
        android.view.WindowInsets insets = root.getRootWindowInsets();
        android.view.DisplayCutout cutout = insets == null ? null : insets.getDisplayCutout();
        nativeCutout(cutout == null ? 0 : (float)cutout.getSafeInsetLeft()/root.getWidth());
    }
    private void startGame(int width, int height) {
        int[] size = resolution(width, height);
        requestedWidth = size[0]; requestedHeight = size[1];
        File base = new File(Environment.getExternalStorageDirectory(), "Stronghold");
        File data = new File(base, GameFiles.userdata(edition));
        if ((!data.isDirectory() && !data.mkdirs()) || !data.canWrite()) { launchError("Не удалось открыть папку профиля: " + data); return; }
        try {
            File pending = GameFiles.pending(getFilesDir(), edition);
            if (!pending.exists() && !pending.createNewFile()) throw new java.io.IOException("Не удалось записать состояние запуска");
        } catch (java.io.IOException error) { launchError(error.getMessage()); return; }
        /* The former world downscale gave no speed (the original raster still
         * draws every tile) and only lost detail; presentation filters replace it. */
        nativeRenderScale(1f);
        applyFilters();
        applyGameSettings();
        if (!nativeStart(gamePath, data.getAbsolutePath(), GameFiles.images(new File(gamePath), edition).getAbsolutePath(),
                GameFiles.pending(getFilesDir(), edition).getAbsolutePath(), requestedWidth, requestedHeight,
                new File(base, "verbose").exists(), skipMovies)) { launchError("Не удалось создать поток игры."); return; }
        started = true;
        preferences.edit().putString("activeResolution", preferences.getString("resolution", "auto")).apply();
        nativeTouch(SemanticTouchControls.UI_SCALE, 0, 0, 0, 0, 0, uiScale / 100f);
        nativeTouch(SemanticTouchControls.MODE, 0, 0, 0, 0, 0, touchpad || mouseMode ? 0 : 1);
    }
    private int[] resolution(int width, int height) {
        android.graphics.Point display = new android.graphics.Point();
        getWindowManager().getDefaultDisplay().getRealSize(display);
        /* Stronghold HD: a height of its own mode table, as wide as the screen (the native side keeps the mode). */
        if (GameFiles.original(edition)) return strongholdMode(preferences.getString("resolution", "720"), display.x, display.y, wide);
        float density = getResources().getDisplayMetrics().density;
        String selected = preferences.getString("resolution","auto");
        if ("autoplus".equals(selected)) {
            /* This start's level becomes the base of the next decision. */
            int[] size = autoPlus(preferences, display.x, display.y, density, wide);
            preferences.edit().putInt("autoPlusHeight", size[1]).apply();
            return size;
        }
        return TouchControls.resolution(selected,display.x,display.y,density,wide);
    }

    /* ---- Авто+: per-size frame costs measured in matches pick the next start's size. ---- */
    private static String perfKey(int width, int height) { return "perf-" + width + "x" + height; }
    /* Measurements belong to one APK: a new build measures afresh, since its costs differ. */
    static void forgetOtherBuildsPerformance(android.content.Context context, SharedPreferences p) {
        long build = 0;
        try { build = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).lastUpdateTime; }
        catch (Exception ignored) { return; }
        if (p.getLong("perfBuild", 0) == build) return;
        SharedPreferences.Editor edit = p.edit().putLong("perfBuild", build);
        for (String key : p.getAll().keySet()) if (key.startsWith("perf-")) edit.remove(key);
        edit.commit();
    }
    static int[] autoPlusSize(int height, int deviceWidth, int deviceHeight, float density, boolean wide) {
        return TouchControls.resolution("x" + height, deviceWidth, deviceHeight, density, wide);
    }
    /** The size the next start uses with Авто+: {width, height, p95 ms or -1}. */
    static int[] autoPlus(SharedPreferences p, int deviceWidth, int deviceHeight, float density, boolean wide) {
        int[] levels = TouchControls.autoPlusLevels(deviceWidth, deviceHeight, density);
        float[] p95 = new float[levels.length]; int[] frames = new int[levels.length];
        int current = TouchControls.autoPlusStart(levels, deviceWidth, deviceHeight, density);
        for (int i = 0; i < levels.length; i++) {
            int[] size = autoPlusSize(levels[i], deviceWidth, deviceHeight, density, wide);
            p95[i] = p.getFloat(perfKey(size[0], size[1]) + "-p95", 0);
            frames[i] = p.getInt(perfKey(size[0], size[1]) + "-frames", 0);
            if (levels[i] == p.getInt("autoPlusHeight", -1)) current = i;
        }
        int next = TouchControls.autoPlusStep(current, p95, frames);
        int[] size = autoPlusSize(levels[next], deviceWidth, deviceHeight, density, wide);
        return new int[] {size[0], size[1], frames[next] > 0 ? Math.round(p95[next]) : -1};
    }
    /* Every ~10 s of a match the last window of frame costs joins the record of
     * the running size. Menu and loading frames are dropped at each change. */
    private void samplePerformance() {
        long now = SystemClock.uptimeMillis();
        boolean match = (controlState[0] & 1) != 0;
        if (match != perfMatch || perfAt == 0) { perfMatch = match; perfAt = now; nativePerfStats(true); return; }
        if (!match || now - perfAt < 10000) return;
        perfAt = now;
        float[] stats = nativePerfStats(true);
        if (stats == null || stats[3] < 30) return;
        String key = perfKey(requestedWidth, requestedHeight);
        float[] record = TouchControls.autoPlusRecord(preferences.getFloat(key + "-p95", 0), preferences.getInt(key + "-frames", 0),
                stats[1], Math.round(stats[3]));
        preferences.edit().putFloat(key + "-p95", record[0]).putInt(key + "-frames", (int)record[1])
                .putFloat(key + "-mean", stats[0]).apply();
        if (debugInput) Log.d(TAG, String.format(java.util.Locale.ROOT, "perf %dx%d mean %.1f p95 %.1f fps %.1f n %.0f -> p95 %.1f over %d",
                requestedWidth, requestedHeight, stats[0], stats[1], stats[2], stats[3], record[0], (int)record[1]));
        if (!"autoplus".equals(preferences.getString("resolution", "auto"))) return;
        android.graphics.Point display = new android.graphics.Point();
        getWindowManager().getDefaultDisplay().getRealSize(display);
        int[] next = autoPlus(preferences, display.x, display.y, getResources().getDisplayMetrics().density, wide);
        String size = next[0] + "×" + next[1];
        if ((next[0] != requestedWidth || next[1] != requestedHeight) && !size.equals(autoPlusToast)) {
            autoPlusToast = size;
            Toast.makeText(this, "Авто+: со следующего запуска игры " + size, Toast.LENGTH_SHORT).show();
        }
    }
    /* Stronghold HD frame heights (its mode table) and their own 16:9 widths (600 rows: 4:3). */
    static final int[] STRONGHOLD_HEIGHTS = {600, 720, 768, 900, 1080}, STRONGHOLD_WIDTHS = {800, 1280, 1366, 1600, 1920};
    /** The frame for a saved choice: a table height ("720", or a former "1280x720"); wide, as wide as the
     * screen (8-pixel multiples, 2560 at most: the widest stock panel edges), else the game's own mode. */
    static int[] strongholdMode(String value, int deviceWidth, int deviceHeight, boolean wide) {
        int height = 720;
        try { String[] size = value.split("x"); height = Integer.parseInt(size[size.length - 1]); }
        catch (RuntimeException invalid) { /* the default height */ }
        int index = 1;
        for (int i = 0; i < STRONGHOLD_HEIGHTS.length; i++) if (STRONGHOLD_HEIGHTS[i] == height) index = i;
        height = STRONGHOLD_HEIGHTS[index];
        if (!wide) return new int[] {STRONGHOLD_WIDTHS[index], height};
        double aspect = (double)Math.max(deviceWidth, deviceHeight) / Math.max(1, Math.min(deviceWidth, deviceHeight));
        int width = ((int)Math.round(height * Math.max(4.0 / 3, aspect)) + 7) & ~7;
        return new int[] {Math.max(800, Math.min(2560, width)), height};
    }
    private void updateBounds(int width, int height) {
        float fit = Math.min((float)width / requestedWidth, (float)height / requestedHeight);
        float x = (width - requestedWidth * fit) / 2, y = (height - requestedHeight * fit) / 2;
        gestures.bounds(x, y, width - x - 1, height - y - 1);
        semantic.bounds(x, y, width - x - 1, height - y - 1);
    }
    private int dp(float n) { return Math.round(n * getResources().getDisplayMetrics().density); }
    private float controls() { return controlsScale / 100f; }
    /* Android controls show the system pointer: without an icon of their own the
     * resolution falls through to the game view below, which hides it. */
    private <T extends View> T arrow(T v) { v.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_ARROW)); return v; }
    private LinearLayout row() { LinearLayout v = arrow(new LinearLayout(this)); v.setOrientation(LinearLayout.HORIZONTAL); return v; }
    private LinearLayout column() { LinearLayout v = arrow(new LinearLayout(this)); v.setOrientation(LinearLayout.VERTICAL); return v; }
    private GradientDrawable panel(int color) {
        GradientDrawable shape = new GradientDrawable(); shape.setColor(color);
        shape.setCornerRadius(dp(7 * controls())); shape.setStroke(Math.max(1, dp(1)), 0x38ffffff); return shape;
    }
    /* Every Android control follows the chosen size and visibility. */
    private TextView button(String title) {
        TextView v = arrow(new TextView(this)); v.setText(title); v.setTextColor(0xffffffff);
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13 * controls());
        v.setGravity(Gravity.CENTER); v.setContentDescription(title); v.setBackground(panel(0xf029333e)); v.setAlpha(opacity);
        int height = dp(Math.max(34, 44 * controls()));
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, height);
        int margin = dp(2 * controls()); p.setMargins(margin, margin, margin, margin); v.setLayoutParams(p);
        v.setMinWidth(height); int padding = dp(10 * controls()); v.setPadding(padding, 0, padding, 0); return v;
    }
    private TextView action(LinearLayout row, String title, Runnable action) {
        TextView v = button(title); v.setOnClickListener(ignored -> action.run()); row.addView(v); return v;
    }
    private void updateZoomIndicator() {
        if (zoomIndicator == null) return;
        int zoom = started ? Math.round(nativeWorldZoom() * 100) : 100;
        if (zoom == displayedZoom) return;
        displayedZoom = zoom;
        String text = zoom + "%";
        zoomIndicator.setText(text);
        zoomIndicator.setContentDescription("Масштаб карты " + text + ". Нажмите, чтобы вернуть 100%");
        zoomIndicator.setVisibility(zoom==100||(controlState[0]&1)==0?View.GONE:View.VISIBLE);
        positionZoom();
    }
    private void updateSpeed() {
        if (speedValue == null || controlState[9] == displayedSpeed) return;
        displayedSpeed = controlState[9];
        /* Hide the value until the original simulation has initialized it. */
        speedValue.setVisibility(displayedSpeed > 0 ? View.VISIBLE : View.GONE);
        speedValue.setText(Integer.toString(displayedSpeed));
        speedValue.setContentDescription("Скорость игры " + displayedSpeed);
    }
    private String switchLabel() { return otherEditions.isEmpty() ? "Лаунчер" : otherEditions.size() == 1 ? title(otherEditions.get(0)) : "Другая игра"; }
    private static String title(String edition) { return GameFiles.title(edition); }
    private float[] saved(String key) {
        if (preferences == null) return null;
        float x = preferences.getFloat("pos-" + key + "-x", Float.NaN), y = preferences.getFloat("pos-" + key + "-y", Float.NaN);
        return Float.isNaN(x) || Float.isNaN(y) ? null : new float[] {x, y};
    }
    private void save(String key, float x, float y) {
        preferences.edit().putFloat("pos-" + key + "-x", x).putFloat("pos-" + key + "-y", y).apply();
    }
    private void resetLayout() {
        SharedPreferences.Editor edit = preferences.edit();
        for (String key : MOVABLE) edit.remove("pos-" + key + "-x").remove("pos-" + key + "-y");
        edit.apply(); rebuildTools();
    }
    /* A dragged element keeps its top-left fraction of the view; defaults keep their old anchors. */
    private void place(View view, String key, int gravity, int marginX, int marginY) {
        float[] position = saved(key);
        FrameLayout.LayoutParams p;
        if (position == null || root.getWidth() == 0) {
            p = new FrameLayout.LayoutParams(-2, -2, gravity);
            p.setMargins(marginX, marginY, marginX, 0);
        } else {
            p = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.LEFT);
            p.leftMargin = Math.round(position[0] * root.getWidth()); p.topMargin = Math.round(position[1] * root.getHeight());
        }
        root.addView(view, p);
        if (position != null) view.post(() -> moveView(view, ((FrameLayout.LayoutParams)view.getLayoutParams()).leftMargin,
                ((FrameLayout.LayoutParams)view.getLayoutParams()).topMargin));
    }
    private void moveView(View view, float left, float top) {
        if (view.getParent() != root) return;
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.LEFT);
        p.leftMargin = Math.round(Math.max(0, Math.min(root.getWidth() - view.getWidth(), left)));
        p.topMargin = Math.round(Math.max(0, Math.min(root.getHeight() - view.getHeight(), top)));
        view.setLayoutParams(p);
    }
    private void rebuildTools() {
        if (root == null || mouseGate == null) return;
        for (int scan : new HashSet<Integer>(held)) if (scan != 0x1d && scan != 0x2a) setKey(scan, false);
        mouseGate.cancel();
        for (View old : new View[] {tools, mouseTools, zoomIndicator, switchButton, layoutEditor, editorBar, gamepadHint})
            if (old != null) root.removeView(old);
        tools = column(); speedValue = null; displayedSpeed = -1;
        if (extra || layoutEdit) {
            LinearLayout bar = row(); tools.addView(bar);
            action(bar,"×",()->{showTools(false);editLayout(false);rebuildTools();});
            action(bar, "Меню", () -> tapKey(0x01)); action(bar, "Пауза", () -> tapKey(0x19));
            action(bar, "Ввод", this::showTextInput); action(bar, "Настройки", this::settings);
            action(bar, "Расположение", () -> editLayout(true)).setContentDescription("Изменить расположение кнопок");
            action(bar, "?", this::help);
            action(bar, "⇄", this::switchEdition).setContentDescription("Другая игра или лаунчер");
            LinearLayout keys = row(); tools.addView(keys);
            modifier(keys, "Ctrl", 0x1d); modifier(keys, "Shift", 0x2a);
            holdKey(keys, "⌫", 0x0e); holdKey(keys, "Enter", 0x1c); holdKey(keys, "F5", 0x3f); holdKey(keys, "F1 · замок", 0x3b);
            holdKey(keys, "−", 0x4a).setContentDescription("Медленнее");
            /* The speed is a value between its buttons, drawn as a darker chip so it stays readable over the map. */
            speedValue = button(""); speedValue.setBackground(panel(0xf0101820)); speedValue.setTextColor(0xffffe08a);
            speedValue.setTypeface(android.graphics.Typeface.DEFAULT_BOLD); speedValue.setMinWidth(dp(40 * controls()));
            speedValue.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15 * controls());
            speedValue.setPadding(dp(6 * controls()), 0, dp(6 * controls()), 0); keys.addView(speedValue);
            holdKey(keys, "+", 0x4e).setContentDescription("Быстрее");
            LinearLayout view=row();tools.addView(view);
            action(view,"Рельеф",()->nativeCommand(0x20,0));action(view,"Масштаб",()->nativeCommand('Z',0));
            TextView left=action(view,"↺",()->nativeCommand('X',0)),right=action(view,"↻",()->nativeCommand('C',0));
            left.setContentDescription("Повернуть карту против часовой стрелки");
            right.setContentDescription("Повернуть карту по часовой стрелке");
            for(TextView turn:new TextView[]{left,right})turn.setTextSize(TypedValue.COMPLEX_UNIT_SP,21*controls());
            if((controlState[0]&2)!=0)action(view,"Редактор…",this::editorCommands);
            updateSpeed();
        }
        zoomIndicator = arrow(new TextView(this)); zoomIndicator.setTextColor(0xffffe08a);
        zoomIndicator.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17 * controls()); zoomIndicator.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        zoomIndicator.setGravity(Gravity.CENTER); zoomIndicator.setBackground(panel(0xd8101820)); zoomIndicator.setAlpha(Math.max(.5f, opacity));
        zoomIndicator.setPadding(dp(9 * controls()), dp(3 * controls()), dp(9 * controls()), dp(3 * controls()));
        zoomIndicator.setMinHeight(dp(32 * controls()));
        zoomIndicator.setOnClickListener(v -> {
            if (!started) return;
            nativeTouch(SemanticTouchControls.ZOOM_RESET, 0, 0, 0, 0, 0, 1);
            haptic(HapticFeedbackConstants.VIRTUAL_KEY);
        });
        root.addView(zoomIndicator,new FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.LEFT));
        displayedZoom = -1; updateZoomIndicator(); positionZoom();
        place(tools, "tools", Gravity.TOP | Gravity.CENTER_HORIZONTAL, 0, dp(4));
        mouseTools = row();
        if (touchpad) {
            mouseButton(mouseTools, "ЛКМ", 0); mouseButton(mouseTools, "ПКМ", 1);
            place(mouseTools, "mouse", Gravity.TOP | Gravity.RIGHT, dp(4), dp(4));
        }
        switchButton = button("⇄  " + switchLabel());
        switchButton.setContentDescription("Другая игра или лаунчер");
        switchButton.setOnClickListener(v -> switchEdition());
        switchButton.setVisibility((controlState[0] & 8) != 0 || layoutEdit ? View.VISIBLE : View.GONE);
        place(switchButton, "switch", Gravity.TOP | Gravity.RIGHT, dp(12), dp(touchpad ? 60 : 12));
        gamepadHint = button("Геймпад · раскладка\nSelect / Back");
        gamepadHint.setContentDescription("Раскладка геймпада. Нажмите Select или Back на геймпаде");
        gamepadHint.setOnClickListener(v -> showGamepadHelp());
        gamepadHint.setVisibility(gamepadConnected && (controlState[0] & 8) != 0 ? View.VISIBLE : View.GONE);
        FrameLayout.LayoutParams padHint = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.LEFT);
        padHint.leftMargin = dp(12); padHint.topMargin = dp(12);
        android.view.WindowInsets insets = root.getRootWindowInsets();
        if (insets != null && insets.getDisplayCutout() != null) padHint.leftMargin += insets.getDisplayCutout().getSafeInsetLeft();
        root.addView(gamepadHint, padHint);
        if (layoutEdit) {
            layoutEditor = new LayoutEditor(); root.addView(layoutEditor, new FrameLayout.LayoutParams(-1, -1));
            editorBar = row(); editorBar.setBackground(panel(0xf0182029)); editorBar.setGravity(Gravity.CENTER_VERTICAL);
            int padding = dp(8); editorBar.setPadding(padding, padding / 2, padding, padding / 2);
            TextView label = new TextView(this); label.setText("Перетащите кнопки"); label.setTextColor(0xffffe08a);
            label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14); label.setPadding(dp(8), 0, dp(12), 0); editorBar.addView(label);
            action(editorBar, "Сбросить", this::resetLayout); action(editorBar, "Готово", () -> editLayout(false));
            /* Just above the HUD: the movable controls normally sit higher, over the map. */
            FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
            p.bottomMargin = Math.max(dp(8), Math.round(root.getHeight() - hudPoint(0, 452)[1]) + dp(8));
            root.addView(editorBar, p);
        }
        if (hint != null) hint.bringToFront();
        touch.invalidate();
    }
    private void editLayout(boolean enabled) {
        if (layoutEdit == enabled) return;
        releaseControls(); layoutEdit = enabled;
        if (enabled) showTools(true);
        rebuildTools();
    }
    /* The quick-command panel state is stored at once: the original "Quit"
     * ends the process without an Android pause. */
    private void showTools(boolean shown) {
        extra = shown;
        if (preferences != null) preferences.edit().putBoolean("extra", extra).apply();
    }
    private void hint(String text) {
        if (hint == null) {
            hint = arrow(new TextView(this)); hint.setTextColor(0xffffefc2); hint.setGravity(Gravity.CENTER);
            hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15); hint.setBackground(panel(0xe0101820));
            hint.setPadding(dp(16), dp(8), dp(16), dp(8));
            FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER_HORIZONTAL | Gravity.TOP);
            p.topMargin = Math.round(getResources().getDisplayMetrics().heightPixels * .3f);
            root.addView(hint, p);
        }
        hint.bringToFront(); hint.setText(text); hint.announceForAccessibility(text);
        hint.animate().cancel(); hint.setAlpha(1); hint.setVisibility(View.VISIBLE);
        hint.animate().alpha(0).setStartDelay(1600).setDuration(450).withEndAction(() -> hint.setVisibility(View.GONE)).start();
    }
    private void switchEdition() {
        releaseControls();
        String[] editions = otherEditions.toArray(new String[0]);
        String[] names = new String[editions.length + 1];
        for (int i = 0; i < editions.length; i++) names[i] = title(editions[i]);
        names[editions.length] = "Открыть лаунчер";
        new AlertDialog.Builder(this).setTitle("Перейти · " + title(edition) + " закроется")
                .setItems(names, (dialog, which) -> {
                    if (which == editions.length) returnToLauncher();
                    else launchEdition(editions[which]);
                })
                .setNegativeButton("Остаться", null).setOnDismissListener(dialog -> immersive()).show();
    }
    private void launchEdition(String other) {
        if (preferences != null) preferences.edit().putBoolean("extra", extra).commit();
        GameFiles.pending(getFilesDir(), edition).delete();
        startActivity(new Intent(this, LauncherActivity.class).putExtra("edition", other).putExtra("launchGame", true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP));
        finishAndRemoveTask(); android.os.Process.killProcess(android.os.Process.myPid());
    }

    private float[] hudPoint(float x,float y) {
        if (started) {
            float[] point = nativeHudPoint(x + controlState[6], y + controlState[7]);
            if (point != null) return point;
        }
        float w=controlState[4]>0?controlState[4]:requestedWidth,h=controlState[5]>0?controlState[5]:requestedHeight;
        if(w<=0||h<=0)return new float[]{0,0};
        float fit=Math.min(touch.getWidth()/w,touch.getHeight()/h),scale=uiScale/100f;
        return new float[]{(touch.getWidth()-w*fit)/2+(w/2+(x+controlState[6]-w/2)*scale)*fit,
                (touch.getHeight()-h*fit)/2+(h+(y+controlState[7]-h)*scale)*fit};
    }
    /* The value sits over the radar's top-right corner; its right edge stays there for any width. */
    private void positionZoom() {
        if(zoomIndicator==null||touch==null)return;
        float[] point=hudPoint((controlState[0]&2)!=0?791:697,465);
        zoomIndicator.measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED);
        zoomIndicator.setX(Math.max(0,point[0]-zoomIndicator.getMeasuredWidth()));
        zoomIndicator.setY(Math.max(0,point[1]-zoomIndicator.getMeasuredHeight()-dp(4)));
    }
    private void editorCommands() {
        String[] names={"Пауза / продолжить","Налоги, деньги и рацион","Восстановить лорда","Тип карты","Удалить животных","Швы местности"};
        int[] keys={'H',0xDE,'V',0xBC,'E','D'};
        new AlertDialog.Builder(this).setTitle("Редактор карты").setItems(names,(d,n)->nativeCommand(keys[n],2)).setNegativeButton("Закрыть",null).show();
    }
    private void modifier(LinearLayout row, String label, int scan) {
        TextView v = action(row, label, () -> {});
        v.setSelected(held.contains(scan)); ((GradientDrawable)v.getBackground()).setColor(held.contains(scan) ? 0xf080552a : 0xf029333e);
        v.setOnClickListener(ignored -> { setKey(scan, !held.contains(scan)); rebuildTools(); });
        v.setContentDescription(label + ": нажмите для удержания, повторно для отпускания");
    }
    private TextView holdKey(LinearLayout row, String title, int scan) {
        TextView v = action(row, title, () -> tapKey(scan));
        Runnable repeat = new Runnable() {
            @Override public void run() { if (held.contains(scan)) { nativeKey(scan, true); v.postDelayed(this, 70); } }
        };
        v.setOnTouchListener((view, event) -> {
            int a = event.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN) { setKey(scan, true); v.setAlpha(1); if (scan == 0x0e) v.postDelayed(repeat, 350); }
            else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) {
                v.removeCallbacks(repeat); setKey(scan, false); v.setAlpha(opacity);
            }
            return true;
        });
        return v;
    }
    private void mouseButton(LinearLayout row, String title, int button) {
        TextView v = action(row, title, () -> { gestures.syncCursor(); clickMouse(button); });
        v.setContentDescription(title + ": удерживайте и двигайте курсор другим пальцем");
        v.setOnTouchListener((view, event) -> {
            int a = event.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN) { gestures.syncCursor(); mouseGate.hold(button, true); v.setAlpha(1); }
            else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) { mouseGate.hold(button, false); v.setAlpha(opacity); }
            return true;
        });
    }
    private void setKey(int scan, boolean down) {
        if (ready && (down ? held.add(scan) : held.remove(scan)))
            if (down || (!pressed.contains(scan) && !padKeys.contains(scan))) nativeKey(scan, down);
    }
    private void tapKey(int scan) { setKey(scan, true); root.postDelayed(() -> setKey(scan, false), 90); }
    private void clickMouse(int button) { mouseGate.click(button); }
    /* Touch gestures, the touchpad cursor's buttons and the radial menus. */
    private void releaseTouch() {
        root.removeCallbacks(longPress); if (gestures != null) gestures.cancel(); mouseGate.cancel();
        root.removeCallbacks(classification); if (semantic != null) semantic.abort();
        root.removeCallbacks(wheelHold);wheel=0;wheelSector=-1;
        updateMagnifier();
    }
    private void releaseControls() {
        if (!ready || root == null) return;
        stopGamepad();
        releaseTouch();
        if (mouse != null) mouse.cancel();
        dropMouseEdges();
        root.removeCallbacks(edgeExit);
        if (mouseMode) nativeMouseEdges(0); /* no edge scrolling behind Android dialogs */
        touch.invalidate();
        for (int scan : held) nativeKey(scan, false); held.clear();
        for (int scan : pressed) nativeKey(scan, false); pressed.clear();
    }

    /* ---- USB/Bluetooth gamepad ---- */
    private static boolean controller(InputDevice device) {
        return device != null && (device.supportsSource(InputDevice.SOURCE_GAMEPAD) ||
                (device.supportsSource(InputDevice.SOURCE_JOYSTICK) && device.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null &&
                        device.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) != null));
    }
    private void refreshGamepads() {
        // Device removal may happen while Settings has paused this activity and its listener.
        if (gamepadDevice != -1 && !controller(InputDevice.getDevice(gamepadDevice))) {
            stopGamepad(); gamepadDevice = -1;
        }
        boolean connected = false;
        for (int id : InputDevice.getDeviceIds()) if (controller(InputDevice.getDevice(id))) connected = true;
        if (connected != gamepadConnected) {
            gamepadConnected = connected;
            if (debugInput) Log.d(TAG, "gamepad " + (connected ? "connected" : "disconnected"));
            rebuildTools();
        }
    }
    @Override public void onInputDeviceAdded(int id) { refreshGamepads(); }
    @Override public void onInputDeviceChanged(int id) { onInputDeviceRemoved(id); refreshGamepads(); }
    @Override public void onInputDeviceRemoved(int id) {
        if (id == gamepadDevice) {
            boolean wasGamepad = gamepadMode;
            stopGamepad(); gamepadDevice = -1;
            if (wasGamepad) useMouse(false);
        }
        refreshGamepads();
        if (!gamepadConnected && gamepadHelp != null) gamepadHelp.dismiss();
    }
    private boolean claimGamepad(int id) {
        if (gamepadDevice == -1) { gamepad.reset(); gamepadDevice = id; }
        return gamepadDevice == id;
    }
    private void useGamepad() {
        if (gamepadMode || touch.getWidth() == 0 || touch.getHeight() == 0) return;
        boolean wasMouse = mouseMode;
        useMouse(true); mouse.cancel(); dropMouseEdges();
        if (!wasMouse) { mouseX = touch.x; mouseY = touch.y; }
        gamepadMode = true;
        nativeMouseWarp(mouseX / touch.getWidth(), mouseY / touch.getHeight()); nativeMouseEdges(0);
        updatePointerIcon();
        if (debugInput) Log.d(TAG, "gamepad mode");
    }
    private void stopGamepad() {
        if (gamepad == null) return;
        if (gamepadClick != null) { gamepadClick.setPressed(false); gamepadClick = null; }
        gamepad.cancel(); root.removeCallbacks(gamepadFrame);
        if (gamepadMode) { gamepadMode = false; dropMouseEdges(); nativeMouseEdges(0); touch.invalidate(); }
    }
    private void scheduleGamepad() {
        root.removeCallbacks(gamepadFrame);
        if (gamepadMode && gamepad.running()) root.postOnAnimation(gamepadFrame);
        touch.invalidate();
    }
    private float gamepadAxis(MotionEvent event, int axis) {
        InputDevice.MotionRange range = event.getDevice().getMotionRange(axis, event.getSource());
        return range == null ? 0 : GamepadControls.axis(event.getAxisValue(axis), range.getFlat());
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (!started || !event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.getActionMasked() != MotionEvent.ACTION_MOVE ||
                !controller(event.getDevice()) || !hasWindowFocus()) return super.dispatchGenericMotionEvent(event);
        if (!claimGamepad(event.getDeviceId())) return true;
        InputDevice.MotionRange z = event.getDevice().getMotionRange(MotionEvent.AXIS_Z, event.getSource());
        boolean standard = z != null && z.getMin() < 0 && event.getDevice().getMotionRange(MotionEvent.AXIS_RZ, event.getSource()) != null;
        float lx = gamepadAxis(event, MotionEvent.AXIS_X), ly = gamepadAxis(event, MotionEvent.AXIS_Y);
        float rx = gamepadAxis(event, standard ? MotionEvent.AXIS_Z : MotionEvent.AXIS_RX);
        float ry = gamepadAxis(event, standard ? MotionEvent.AXIS_RZ : MotionEvent.AXIS_RY);
        float lt = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE));
        float rt = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS));
        float hx = event.getAxisValue(MotionEvent.AXIS_HAT_X), hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (debugInput) Log.d(TAG, String.format(java.util.Locale.ROOT, "gamepad axes %.2f %.2f %.2f %.2f triggers %.2f %.2f hat %.0f %.0f", lx, ly, rx, ry, lt, rt, hx, hy));
        if (lx != 0 || ly != 0 || rx != 0 || ry != 0 || lt > .55f || rt > .55f || Math.abs(hx) > .5f || Math.abs(hy) > .5f) useGamepad();
        gamepad.motion(lx, ly, rx, ry, lt, rt, hx, hy, SystemClock.uptimeMillis());
        scheduleGamepad();
        return true;
    }
    /** Controller clicks can also reach the existing Android overlay buttons. */
    private View controlAt(View view, float x, float y) {
        if (view.getVisibility() != View.VISIBLE || x < 0 || y < 0 || x >= view.getWidth() || y >= view.getHeight()) return null;
        if (view instanceof android.view.ViewGroup) {
            android.view.ViewGroup group = (android.view.ViewGroup)view;
            for (int i = group.getChildCount() - 1; i >= 0; i--) {
                View child = group.getChildAt(i);
                View found = controlAt(child, x - child.getX(), y - child.getY());
                if (found != null) return found;
            }
        }
        return view.isEnabled() && view.isClickable() ? view : null;
    }
    private void showGamepadHelp() {
        if (gamepadHelp != null) { gamepadHelp.dismiss(); return; }
        releaseControls();
        LinearLayout columns = row(); columns.setPadding(dp(20), dp(4), dp(20), dp(4));
        String[] text = {
            "Левый стик — курсор\nПравый стик — камера\nA / B — ЛКМ / ПКМ (можно удерживать)\nX — рельеф · Y — пауза\nLB / RB — масштаб / прокрутка списка\nLT — точное наведение · RT — Shift\nКрестовина ↑ / ↓ — скорость\nКрестовина ← / → — поворот карты\nL3 — масштаб 100% · R3 — замок (F1)\nStart — меню · Select / Back — раскладка",
            "Удерживайте Select для комбинаций:\n↑ → ↓ ← — отряды 1–4\nA B X Y — отряды 5–8 · LB — отряд 9\nДобавьте RT — записать отряд\nДобавьте LT — точки карты 1–9\nLT + RT — записать точку карты\nRB — цвета радара · L3 — панель команд\nR3 — клавиатура · Start — настройки\n\nКурсор скрывается через 30 с бездействия.\nЗакрыть: A, B, Start или Select."
        };
        for (String value : text) {
            TextView column = Theme.text(this, value, 13, Theme.TEXT, false);
            column.setPadding(dp(4), 0, dp(12), 0); columns.addView(column, new LinearLayout.LayoutParams(0, -2, 1));
        }
        ScrollView scroll = new ScrollView(this); scroll.addView(columns);
        gamepadHelp = new AlertDialog.Builder(this).setTitle("Геймпад · раскладка").setView(scroll).setPositiveButton("Закрыть", null).create();
        boolean[] armed = {false};
        gamepadHelp.setOnKeyListener((dialog, code, event) -> {
            if (!controller(event.getDevice()) || !GamepadControls.handles(code)) return false;
            boolean close = code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BUTTON_START ||
                    code == KeyEvent.KEYCODE_BUTTON_SELECT || code == KeyEvent.KEYCODE_BUTTON_MODE || code == KeyEvent.KEYCODE_BACK;
            if (!close) return code < KeyEvent.KEYCODE_DPAD_UP || code > KeyEvent.KEYCODE_DPAD_RIGHT;
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) armed[0] = true;
            if (event.getAction() == KeyEvent.ACTION_UP && armed[0]) dialog.dismiss();
            return true;
        });
        gamepadHelp.setOnDismissListener(dialog -> { gamepadHelp = null; immersive(); });
        gamepadHelp.show();
        gamepadHelp.getWindow().setLayout(Math.round(root.getWidth() * .94f), -2);
    }

    /* ---- Vibration, engine cues and the placement loupe ---- */
    /* With a mouse nobody holds the phone: no vibration then. */
    private void haptic(int effect) {
        boolean play = vibration && !mouseMode && touch != null;
        if (play) touch.performHapticFeedback(effect);
        if (debugInput) Log.d(TAG, "haptic " + effect + (play ? "" : " (off)"));
    }
    private void watchFeedback() {
        feedbackUntil = SystemClock.uptimeMillis() + 300;
        if (!feedbackPosted && root != null) { feedbackPosted = true; root.postOnAnimation(feedbackPoll); }
    }
    /* A section change is felt a little stronger than the 100% zoom detent. */
    private void feedback() {
        if (!ready) return;
        int bits = nativeFeedback();
        if (bits == 0) return;
        if ((bits & FEEDBACK_PLACE) != 0) haptic(HapticFeedbackConstants.VIRTUAL_KEY);
        else if ((bits & FEEDBACK_SECTION) != 0) haptic(HapticFeedbackConstants.CONTEXT_CLICK);
        else if ((bits & FEEDBACK_ZOOM_SNAP) != 0) haptic(HapticFeedbackConstants.CLOCK_TICK);
    }
    private void updateMagnifier() {
        if (!ready || semantic == null || touch == null || root == null) return;
        long due = loupe && started && !mouseMode ? semantic.loupe(SystemClock.uptimeMillis()) : -1;
        root.removeCallbacks(magnifierDue);
        if (due > 0) root.postDelayed(magnifierDue, due);
        boolean show = due == 0;
        float x = semantic.fingerX(), y = semantic.fingerY();
        if (show == magnifierShown && (!show || (x == magnifierX && y == magnifierY))) return;
        if (debugInput && show != magnifierShown) Log.d(TAG, "loupe " + (show ? "on" : "off"));
        magnifierShown = show; magnifierX = x; magnifierY = y;
        int w = touch.getWidth(), h = touch.getHeight();
        if (w > 0 && h > 0) nativeMagnifier(show, x / w, y / h, loupeSize, loupeZoom);
    }

    /* ---- USB/Bluetooth mouse and keyboard ---- */
    /* A mouse takes the original desktop path (hover, edge scrolling, right
     * click); the next finger returns to direct touch. Each switch releases
     * what the other device held, so nothing stays pressed or clicks late. */
    private void useMouse(boolean on) {
        if (mouseMode == on || !started) return;
        if (on) { releaseTouch(); mouseModeAt = SystemClock.uptimeMillis(); updatePointerIcon(); }
        else {
            mouse.cancel(); dropMouseEdges();
            if (touchpad) gestures.place(mouseX, mouseY); /* the touchpad cursor continues from the mouse */
        }
        mouseMode = on;
        nativeTouch(SemanticTouchControls.MODE, 0, 0, 0, 0, 0, touchpad || on ? 0 : 1);
        if (debugInput) Log.d(TAG, on ? "mouse mode" : "touch mode");
        updateMagnifier(); touch.invalidate();
    }
    private void mouseButton(int button, boolean down) {
        if (!mouseEdges.isEmpty() || !nativeTouchDesktop()) {
            mouseEdges.add(new int[] {button, down ? 1 : 0});
            root.removeCallbacks(mouseEdgeFlush); root.postOnAnimation(mouseEdgeFlush);
        } else sendMouseButton(button, down);
    }
    private void sendMouseButton(int button, boolean down) {
        if (!down && (sentButtons & (1 << button)) == 0) return;
        sentButtons = down ? sentButtons | 1 << button : sentButtons & ~(1 << button);
        if (debugInput) Log.d(TAG, "button " + button + (down ? " down" : " up"));
        nativeMouseButton(button, down);
    }
    /* Edges not yet delivered are dropped; buttons already pressed are released. */
    private void dropMouseEdges() {
        mouseEdges.clear(); if (root != null) root.removeCallbacks(mouseEdgeFlush);
        for (int button = 0; button < 3; button++) if ((sentButtons & (1 << button)) != 0) sendMouseButton(button, false);
    }
    private boolean mouseEvent(MotionEvent e) {
        if (!started) return true;
        int action = e.getActionMasked(), state = e.getButtonState();
        float x = e.getX(), y = e.getY();
        if ((state & HardwareInput.RIGHT) != 0 || (e.getActionButton() & HardwareInput.RIGHT) != 0) secondaryAt = SystemClock.uptimeMillis();
        /* Enter/exit also follow window changes: only real activity takes over from touch.
         * MIUI's edge-gesture windows swallow a mouse before the screen edge:
         * leaving towards one keeps the map scrolling there until it returns. */
        root.removeCallbacks(edgeExit); /* a button press also starts with an exit */
        if (action == MotionEvent.ACTION_HOVER_EXIT) {
            exitEdges = mouseMode && !overControl(x, y) ? HardwareInput.exitEdges(x, touch.getWidth()) : 0;
            if (exitEdges != 0) root.postDelayed(edgeExit, 40);
            return true;
        }
        if (action == MotionEvent.ACTION_HOVER_ENTER && !mouseMode) return true;
        stopGamepad();
        useMouse(true);
        /* The radial menus keep working with left clicks; any other button closes them. */
        if (wheel != 0 || (action == MotionEvent.ACTION_DOWN && (state & HardwareInput.LEFT) != 0 && overWheelButton(x, y))) {
            if ((state & ~HardwareInput.LEFT & 7) != 0) { wheel = 0; root.removeCallbacks(wheelHold); touch.invalidate(); }
            else if (action != MotionEvent.ACTION_HOVER_MOVE && action != MotionEvent.ACTION_HOVER_ENTER &&
                     action != MotionEvent.ACTION_SCROLL) { wheelPointer(e); return true; }
        }
        if (action == MotionEvent.ACTION_CANCEL) { mouse.cancel(); dropMouseEdges(); return true; }
        if (action == MotionEvent.ACTION_SCROLL) { mouse.scroll(x, y, e.getAxisValue(MotionEvent.AXIS_VSCROLL), e.getEventTime()); return true; }
        mouse.pointer(x, y, state);
        mouseX = x; mouseY = y;
        return true;
    }
    /* The original draws no cursor: it hands Windows the cursor of its style
     * (Меч: sword.ani; Стрелка: the system arrow), which nothing draws here.
     * The mouse pointer over the game shows that image instead, scaled like
     * the game's frame; anything unreadable shows Android's own arrow. */
    private void updatePointerIcon() {
        if (touch == null || !started || requestedWidth <= 0 || touch.getWidth() <= 0) return;
        /* A choice just made in the Android settings may not have reached the game yet. */
        int[] values = preferences.contains("game-3") ? null : nativeSettings();
        int style = values != null ? values[3] : preferences.getInt("game-3", preferences.getInt("last-game-3", 2));
        float fit = Math.min((float)touch.getWidth() / requestedWidth, (float)touch.getHeight() / requestedHeight);
        int size = Math.round(32 * fit);
        if (style == pointerStyle && size == pointerSize) return;
        pointerStyle = style; pointerSize = size;
        touch.cursorImage = null;
        PointerIcon icon = cursorIcon(style == 1 ? "arrow.cur" : "sword.ani", fit);
        touch.setPointerIcon(icon != null ? icon : PointerIcon.getSystemIcon(this, PointerIcon.TYPE_ARROW));
        if (debugInput) Log.d(TAG, "pointer " + (icon != null ? "sword " + size + " px" : "arrow") + ", style " + style);
    }
    private PointerIcon cursorIcon(String name, float scale) {
        File[] files = new File(gamePath).listFiles();
        if (files != null) for (File file : files) if (file.getName().equalsIgnoreCase(name)) try {
            GameCursor cursor = GameCursor.decode(java.nio.file.Files.readAllBytes(file.toPath()));
            if (cursor == null) return null;
            android.graphics.Bitmap image = android.graphics.Bitmap.createBitmap(cursor.argb, cursor.width, cursor.height,
                    android.graphics.Bitmap.Config.ARGB_8888);
            image = android.graphics.Bitmap.createScaledBitmap(image, Math.max(1, Math.round(cursor.width * scale)),
                    Math.max(1, Math.round(cursor.height * scale)), true);
            touch.cursorImage = image; touch.hotX = cursor.hotX * scale; touch.hotY = cursor.hotY * scale;
            return PointerIcon.create(image, Math.min(image.getWidth() - 1, cursor.hotX * scale),
                    Math.min(image.getHeight() - 1, cursor.hotY * scale));
        } catch (java.io.IOException | RuntimeException unreadable) { return null; }
        return null;
    }
    /* An Android control of ours under a view point (the pointer moved onto it). */
    private boolean overControl(float x, float y) {
        for (View v : new View[] {tools, mouseTools, switchButton, zoomIndicator, editorBar, hint, layoutEditor, gamepadHint})
            if (v != null && v.getParent() == root && v.getVisibility() == View.VISIBLE && v.getWidth() > 0 &&
                x >= v.getX() && x < v.getX() + v.getWidth() && y >= v.getY() && y < v.getY() + v.getHeight()) return true;
        return false;
    }
    private boolean overWheelButton(float x, float y) {
        if ((controlState[0] & 1) == 0) return false;
        for (int kind = 1; kind <= 2; kind++) if ((kind == 1 ? groupWheel : markerWheel) &&
                Math.hypot(x - wheelButtonX(kind), y - wheelButtonY(kind)) <= wheelButtonRadius() + dp(2)) return true;
        return false;
    }
    /* Physical keys enter the game as its own DirectInput keys, with Android's
     * auto-repeat like Windows'. Volume and other system keys stay with Android;
     * a BACK key (remote, gamepad) still opens the game menu through onBackPressed. */
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (started && hasWindowFocus() && controller(e.getDevice()) && GamepadControls.handles(code)) {
            if (!claimGamepad(e.getDeviceId())) return true;
            if (debugInput) Log.d(TAG, "gamepad key " + code + " action " + e.getAction() + " repeat " + e.getRepeatCount());
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) useGamepad();
            gamepad.key(code, e.getAction() == KeyEvent.ACTION_DOWN && !e.isCanceled(), e.getRepeatCount(), SystemClock.uptimeMillis());
            scheduleGamepad();
            return true;
        }
        /* Android may answer a secondary click with BACK: the right button is the game's. */
        if (code == KeyEvent.KEYCODE_BACK && mouseMode && SystemClock.uptimeMillis() - secondaryAt < 500) return true;
        int scan = ready && root != null ? HardwareInput.scanCode(code) : 0;
        if (scan == 0) return super.dispatchKeyEvent(e);
        if (e.getAction() == KeyEvent.ACTION_DOWN) { pressed.add(scan); nativeKey(scan, true); }
        else if (e.getAction() == KeyEvent.ACTION_UP && pressed.remove(scan) && !held.contains(scan) && !padKeys.contains(scan)) nativeKey(scan, false);
        return true;
    }
    private boolean pointer(MotionEvent e) {
        int action = e.getActionMasked(); long time = e.getEventTime();
        if (HardwareInput.isMouse(e.getSource(), e.getToolType(0))) return mouseEvent(e);
        if (action == MotionEvent.ACTION_DOWN) stopGamepad();
        if (mouseMode && action == MotionEvent.ACTION_DOWN) useMouse(false);
        boolean handled = touchPointer(e, action, time);
        updateMagnifier();
        return handled;
    }
    private boolean touchPointer(MotionEvent e, int action, long time) {
        if(wheelPointer(e))return true;
        if (!touchpad) {
            if (action == MotionEvent.ACTION_DOWN) {
                root.removeCallbacks(classification);
                semantic.down(e.getX(), e.getY(), time); root.postOnAnimation(classification);
                root.postDelayed(longPress, SemanticTouchControls.LONG_HOLD_MS);
            } else if (action == MotionEvent.ACTION_POINTER_DOWN) {
                root.removeCallbacks(longPress);
                if (e.getPointerCount() == 2) semantic.secondDown(e.getX(0), e.getY(0), e.getX(1), e.getY(1), time);
                else semantic.abort();
            } else if (action == MotionEvent.ACTION_MOVE) {
                if (e.getPointerCount() == 1) semantic.move(e.getX(), e.getY(), time);
                else if (e.getPointerCount() == 2) semantic.pairMove(e.getX(0), e.getY(0), e.getX(1), e.getY(1));
            } else if (action == MotionEvent.ACTION_POINTER_UP) {
                root.removeCallbacks(longPress); semantic.pointerUp(time);
            } else if (action == MotionEvent.ACTION_UP) {
                root.removeCallbacks(longPress); semantic.up(e.getX(), e.getY(), time);
            } else if (action == MotionEvent.ACTION_CANCEL) {
                root.removeCallbacks(longPress); root.removeCallbacks(classification); semantic.abort();
            }
            touch.invalidate(); return true;
        }
        if (action == MotionEvent.ACTION_DOWN) {
            if (!touchpad) mouseGate.cancel();
            gestures.down(e.getX(), e.getY(), time); root.postDelayed(longPress, 500);
        } else if (action == MotionEvent.ACTION_POINTER_DOWN) {
            root.removeCallbacks(longPress); mouseGate.cancel();
            if (e.getPointerCount() == 2) gestures.secondDown(e.getX(0), e.getY(0), e.getX(1), e.getY(1), time);
            else { gestures.cancel(); gestures.pointerUp(time); }
        } else if (action == MotionEvent.ACTION_MOVE) {
            if (e.getPointerCount() == 1) gestures.move(e.getX(), e.getY());
            else if (e.getPointerCount() == 2) gestures.pairMove(e.getX(0), e.getY(0), e.getX(1), e.getY(1));
        } else if (action == MotionEvent.ACTION_POINTER_UP) {
            // Finish the pair before Android renumbers pointers; ignore its surviving finger.
            root.removeCallbacks(longPress); gestures.pointerUp(time);
        } else if (action == MotionEvent.ACTION_UP) {
            root.removeCallbacks(longPress); gestures.up(e.getX(), e.getY(), time);
        } else if (action == MotionEvent.ACTION_CANCEL) { root.removeCallbacks(longPress); gestures.cancel(); mouseGate.cancel(); }
        return true;
    }
    private float wheelButtonRadius() { return dp(25 * controls()); }
    private float wheelButtonX(int kind) {
        float[] position = saved("wheel" + kind);
        return position != null ? position[0] * touch.getWidth() : dp((kind == 1 ? 34 : 96) * controls());
    }
    private float wheelButtonY(int kind) {
        float[] position = saved("wheel" + kind);
        return position != null ? position[1] * touch.getHeight() : Math.max(dp(32), hudPoint(0,452)[1] - dp(34 * controls()));
    }
    private void openWheel(int kind,float x,float y) {
        semantic.abort();gestures.cancel();mouseGate.cancel();root.removeCallbacks(longPress);root.removeCallbacks(classification);
        wheel=kind;wheelSector=-1;wheelDragged=wheelPinned=false;
        wheelDownX=x;wheelDownY=y;
        wheelAssign=kind==1&&controlState[1]>0;
        wheelRadius=Math.min(dp(98*Math.max(1,controls())),Math.min(touch.getWidth(),touch.getHeight())/2f-dp(12));
        float margin=wheelRadius+dp(6);
        wheelX=Math.max(margin,Math.min(touch.getWidth()-margin,x));wheelY=Math.max(margin,Math.min(touch.getHeight()-margin,y));
        root.postDelayed(wheelHold,300);haptic(HapticFeedbackConstants.VIRTUAL_KEY);touch.invalidate();
    }
    private boolean wheelAvailable(int sector) {
        return sector>=0&&(wheelAssign||(controlState[wheel==1?2:3]&(1<<sector))!=0);
    }
    private void chooseWheel(int sector) {
        if(wheelAvailable(sector))nativeCommand('1'+sector,wheel==1?(wheelAssign?1:0):(wheelAssign?3:2));
        wheel=0;wheelSector=-1;root.removeCallbacks(wheelHold);touch.invalidate();
    }
    private boolean wheelPointer(MotionEvent e) {
        int action=e.getActionMasked();float x=e.getX(),y=e.getY();
        if(wheel==0) {
            if(action!=MotionEvent.ACTION_DOWN||(controlState[0]&1)==0)return false;
            for(int kind=1;kind<=2;kind++)if((kind==1?groupWheel:markerWheel)&&
                    Math.hypot(x-wheelButtonX(kind),y-wheelButtonY(kind))<=wheelButtonRadius()+dp(2)) {openWheel(kind,x,y);return true;}
            return false;
        }
        if(action==MotionEvent.ACTION_CANCEL||action==MotionEvent.ACTION_POINTER_DOWN) {
            wheel=0;root.removeCallbacks(wheelHold);touch.invalidate();return true;
        }
        if(action==MotionEvent.ACTION_MOVE||action==MotionEvent.ACTION_DOWN) {
            if(wheelPinned||Math.hypot(x-wheelDownX,y-wheelDownY)>ViewConfiguration.get(this).getScaledTouchSlop()) {
                int sector=TouchControls.radialSector(x,y,wheelX,wheelY,wheelRadius);
                /* A light tick as the finger crosses into another usable sector. */
                if(sector!=wheelSector&&wheelAvailable(sector))haptic(HapticFeedbackConstants.CLOCK_TICK);
                wheelDragged=true;wheelSector=sector;
            }
        } else if(action==MotionEvent.ACTION_UP) {
            int sector=TouchControls.radialSector(x,y,wheelX,wheelY,wheelRadius);
            if(!wheelPinned&&e.getEventTime()-e.getDownTime()>=300)wheelAssign=wheel==2;
            root.removeCallbacks(wheelHold);
            // Clamping the wheel away from the edge must not turn an unmoved opener tap into a sector choice.
            if(!wheelPinned&&!wheelDragged){wheelPinned=true;wheelSector=-1;}
            else if(sector>=0)chooseWheel(sector);
            else{wheel=0;wheelSector=-1;}
        }
        touch.invalidate();return true;
    }
    /* Drag mode for the Android controls: the game receives no touches meanwhile. */
    private final class LayoutEditor extends View {
        private final Paint pen = new Paint(Paint.ANTI_ALIAS_FLAG);
        private View dragged; private String draggedKey; private int draggedWheel;
        private float grabX, grabY, lastLeft, lastTop;
        LayoutEditor() { super(GameActivity.this); setContentDescription("Расположение кнопок: перетащите элементы"); arrow(this); }
        private View[] views() { return new View[] {tools, mouseTools, switchButton}; }
        private String key(View view) { return view == tools ? "tools" : view == mouseTools ? "mouse" : "switch"; }
        private boolean shown(View view) { return view != null && view.getParent() == root && view.getVisibility() == VISIBLE && view.getWidth() > 0; }
        @Override protected void onDraw(Canvas canvas) {
            canvas.drawColor(0x66000000);
            pen.setStyle(Paint.Style.STROKE); pen.setStrokeWidth(dp(2)); pen.setColor(0xffffd45b);
            pen.setPathEffect(new DashPathEffect(new float[] {dp(7), dp(5)}, 0));
            for (View view : views()) if (shown(view))
                canvas.drawRect(view.getLeft() - dp(3), view.getTop() - dp(3), view.getRight() + dp(3), view.getBottom() + dp(3), pen);
            for (int kind = 1; kind <= 2; kind++) if (kind == 1 ? groupWheel : markerWheel)
                canvas.drawCircle(wheelButtonX(kind), wheelButtonY(kind), wheelButtonRadius() + dp(4), pen);
            pen.setPathEffect(null);
        }
        @Override public boolean onTouchEvent(MotionEvent event) {
            float x = event.getX(), y = event.getY();
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                dragged = null; draggedWheel = 0;
                for (int kind = 1; kind <= 2; kind++) if ((kind == 1 ? groupWheel : markerWheel) &&
                        Math.hypot(x - wheelButtonX(kind), y - wheelButtonY(kind)) <= wheelButtonRadius() + dp(10)) {
                    draggedWheel = kind; grabX = x - wheelButtonX(kind); grabY = y - wheelButtonY(kind);
                }
                if (draggedWheel == 0) for (View view : views()) if (shown(view) && x >= view.getLeft() && x <= view.getRight() &&
                        y >= view.getTop() && y <= view.getBottom()) {
                    dragged = view; draggedKey = key(view); grabX = x - view.getLeft(); grabY = y - view.getTop();
                    lastLeft = view.getLeft(); lastTop = view.getTop();
                }
                if (dragged != null || draggedWheel != 0) haptic(HapticFeedbackConstants.VIRTUAL_KEY);
            } else if (action == MotionEvent.ACTION_MOVE) {
                if (dragged != null) {
                    lastLeft = Math.max(0, Math.min(root.getWidth() - dragged.getWidth(), x - grabX));
                    lastTop = Math.max(0, Math.min(root.getHeight() - dragged.getHeight(), y - grabY));
                    moveView(dragged, lastLeft, lastTop);
                } else if (draggedWheel != 0) {
                    float r = wheelButtonRadius();
                    float cx = Math.max(r, Math.min(touch.getWidth() - r, x - grabX)), cy = Math.max(r, Math.min(touch.getHeight() - r, y - grabY));
                    save("wheel" + draggedWheel, cx / touch.getWidth(), cy / touch.getHeight());
                    touch.invalidate();
                }
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                if (dragged != null && root.getWidth() > 0) save(draggedKey, lastLeft / root.getWidth(), lastTop / root.getHeight());
                dragged = null; draggedWheel = 0;
            }
            invalidate(); return true;
        }
    }
    private final class PointerView extends View {
        float x, y; private final Paint pen = new Paint(Paint.ANTI_ALIAS_FLAG);
        android.graphics.Bitmap cursorImage;
        float hotX, hotY;
        PointerView() {
            super(GameActivity.this); setContentDescription("Игровое поле: касание, выделение перетаскиванием, два пальца для карты");
            setPointerIcon(PointerIcon.getSystemIcon(GameActivity.this, PointerIcon.TYPE_ARROW)); /* until the game's cursor is read */
        }
        @Override public void onInitializeAccessibilityNodeInfo(android.view.accessibility.AccessibilityNodeInfo info) {
            super.onInitializeAccessibilityNodeInfo(info);
            if((controlState[0]&1)==0)return;
            String[] labels={"Вызвать отряд","Записать выделенный отряд","Перейти к точке карты","Записать точку карты"};
            for(int i=0;i<4;i++)if((i<2?groupWheel:markerWheel)&&(i!=1||controlState[1]>0))
                info.addAction(new android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction(0x01020000+i,labels[i]));
        }
        @Override public boolean performAccessibilityAction(int action,Bundle args) {
            int index=action-0x01020000;
            if(index<0||index>3)return super.performAccessibilityAction(action,args);
            int kind=index<2?1:2;
            if((controlState[0]&1)==0||!(kind==1?groupWheel:markerWheel)||(index==1&&controlState[1]==0))return false;
            releaseControls();wheel=kind;wheelAssign=(index&1)!=0;
            java.util.ArrayList<Integer> slots=new java.util.ArrayList<>();
            java.util.ArrayList<String> labels=new java.util.ArrayList<>();
            for(int i=0;i<9;i++)if(wheelAvailable(i)){slots.add(i);labels.add(Integer.toString(i+1));}
            new AlertDialog.Builder(GameActivity.this).setTitle(wheelAssign?"Записать в ячейку":"Выбрать ячейку")
                    .setItems(labels.toArray(new String[0]),(dialog,n)->chooseWheel(slots.get(n)))
                    .setNegativeButton("Отмена",(dialog,n)->chooseWheel(-1)).setOnCancelListener(dialog->chooseWheel(-1)).show();
            return true;
        }
        @Override protected void onDraw(Canvas canvas) {
            int alpha=Math.round(255*opacity);
            if((controlState[0]&1)!=0||layoutEdit)for(int kind=1;kind<=2;kind++)if(kind==1?groupWheel:markerWheel) {
                float bx=wheelButtonX(kind),by=wheelButtonY(kind),r=wheelButtonRadius(),s=controls();
                pen.setStyle(Paint.Style.FILL);pen.setColor(0xf029333e);pen.setAlpha(alpha);canvas.drawCircle(bx,by,r,pen);
                pen.setColor(0xffffe08a);pen.setAlpha(alpha);pen.setTextSize(dp(11*s));pen.setTextAlign(Paint.Align.CENTER);
                canvas.drawText(kind==1?"Отряд":"Точки",bx,by+dp(4*s),pen);
                pen.setTextSize(dp(9*s));canvas.drawText(Integer.toString(Integer.bitCount(controlState[kind==1?2:3])),bx+dp(18*s),by-dp(15*s),pen);
                pen.setAlpha(255);
            }
            if(wheel!=0) {
                pen.setStyle(Paint.Style.FILL);pen.setTextAlign(Paint.Align.CENTER);
                android.graphics.RectF bounds=new android.graphics.RectF(wheelX-wheelRadius,wheelY-wheelRadius,wheelX+wheelRadius,wheelY+wheelRadius);
                for(int i=0;i<9;i++) {
                    boolean occupied=(controlState[wheel==1?2:3]&(1<<i))!=0,available=wheelAvailable(i);
                    pen.setColor(i==wheelSector&&available?0xffb87b2d:!available?0xe51c2228:
                            wheelAssign&&!occupied?0xff586d32:occupied?0xff435f78:0xe529333e);
                    canvas.drawArc(bounds,-110+i*40,39,true,pen);
                    double a=i*Math.PI*2/9-Math.PI/2;
                    pen.setColor(available?0xffffedb4:0xff697078);pen.setTextSize(dp(20*Math.max(1,controls())));
                    canvas.drawText(Integer.toString(i+1),wheelX+(float)Math.cos(a)*wheelRadius*.74f,wheelY+(float)Math.sin(a)*wheelRadius*.74f+dp(7),pen);
                }
                pen.setColor(0xff18212a);canvas.drawCircle(wheelX,wheelY,wheelRadius*.44f,pen);
                pen.setColor(0xffffe08a);pen.setTextSize(dp(11));
                canvas.drawText(wheelAssign?"Записать":"Вызвать",wheelX,wheelY-dp(2),pen);
                pen.setTextSize(dp(9));canvas.drawText("Центр: отмена",wheelX,wheelY+dp(13),pen);
            }
            pen.setTextAlign(Paint.Align.LEFT);
            boolean padCursor = gamepadMode && gamepad.cursorVisible(SystemClock.uptimeMillis());
            if (padCursor && cursorImage != null) { pen.setStyle(Paint.Style.FILL); pen.setAlpha(255); canvas.drawBitmap(cursorImage, x-hotX, y-hotY, pen); }
            if ((padCursor && cursorImage == null) || (touchpad && !mouseMode)) {
                float r = dp(9); pen.setStyle(Paint.Style.STROKE); pen.setStrokeWidth(dp(3)); pen.setColor(0xff000000);
                canvas.drawCircle(x, y, r, pen); canvas.drawLine(x-r-dp(4), y, x+r+dp(4), y, pen); canvas.drawLine(x, y-r-dp(4), x, y+r+dp(4), pen);
                pen.setStrokeWidth(dp(1)); pen.setColor(0xffffe08a);
                canvas.drawCircle(x, y, r, pen); canvas.drawLine(x-r-dp(4), y, x+r+dp(4), y, pen); canvas.drawLine(x, y-r-dp(4), x, y+r+dp(4), pen);
            }
            if (diagnostics && semantic != null) {
                String state = "Touch #" + semantic.sequence() + " " + semantic.phase() + " / " + semantic.classificationName() +
                        (magnifierShown ? " · лупа" : "");
                String point = gamepadMode ? String.format(java.util.Locale.ROOT, "Геймпад %.0f, %.0f", mouseX, mouseY) : mouseMode ? "Мышь" : touchpad ? "Тачпад" :
                        String.format(java.util.Locale.ROOT, "Палец %.0f, %.0f", semantic.fingerX(), semantic.fingerY());
                pen.setStyle(Paint.Style.FILL); pen.setTextSize(dp(12));
                float w = Math.max(dp(230), pen.measureText(state) + dp(16));
                float l = Math.max(dp(4), getWidth() - w - dp(60)), t = dp(6);
                pen.setColor(0xdd101820); canvas.drawRect(l,t,l+w,t+dp(46),pen);
                pen.setColor(0xffffffff); canvas.drawText(state,l+dp(8),t+dp(18),pen); canvas.drawText(point,l+dp(8),t+dp(36),pen);
                if (!touchpad && semantic.active()) {
                    pen.setStyle(Paint.Style.STROKE); pen.setStrokeWidth(dp(2)); pen.setColor(0xffffd45b);
                    canvas.drawCircle(semantic.fingerX(),semantic.fingerY(),dp(7),pen);
                }
            }
        }
    }
    static int[] gameSettings() { return loadedEdition == null ? null : nativeSettings(); }
    static void gameSetting(int index, int value) { if (loadedEdition != null) nativeSetting(index, value); }
    static boolean gameRunning() { return loadedEdition != null; }
    private void applyGameSettings() {
        for (int i = 0; i < 9; i++) if (preferences.contains("game-" + i)) nativeSetting(i, preferences.getInt("game-" + i, 0));
    }
    /* In-game changes (numpad speed, original options) become the stored values,
     * so a later start or resume cannot restore a stale Android copy over them. */
    private void rememberGameSettings() {
        int[] values = started ? nativeSettings() : null;
        if (values == null || preferences == null) return;
        SharedPreferences.Editor edit = preferences.edit();
        for (int i = 0; i < values.length; i++) {
            edit.putInt("last-game-" + i, values[i]);
            if (preferences.contains("game-" + i)) edit.putInt("game-" + i, values[i]);
        }
        edit.apply();
    }
    private void settings() {
        settings("");
    }
    private static final int UI_SETTINGS_GRAPHICS = 64, UI_SETTINGS_AUDIO = 128;
    private void settings(String section) {
        releaseControls();
        rememberGameSettings();
        startActivity(new Intent(this, SettingsActivity.settingsClass(edition)).putExtra("section", section));
    }
    private void help() {
        releaseControls();
        if (GameFiles.original(edition)) {
            new AlertDialog.Builder(this).setTitle("Управление · Stronghold HD")
                    .setMessage("На карте тап выбирает войска или отдаёт приказ, сдвиг пальца двигает камеру. Чтобы выделить войска рамкой или провести стену, удерживайте палец около трети секунды, затем тяните. Удержание отменяет постройку или выделение.\n\nДва пальца: сдвиг — камера, сведение или разведение — оригинальное переключение приближения (Z), поворот примерно на четверть оборота — поворот карты (C / X), короткий тап — отмена. Удержание радара переключает рельеф, короткий тап двумя пальцами над ним — окраску «свои / чужие».\n\nВ меню и окнах тап нажимает кнопку или открывает клавиатуру для текстового поля; перетаскивание тянет ползунки и списки. Удержание книги писаря открывает быстрые команды и настройки. Android «Назад» открывает меню игры (Esc).\n\nСвайп по панели построек переключает доступные разделы. Выбранная постройка подсвечивается, рядом показываются её название и стоимость. Лупа при выборе места выключена по умолчанию; её размер и приближение меняются в настройках.\n\nГруппы и точки карты включаются в настройках. Группы: тап при выбранных войсках — записать, удержание — вызвать; повторный вызов центрирует камеру. Точки: тап — перейти, удержание — записать текущий вид. Центр или область за кругом отменяет выбор.\n\nВ настройках доступны скорость игры, звук, прокрутка, размер интерфейса, чёткий текст, тачпад и расположение кнопок. «Во весь экран» выбирает пропорции устройства. Кнопка «⇄» открывает переход между играми и лаунчером.")
                    .setPositiveButton("Понятно", null).show();
            return;
        }
        new AlertDialog.Builder(this).setTitle("Управление")
                .setMessage("На карте: тап — выбор или приказ, двойной тап — отряд одного типа. Сдвиг пальца двигает камеру; для рамки сначала удерживайте палец около трети секунды, затем тяните.\n\nУдержание на карте отменяет выделение, постройку или способность; если отменять нечего — выравнивает рельеф. Удержание радара тоже переключает рельеф.\n\nДва пальца двигают камеру и меняют приближение. Поворот двумя пальцами примерно на четверть оборота поворачивает карту на 90°. Короткий тап двумя пальцами — отмена; над радаром — окраска «свои / чужие». Тап по значению масштаба над радаром возвращает 100%.\n\nВ меню тап нажимает кнопку, удержание показывает описание; слайдеры и списки можно тянуть. Удержание книги писаря открывает дополнительные команды и настройки; в редакторе удерживайте перо слева от панели. Кнопка «Расположение» позволяет перетащить панели и кнопки. Android «Назад» открывает игровое меню.\n\nГруппы и точки карты включаются в настройках. Группы: тап при выбранных войсках — записать, удержание — вызвать; повторный вызов центрирует камеру. Точки: тап — перейти, удержание — записать текущий вид. Центр или область за кругом отменяет выбор.\n\nСвайп по панели построек листает доступные разделы: влево — следующий, вправо — предыдущий; из подраздела свайп в любую сторону открывает следующий раздел. Выбранная постройка подсвечивается на своей кнопке.\n\nЛупа: пока палец выбирает место для постройки, стены, сноса или способности, рядом показывается увеличенный участок карты. Короткий тап ставит сразу, без лупы.\n\nВибрация отмечает привязку масштаба к 100%, смену раздела построек, удержание, поворот и секторы радиального меню. Лупу и вибрацию можно выключить в настройках.\n\nМышь и клавиатура (USB или Bluetooth): с мышью игра работает как на ПК — наведение, ЛКМ и ПКМ, прокрутка камеры у края экрана. Колесо в матче меняет масштаб вокруг курсора, в меню и списках прокручивает их. Перетаскивание средней кнопкой двигает карту, щелчок средней кнопкой возвращает 100%. Клавиши — как в оригинале: Esc — меню, пробел — рельеф, +/− цифрового блока — скорость, X/C — поворот. Касание экрана снова включает сенсорное управление.\n\nТачпад — дополнительный режим курсора с ЛКМ/ПКМ.")
                .setPositiveButton("Понятно", null).show();
    }
    private void showTextInput() { nativeEditorRequest(); }
    private void showEditor() {
        if (textDialog) return;
        String[] editor = nativeEditor();
        if (editor == null) return;
        final int token = Integer.parseInt(editor[1]);
        textDialog = true;
        releaseControls(); EditText input = new EditText(this); input.setSingleLine(true);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        input.setFilters(new InputFilter[] {new InputFilter.LengthFilter(Integer.parseInt(editor[2]))});
        {
            int bank = Integer.parseInt(editor[3]);
            // An IPv4 address needs several dots; the decimal-number key listener discards all but the first.
            if (bank == 5) input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
            else if (bank == 6 || bank == 7) input.setInputType(InputType.TYPE_CLASS_NUMBER);
            input.setText(editor[0]); input.setSelection(input.length());
        }
        input.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI); input.setHint("Текст в выбранное поле");
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("Ввод текста").setView(input)
                .setPositiveButton("Готово", null).setNegativeButton("Отмена", null).create();
        dialog.setOnShowListener(ignored -> {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                if (nativeEdit(input.getText().toString(), token)) dialog.dismiss();
                else Toast.makeText(this, "Не удалось передать текст. Повторите.", Toast.LENGTH_SHORT).show();
            }); input.requestFocus();
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            input.postDelayed(() -> ((android.view.inputmethod.InputMethodManager)getSystemService(INPUT_METHOD_SERVICE))
                    .showSoftInput(input, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT), 150);
        });
        input.setOnEditorActionListener((view, action, event) -> {
            /* A hardware keyboard's Enter arrives without an IME action. */
            boolean enter = event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER;
            if (action != EditorInfo.IME_ACTION_DONE && !enter) return false;
            if (!enter || event.getAction() == KeyEvent.ACTION_DOWN) dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
            return true;
        });
        dialog.setOnDismissListener(ignored -> { textDialog = false; immersive(); });
        dialog.show();
    }
    private void chooseEdition() {
        releaseControls();
        startActivity(launcherIntent());
    }
    private Intent launcherIntent() {
        return new Intent(this, LauncherActivity.class).putExtra("showLauncher", true).putExtra("edition", edition)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
    }
    private void launchError(String message) {
        startActivity(launcherIntent().putExtra("error", message));
        finishAndRemoveTask(); android.os.Process.killProcess(android.os.Process.myPid());
    }
    private void returnToLauncher() {
        releaseControls();
        if (preferences != null) preferences.edit().putBoolean("extra", extra).commit();
        GameFiles.pending(getFilesDir(), edition).delete();
        startActivity(launcherIntent());
        finishAndRemoveTask(); android.os.Process.killProcess(android.os.Process.myPid());
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String nextPath = intent.getStringExtra("game");
        if (gamePath != null && gamePath.equals(nextPath) && intent.getBooleanExtra("skipMovies", false) == skipMovies)
            rememberedSuccess = false;
        if (nextPath != null && (!nextPath.equals(gamePath) || intent.getBooleanExtra("skipMovies", false) != skipMovies)) {
            new AlertDialog.Builder(this).setTitle("Нужен перезапуск игры")
                    .setMessage("Параметры запуска изменены. Если матч ещё не сохранён, вернитесь в игру и сохраните его. Затем закройте игру и запустите её из лаунчера.")
                    .setPositiveButton("Закрыть игру", (d, which) -> returnToLauncher())
                    .setNegativeButton("Вернуться в игру", null).show();
        }
    }
    private void immersive() { getWindow().getDecorView().setSystemUiVisibility(5894); }
    @Override public void surfaceCreated(SurfaceHolder holder) { if (ready) nativeSurface(holder.getSurface()); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) { applyCutout(); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { if (ready) nativeSurface(null); }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) { immersive(); applyCutout(); if (root != null) rebuildTools(); } else releaseControls();
    }
    @Override protected void onPause() {
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        if (root != null) root.removeCallbacks(zoomRefresh); releaseControls();
        if (ready && preferences != null) { rememberGameSettings(); preferences.edit().putBoolean("extra", extra).apply(); }
        if(lanDiscovery!=null){lanDiscovery.release();lanDiscovery=null;}
        if (ready) nativePause(true); super.onPause();
    }
    @Override protected void onResume() {
        super.onResume();
        if (inputManager != null) { inputManager.registerInputDeviceListener(this, null); refreshGamepads(); }
        if (root != null) discoverEditions();
        if (ready) nativePause(false);
        if (ready && preferences != null) {
            boolean oldWide = wide; String oldResolution = preferences.getString("activeResolution", "auto");
            /* Game options changed in Android settings were applied there, immediately. */
            readPreferences(); applyFilters();
            nativeTouch(SemanticTouchControls.UI_SCALE, 0, 0, 0, 0, 0, uiScale / 100f);
            nativeTouch(SemanticTouchControls.MODE, 0, 0, 0, 0, 0, touchpad || mouseMode ? 0 : 1);
            perfAt = 0; /* frames around the pause are not a sample */
            if (mouseMode) updatePointerIcon();
            if (gestures != null) gestures.mode(touchpad, sensitivity);
            if (semantic != null) semantic.buildingSwipes(preferences.getBoolean("buildingSwipes",true));
            if (started && (oldWide != wide || !oldResolution.equals(preferences.getString("resolution", "auto"))))
                Toast.makeText(this, "Разрешение изменится при новом запуске игры. Сначала сохраните матч.", Toast.LENGTH_LONG).show();
            if (preferences.getBoolean("layoutEditRequest", false)) {
                preferences.edit().remove("layoutEditRequest").apply();
                layoutEdit = false; if (root != null) root.post(() -> editLayout(true));
            }
            if (root != null) rebuildTools();
        }
        if (root != null) { root.removeCallbacks(zoomRefresh); root.post(zoomRefresh); }
    }
    @Override public void onBackPressed() {
        if (layoutEdit) editLayout(false);
        else if (root != null) tapKey(0x01);
        else returnToLauncher();
    }
}
