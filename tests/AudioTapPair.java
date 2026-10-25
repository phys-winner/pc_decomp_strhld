import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import java.lang.reflect.Method;

/** Two QA taps in one shell VM, avoiding the input command's 3-second startup between taps. */
public final class AudioTapPair {
    private static Object manager;
    private static Method inject;
    private static void tap(float x, float y) throws Exception {
        long down = SystemClock.uptimeMillis();
        MotionEvent event = MotionEvent.obtain(down, down, MotionEvent.ACTION_DOWN, x, y, 0);
        event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        if (!(Boolean) inject.invoke(manager, event, 2)) throw new IllegalStateException("DOWN rejected");
        event.recycle();
        SystemClock.sleep(25);
        event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), MotionEvent.ACTION_UP, x, y, 0);
        event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        if (!(Boolean) inject.invoke(manager, event, 2)) throw new IllegalStateException("UP rejected");
        event.recycle();
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 5) throw new IllegalArgumentException("x1 y1 x2 y2 gap_ms");
        Class<?> type = Class.forName("android.hardware.input.InputManager");
        manager = type.getMethod("getInstance").invoke(null);
        inject = type.getMethod("injectInputEvent", InputEvent.class, int.class);
        long before = SystemClock.uptimeMillis();
        tap(Float.parseFloat(args[0]), Float.parseFloat(args[1]));
        SystemClock.sleep(Integer.parseInt(args[4]));
        long second = SystemClock.uptimeMillis();
        tap(Float.parseFloat(args[2]), Float.parseFloat(args[3]));
        System.out.println("AudioTapPair first=" + before + " second=" + second + " delta_ms=" + (second-before));
    }
}
