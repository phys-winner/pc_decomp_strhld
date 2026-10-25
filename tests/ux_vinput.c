/* Virtual USB mouse and keyboard for device tests (root, /dev/uinput).
 * Build: clang --target=aarch64-linux-android30 -O2 tests/ux_vinput.c -o ux-vinput
 *        -Wl,-z,max-page-size=16384; push to /data/local/tmp, chmod 755, run with su -c.
 *
 *   ux-vinput [command ...]        create both devices, run, remove them
 *   ux-vinput serve <fifo>         keep them; run lines written to <fifo>
 *                                  until "quit" (the FIFO is created if missing)
 *
 * Commands (separate arguments, or ';'-separated words on one line):
 *   move dx dy [steps ms]   relative motion, optionally split into steps
 *   home                    pointer to the top-left corner
 *   goto x y                home, then unaccelerated steps to display pixel x,y
 *   down|up left|right|middle|side|extra
 *   click button [ms]       press, hold ms (default 80), release
 *   drag button dx dy [steps ms]   press, move, release
 *   wheel n [ms]            n notches, positive away from the user, ms apart
 *   key code 1|0            Linux KEY_* code (KEY_A 30, KEY_ESC 1, KEY_KPPLUS 78)
 *   tap code [ms]           key down, hold ms (default 60), up
 *   sleep ms
 * Android applies pointer acceleration above 500 px/s; goto stays below it,
 * so its steps are 1:1 in display pixels of the current orientation. */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int mouse_fd = -1, keyboard_fd = -1;

static void pause_ms(long ms)
{
    if (ms <= 0) return;
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

static void emit(int fd, int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type; e.code = (unsigned short)code; e.value = value;
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("ux-vinput: write");
}
static void sync_device(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static int create(const char *name, int product, int mouse)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("ux-vinput: /dev/uinput"); exit(2); }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    if (mouse) {
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        const int buttons[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
        for (unsigned i = 0; i < sizeof buttons / sizeof *buttons; i++) ioctl(fd, UI_SET_KEYBIT, buttons[i]);
        const int axes[] = { REL_X, REL_Y, REL_WHEEL, REL_HWHEEL };
        for (unsigned i = 0; i < sizeof axes / sizeof *axes; i++) ioctl(fd, UI_SET_RELBIT, axes[i]);
    } else {
        for (int key = KEY_ESC; key <= KEY_MICMUTE; key++) ioctl(fd, UI_SET_KEYBIT, key);
    }
    struct uinput_user_dev device;
    memset(&device, 0, sizeof device);
    snprintf(device.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    device.id.bustype = BUS_USB; device.id.vendor = 0x1209; device.id.product = (unsigned short)product; device.id.version = 1;
    if (write(fd, &device, sizeof device) != (ssize_t)sizeof device || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("ux-vinput: create"); exit(2);
    }
    return fd;
}

static void move(int dx, int dy, int steps, long ms)
{
    if (steps < 1) steps = 1;
    int sentx = 0, senty = 0;
    for (int i = 1; i <= steps; i++) {
        int x = dx * i / steps, y = dy * i / steps;
        if (x != sentx) emit(mouse_fd, EV_REL, REL_X, x - sentx);
        if (y != senty) emit(mouse_fd, EV_REL, REL_Y, y - senty);
        sentx = x; senty = y;
        sync_device(mouse_fd);
        if (i < steps) pause_ms(ms / steps);
    }
}

static void home(void) { for (int i = 0; i < 8; i++) { move(-1000, -1000, 1, 0); pause_ms(8); } pause_ms(60); }

static void slow_to(int x, int y)
{
    /* 8 px per 20 ms = 400 px/s, under the 500 px/s acceleration threshold. */
    int dx = x, dy = y;
    while (dx || dy) {
        int sx = dx > 8 ? 8 : dx < -8 ? -8 : dx, sy = dy > 8 ? 8 : dy < -8 ? -8 : dy;
        if (sx) emit(mouse_fd, EV_REL, REL_X, sx);
        if (sy) emit(mouse_fd, EV_REL, REL_Y, sy);
        sync_device(mouse_fd);
        dx -= sx; dy -= sy;
        pause_ms(20);
    }
}

static int button(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "left")) return BTN_LEFT;
    if (!strcmp(name, "right")) return BTN_RIGHT;
    if (!strcmp(name, "middle")) return BTN_MIDDLE;
    if (!strcmp(name, "side")) return BTN_SIDE;
    if (!strcmp(name, "extra")) return BTN_EXTRA;
    return -1;
}
static void press(int code, int down) { emit(mouse_fd, EV_KEY, code, down); sync_device(mouse_fd); }
static void key(int code, int down) { emit(keyboard_fd, EV_KEY, code, down); sync_device(keyboard_fd); }
static int number(char **words, int at, int count, int fallback) { return at < count ? atoi(words[at]) : fallback; }

/* Runs one command starting at words[0]; returns how many words it used, 0 on error. */
static int command(char **w, int n)
{
    const char *c = w[0];
    if (!strcmp(c, "move") && n >= 3) {
        int steps = number(w, 3, n, 1); long ms = number(w, 4, n, 0);
        move(atoi(w[1]), atoi(w[2]), steps, ms); return n >= 5 ? 5 : n >= 4 ? 4 : 3;
    }
    if (!strcmp(c, "home")) { home(); return 1; }
    if (!strcmp(c, "goto") && n >= 3) { home(); slow_to(atoi(w[1]), atoi(w[2])); return 3; }
    if ((!strcmp(c, "down") || !strcmp(c, "up")) && n >= 2 && button(w[1]) >= 0) { press(button(w[1]), c[0] == 'd'); return 2; }
    if (!strcmp(c, "click") && n >= 2 && button(w[1]) >= 0) {
        int ms = n >= 3 && w[2][0] >= '0' && w[2][0] <= '9' ? atoi(w[2]) : 80;
        press(button(w[1]), 1); pause_ms(ms); press(button(w[1]), 0); return n >= 3 && w[2][0] >= '0' && w[2][0] <= '9' ? 3 : 2;
    }
    if (!strcmp(c, "drag") && n >= 4 && button(w[1]) >= 0) {
        int steps = number(w, 4, n, 10); long ms = number(w, 5, n, 300);
        press(button(w[1]), 1); pause_ms(60); move(atoi(w[2]), atoi(w[3]), steps, ms); pause_ms(60); press(button(w[1]), 0);
        return n >= 6 ? 6 : n >= 5 ? 5 : 4;
    }
    if (!strcmp(c, "wheel") && n >= 2) {
        int notches = atoi(w[1]), ms = number(w, 2, n, 120), sign = notches < 0 ? -1 : 1;
        for (int i = 0; i < abs(notches); i++) {
            emit(mouse_fd, EV_REL, REL_WHEEL, sign); sync_device(mouse_fd);
            if (i + 1 < abs(notches)) pause_ms(ms);
        }
        return n >= 3 ? 3 : 2;
    }
    if (!strcmp(c, "hwheel") && n >= 2) { emit(mouse_fd, EV_REL, REL_HWHEEL, atoi(w[1])); sync_device(mouse_fd); return 2; }
    if (!strcmp(c, "key") && n >= 3) { key(atoi(w[1]), atoi(w[2]) != 0); return 3; }
    if (!strcmp(c, "tap") && n >= 2) {
        int ms = n >= 3 && w[2][0] >= '0' && w[2][0] <= '9' ? atoi(w[2]) : 60;
        key(atoi(w[1]), 1); pause_ms(ms); key(atoi(w[1]), 0); return n >= 3 && w[2][0] >= '0' && w[2][0] <= '9' ? 3 : 2;
    }
    if (!strcmp(c, "sleep") && n >= 2) { pause_ms(atol(w[1])); return 2; }
    fprintf(stderr, "ux-vinput: bad command near '%s'\n", c);
    return 0;
}

static int run(char **words, int count)
{
    for (int at = 0; at < count;) {
        if (!strcmp(words[at], ";")) { at++; continue; }
        int used = command(words + at, count - at);
        if (!used) return 1;
        at += used;
    }
    return 0;
}

static void destroy(void)
{
    if (mouse_fd >= 0) { ioctl(mouse_fd, UI_DEV_DESTROY); close(mouse_fd); }
    if (keyboard_fd >= 0) { ioctl(keyboard_fd, UI_DEV_DESTROY); close(keyboard_fd); }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s command ... | serve <fifo>\n", argv[0]);
        return 2;
    }
    mouse_fd = create("ux-vinput mouse", 0x5578, 1);
    keyboard_fd = create("ux-vinput keyboard", 0x5579, 0);
    pause_ms(900); /* InputReader opens new devices asynchronously */
    int status = 0;
    if (!strcmp(argv[1], "serve") && argc >= 3) {
        if (mkfifo(argv[2], 0666) && errno != EEXIST) { perror("ux-vinput: mkfifo"); destroy(); return 2; }
        chmod(argv[2], 0666);
        printf("ux-vinput: serving %s\n", argv[2]); fflush(stdout);
        for (int quit = 0; !quit;) {
            FILE *input = fopen(argv[2], "r");
            if (!input) { perror("ux-vinput: fifo"); break; }
            char line[1024];
            while (!quit && fgets(line, sizeof line, input)) {
                char *words[128]; int count = 0;
                for (char *word = strtok(line, " \t\r\n;"); word && count < 128; word = strtok(NULL, " \t\r\n;")) words[count++] = word;
                if (count && !strcmp(words[0], "quit")) quit = 1;
                else if (count) { int bad = run(words, count); printf("ux-vinput: %s\n", bad ? "error" : "ok"); fflush(stdout); }
            }
            fclose(input);
        }
        unlink(argv[2]);
    } else {
        status = run(argv + 1, argc - 1);
        pause_ms(150);
    }
    destroy();
    return status;
}
