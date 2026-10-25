#ifndef STRONGHOLD_TOUCH_API_H
#define STRONGHOLD_TOUCH_API_H

/* JNI only queues events; engine decisions run on the guest game thread. */
enum stronghold_touch_action {
    SH_TOUCH_QUERY = 0,
    SH_TOUCH_TAP = 1,
    SH_TOUCH_DOUBLE_TAP = 2,
    SH_TOUCH_BOX_BEGIN = 3,
    SH_TOUCH_BOX_UPDATE = 4,
    SH_TOUCH_BOX_END = 5,
    SH_TOUCH_PAN = 6,
    SH_TOUCH_TRANSFORM = 7,
    SH_TOUCH_AIM_BEGIN = 8,
    SH_TOUCH_AIM_UPDATE = 9,
    SH_TOUCH_AIM_COMMIT = 10,
    SH_TOUCH_CANCEL = 11,
    SH_TOUCH_UI_CLICK = 12,
    SH_TOUCH_UI_HOLD_BEGIN = 13,
    SH_TOUCH_UI_HOLD_END = 14,
    SH_TOUCH_UI_SCROLL = 15,
    SH_TOUCH_ABORT = 16,
    SH_TOUCH_UI_SCALE = 17,
    SH_TOUCH_MODE = 18,
    SH_TOUCH_UI_HOVER = 19,
    SH_TOUCH_OPEN_TOOLS = 20,
    SH_TOUCH_RADAR_TOGGLE = 21,
    SH_TOUCH_BUILD_SWIPE = 22,
    SH_TOUCH_HOLD = 23,
    SH_TOUCH_ZOOM_RESET = 24,
    SH_TOUCH_ROTATE = 25,
    SH_TOUCH_WHEEL = 26,
    SH_TOUCH_GRAB = 27,
    SH_TOUCH_UI_DRAG_BEGIN = 28,
    SH_TOUCH_UI_DRAG_UPDATE = 29,
    SH_TOUCH_UI_DRAG_END = 30
};

enum stronghold_touch_class {
    SH_TOUCH_PENDING = 0,
    SH_TOUCH_UI = 1,
    SH_TOUCH_LIST = 2,
    SH_TOUCH_WORLD = 3,
    SH_TOUCH_ABILITY = 4,
    SH_TOUCH_BUILDING = 5,
    SH_TOUCH_BLOCKED = 6,
    SH_TOUCH_BOOK = 7,
    SH_TOUCH_RADAR = 8,
    SH_TOUCH_BUILD_PANEL = 9,
    SH_TOUCH_DRAG = 10,
    SH_TOUCH_MENU = 11
};

/* x/y and pan dx/dy are normalized to the full Android surface. The scale
 * factor is incremental for TRANSFORM and absolute for UI_SCALE. HOLD is a
 * stationary long press: it cancels an armed command or a selection, and
 * otherwise toggles the original relief view. ROTATE carries the twist
 * direction in dx: +1 clockwise, -1 counter-clockwise.
 * A USB/Bluetooth mouse, in either MODE and outside touch sequences: WHEEL at
 * the cursor x/y carries original WHEEL_DELTA units in dx, 1 in dy when a new
 * wheel burst begins, and a zoom factor in scale; in a match the world under
 * the cursor zooms, menus, lists and the HUD keep the original wheel. GRAB
 * (middle-button drag) pans the world in a match by dx/dy. */
void stronghold_touch_enqueue(int action, unsigned sequence, float x, float y,
                             float dx, float dy, float scale);
int stronghold_touch_status(unsigned sequence);
/* 1 once MODE 0 is applied: the original desktop mouse path is active. */
int stronghold_touch_desktop(void);
void stronghold_touch_frame(void);
void stronghold_touch_after_world(void);
void stronghold_touch_abort(void);

#endif
