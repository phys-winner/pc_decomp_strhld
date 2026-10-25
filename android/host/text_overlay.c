/* Crisp GDI text (0.9). The raster path is untouched: guest memory keeps
 * exactly the pixels it always had. Each run blended by gdi_android.c also
 * leaves a note: where its ink landed (a UI group of the composed frame, or
 * the HUD/menu surface), the ink box's text-free pixels and coverage, and a
 * hash of each of its rows right after the run. Before a layer is presented,
 * every run whose rows there still hash to those values gets its text-free
 * pixels and coverage back and is drawn again from a glyph atlas rendered at
 * output size, right after its layer. Rows painted over later at the top or
 * bottom of a run (the next list row's background over descenders) are left
 * alone and the crisp run is cut there; anything else that cannot be proven
 * intact keeps its original raster text. */
#include "host.h"
#include "display.h"
#include "text_overlay.h"
#include <limits.h>
#include <math.h>
#include <GLES2/gl2.h>
#define MAX_NOTES 768
#define MAX_CLEAN (8u << 20)     /* bytes of text-free pixels kept */
#define HUD_ROWS 128             /* group 0 rows display.c copies from the HUD surface */
#define MARGIN 3                 /* original pixels around the raster box a crisp glyph may reach */
#define ATLAS 2048
#define MAX_GLYPHS 4096
#define GLYPH_BUCKETS 8192
#define MAX_SHELVES 512
#define MAX_QUADS 4096

typedef struct {
    uint32_t serial, epoch;      /* noting order; publish epoch it was noted in */
    int kind;                    /* UI group (redirected back buffer), SH_UI_LAYERS HUD/menu surface */
    uint32_t address, pitch;     /* first ink-box pixel in guest memory, surface bytes per row */
    int x, y, w, h;              /* ink box in layer or persistent HUD/menu surface pixels */
    int tx, ty, bw, bh;          /* the run's raster box: origin and size */
    int cx0, cy0, cx1, cy1;      /* where the crisp run is cut: the drawable edge where the clip cut
                                    raster glyphs, else a pixel beyond the visible raster box (glyph
                                    metrics differ by a fraction of a pixel between sizes) */
    int target;                  /* group its stores reached in a UI pass, -1 none */
    uint64_t *rows;              /* hash of each ink-box row after the run, kept current by later runs */
    uint16_t *clean;             /* w*h text-free pixels */
    uint8_t *coverage;           /* w*h text-free coverage of target, NULL when the run left it unchanged */
    uint16_t *text;
    int count, size, weight, italic;
    uint16_t color;              /* RGB565 of a fully covered pixel */
} note_t;

typedef struct { int x0, y0, x1, y1, u, v; uint32_t rgba; } quad_t;
typedef struct { quad_t *quads; int count, capacity; } queue_t;
typedef struct {
    uint64_t key;
    uint16_t *text;
    int count, size, weight, italic, scale, phase, w, h;
    int shelf, ax, ay, next, used;
    uint32_t stamp;
    uint8_t *pending;            /* coverage awaiting upload */
} glyph_t;
typedef struct { int y, h, x; uint32_t stamp; } shelf_t;

static note_t *notes[MAX_NOTES];
static int note_count;
static size_t clean_bytes;
static uint32_t note_serial, epoch;
static note_t *pending;
static note_t *refresh[MAX_NOTES];
static int refresh_count;

/* Queues and the atlas are shared with the GL thread; notes stay on the guest thread. */
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static queue_t queues[SH_UI_LAYERS + 1]; /* UI groups, then the uncomposed frame (group -1) */
static glyph_t glyphs[MAX_GLYPHS];
static int glyph_buckets[GLYPH_BUCKETS], glyph_free = -1, glyph_top, glyph_ready;
static shelf_t shelves[MAX_SHELVES];
static int shelf_count, shelf_bottom;
static uint32_t use_stamp;
static uint16_t *restore_pixels;
static size_t restore_capacity;
static struct { uint16_t *pixels; int stride, x, y, w, h; size_t at; } restore_list[MAX_NOTES];
static int restore_count;
/* Old presenter only (stronghold_text_overlay_publish): its composed frame is
 * one texture with every group's text drawn after it, so a lower group's run
 * under a higher group's bounding box (window pixels) keeps its raster copy. */
static int occluder[SH_UI_LAYERS], occluder_rect[SH_UI_LAYERS][4];

static GLuint program, atlas, buffer;
static GLint view_uniform;
static int gl_failed;
static uint8_t *vertices;
static size_t vertex_capacity;

typedef struct { unsigned noted, shown, partial, unlocated, coverage, changed, blocked, placed; } counts_t;
static counts_t stats, reported;   /* cumulative; the log shows what changed */
static uint64_t report_ns;

static uint64_t mix64(uint64_t h, uint64_t v)
{
    h ^= v; h *= 0x9E3779B97F4A7C15ull; return h ^ h >> 31;
}

static uint64_t hash_row(const uint16_t *p, int w)
{
    uint64_t hash = 0x6A09E667F3BCC909ull ^ (uint32_t)w;
    const uint8_t *b = (const uint8_t *)p;
    size_t n = (size_t)w * 2;
    for (; n >= 8; b += 8, n -= 8) { uint64_t v; memcpy(&v, b, 8); hash = mix64(hash, v); }
    if (n) { uint64_t v = 0; memcpy(&v, b, n); hash = mix64(hash, v ^ (uint64_t)n << 56); }
    return hash;
}

/* The rows of a run's ink box (at p, stride pixels per row) still holding
 * that run: all of them, or one band when only rows at its top and/or bottom
 * were painted over later and at least half remain. 0 when none qualify. */
static int intact_rows(const note_t *n, const uint16_t *p, size_t stride, int *r0, int *r1)
{
    int first = -1, last = -1;
    for (int r = 0; r < n->h; r++) {
        int same = hash_row(p + (size_t)r * stride, n->w) == n->rows[r];
        if (same && first < 0) first = r;
        if (same && last >= 0 && last != r - 1) return 0;   /* a second band: changed in the middle */
        if (same) last = r;
    }
    if (first < 0 || (last + 1 - first) * 2 < n->h) return 0;
    *r0 = first; *r1 = last + 1;
    return 1;
}

static int source_readable(const note_t *n)
{
    return void_vm_readable(n->address, (uint32_t)(n->h - 1) * n->pitch + (uint32_t)n->w * 2);
}

static const uint16_t *source(const note_t *n) { return (const uint16_t *)GPTR(n->address); }

static void source_rows(const note_t *n)
{
    for (int r = 0; r < n->h; r++) n->rows[r] = hash_row(source(n) + (size_t)r * (n->pitch / 2), n->w);
}

/* Exactly as noted (when another run is about to paint over it). */
static int source_intact(const note_t *n)
{
    int r0, r1;
    return source_readable(n) && intact_rows(n, source(n), n->pitch / 2, &r0, &r1) && !r0 && r1 == n->h;
}

/* Still usable from that surface (between frames). */
static int source_alive(const note_t *n)
{
    int r0, r1;
    return source_readable(n) && intact_rows(n, source(n), n->pitch / 2, &r0, &r1);
}

static void free_note(note_t *n)
{
    clean_bytes -= (size_t)n->w * n->h * 3;
    free(n->clean); free(n->coverage); free(n->rows); free(n->text); free(n);
}

static void drop_note(int index)
{
    free_note(notes[index]);
    memmove(notes + index, notes + index + 1, (size_t)(note_count - index - 1) * sizeof *notes);
    --note_count;
}

static void forget_notes(void)
{
    while (note_count) drop_note(note_count - 1);
}

static int overlaps(const note_t *n, int x0, int y0, int x1, int y1)
{
    return n->x < x1 && x0 < n->x + n->w && n->y < y1 && y0 < n->y + n->h;
}

static int notes_overlap(const note_t *a, const note_t *b)
{
    return overlaps(a, b->x, b->y, b->x + b->w, b->y + b->h);
}

int stronghold_text_overlay_begin(uint32_t dst, int w, int h, int pitch, int x, int y,
    const uint8_t *mask, int mw, int mh, int stride, const uint16_t *text, int count,
    int size, int weight, int italic, uint32_t color, uint32_t background, int opaque)
{
    if (pending) { free_note(pending); pending = NULL; }
    if (count <= 0 || !text || !mask || mw <= 0 || mh <= 0 || stride < mw || (pitch & 1) || w <= 0 || h <= 0) return 0;
    if (!stronghold_display_crisp_text() || gl_failed) { if (note_count) forget_notes(); return 0; }
    /* The visible part of the raster box. */
    int vx0 = x > 0 ? x : 0, vy0 = y > 0 ? y : 0;
    int vx1 = x + mw < w ? x + mw : w, vy1 = y + mh < h ? y + mh : h;
    if (vx0 >= vx1 || vy0 >= vy1) return 0;
    /* The run's ink in drawable coordinates (mask column = drawable column - x),
     * then the part the clip left visible. */
    int ix0 = INT_MAX, iy0 = INT_MAX, ix1 = INT_MIN, iy1 = INT_MIN;
    for (int row = 0; row < mh; row++) {
        const uint8_t *a = mask + (size_t)row * stride;
        int first = 0, last = mw - 1;
        while (first <= last && !a[first]) first++;
        if (first > last) continue;
        while (!a[last]) last--;
        if (first + x < ix0) ix0 = first + x;
        if (last + 1 + x > ix1) ix1 = last + 1 + x;
        if (row + y < iy0) iy0 = row + y;
        iy1 = row + 1 + y;
    }
    if (ix0 >= ix1) return 0;    /* blank: nothing to sharpen */
    /* How far the clip cut into the ink: one pixel is only an antialiased edge. */
    int cut_left = vx0 - ix0, cut_top = vy0 - iy0, cut_right = ix1 - vx1, cut_bottom = iy1 - vy1;
    if (ix0 < vx0) ix0 = vx0;
    if (iy0 < vy0) iy0 = vy0;
    if (ix1 > vx1) ix1 = vx1;
    if (iy1 > vy1) iy1 = vy1;
    if (ix0 >= ix1 || iy0 >= iy1) return 0;
    stats.noted++;
    uint32_t first = dst + (uint32_t)iy0 * (uint32_t)pitch + (uint32_t)ix0 * 2;
    uint32_t last = dst + (uint32_t)(iy1 - 1) * (uint32_t)pitch + (uint32_t)(ix1 - 1) * 2;
    int lx, ly, ex, ey;
    int kind = stronghold_display_locate(first, &lx, &ly);
    /* Both corners on the same grid prove the surface shares that grid's pitch. */
    if (kind < 0 || kind > SH_UI_LAYERS || stronghold_display_locate(last, &ex, &ey) != kind ||
        ex - lx != ix1 - 1 - ix0 || ey - ly != iy1 - 1 - iy0) { stats.unlocated++; return 0; }
    int nw = ix1 - ix0, nh = iy1 - iy0;
    size_t pixels = (size_t)nw * nh;
    note_t *n = calloc(1, sizeof *n);
    if (n) { n->clean = malloc(pixels * 2); n->rows = malloc((size_t)nh * sizeof *n->rows); n->text = malloc((size_t)count * 2); }
    if (!n || !n->clean || !n->rows || !n->text) { if (n) { free(n->clean); free(n->rows); free(n->text); free(n); } return 0; }
    int ox = lx - ix0, oy = ly - iy0;      /* drawable-area to layer/surface coordinates */
    n->kind = kind; n->address = first; n->pitch = (uint32_t)pitch;
    n->x = lx; n->y = ly; n->w = nw; n->h = nh;
    n->tx = x + ox; n->ty = y + oy; n->bw = mw; n->bh = mh;
    n->cx0 = (cut_left > 1 ? vx0 : vx0 - MARGIN) + ox; n->cy0 = (cut_top > 1 ? vy0 : vy0 - MARGIN) + oy;
    n->cx1 = (cut_right > 1 ? vx1 : vx1 + MARGIN) + ox; n->cy1 = (cut_bottom > 1 ? vy1 : vy1 + MARGIN) + oy;
    memcpy(n->text, text, (size_t)count * 2);
    n->count = count; n->size = size; n->weight = weight; n->italic = italic;
    n->color = (uint16_t)((color & 0xF8u) << 8 | (color >> 8 & 0xFCu) << 3 | (color >> 19 & 0x1Fu));
    clean_bytes += pixels * 3;
    /* In a UI pass the stores also raise a group's coverage (directly, or as
     * the HUD surface's mirror): keep what it was under the ink box. An opaque
     * run writes its whole box with or without glyphs, so it changes nothing there. */
    int group = -1, width = 0;
    stronghold_display_capture_row(&group, ly, &width);
    n->target = group >= 0 && (kind == SH_UI_LAYERS || kind == group) ? group : -1;
    if (n->target >= 0 && !opaque) {
        uint8_t *coverage = malloc(pixels);
        int changes = 0;
        if (!coverage) n->target = -1;   /* not hidden in that group */
        for (int row = 0; coverage && row < nh; row++) {
            int g2 = -1;
            const uint8_t *c = stronghold_display_capture_row(&g2, ly + row, &width);
            if (!c || g2 != group || lx < 0 || lx + nw > width) { free(coverage); coverage = NULL; n->target = -1; break; }
            memcpy(coverage + (size_t)row * nw, c + lx, (size_t)nw);
            for (int col = 0; col < nw && !changes; col++) changes = c[lx + col] != 255;
        }
        if (coverage && !changes) { free(coverage); coverage = NULL; }
        n->coverage = coverage;
    }
    /* Older runs under this one: a changed one is gone; one this run paints
     * over opaquely loses glyphs; the rest get their row hashes refreshed in end. */
    int rx0 = opaque ? vx0 + ox : n->x, ry0 = opaque ? vy0 + oy : n->y;
    int rx1 = opaque ? vx1 + ox : n->x + n->w, ry1 = opaque ? vy1 + oy : n->y + n->h;
    refresh_count = 0;
    for (int i = note_count - 1; i >= 0; i--) {
        note_t *o = notes[i];
        if (o->kind != kind || !overlaps(o, rx0, ry0, rx1, ry1)) continue;
        if (opaque || !source_intact(o)) drop_note(i);
    }
    for (int i = 0; i < note_count; i++)
        if (notes[i]->kind == kind && overlaps(notes[i], rx0, ry0, rx1, ry1)) refresh[refresh_count++] = notes[i];
    if (opaque) {
        uint16_t fill = (uint16_t)((background & 0xF8u) << 8 | (background >> 8 & 0xFCu) << 3 | (background >> 19 & 0x1Fu));
        for (size_t i = 0; i < pixels; i++) n->clean[i] = fill;
    } else for (int row = 0; row < nh; row++)
        memcpy(n->clean + (size_t)row * nw, GPTR(first + (uint32_t)row * (uint32_t)pitch), (size_t)nw * 2);
    pending = n;
    return 1;
}

void stronghold_text_overlay_end(void)
{
    note_t *n = pending;
    pending = NULL;
    if (!n) return;
    source_rows(n);
    for (int i = 0; i < refresh_count; i++) source_rows(refresh[i]);
    refresh_count = 0;
    while (note_count && (note_count >= MAX_NOTES || clean_bytes > MAX_CLEAN)) drop_note(0);
    n->serial = ++note_serial; n->epoch = epoch;
    notes[note_count++] = n;
}

/* ------------------------------------------------------------- glyph atlas */
static uint64_t glyph_key(const note_t *n, int scale, int phase)
{
    uint64_t h = mix64(0x243F6A8885A308D3ull, (uint64_t)(uint32_t)n->size << 32 | (uint32_t)n->weight);
    h = mix64(h, (uint64_t)(uint32_t)scale << 32 | (uint32_t)(phase << 1 | (n->italic != 0)));
    return h ^ hash_row(n->text, n->count);
}

static void glyph_unlink(int index)
{
    glyph_t *g = &glyphs[index];
    int *link = &glyph_buckets[g->key & (GLYPH_BUCKETS - 1)];
    while (*link >= 0 && *link != index) link = &glyphs[*link].next;
    if (*link == index) *link = g->next;
    free(g->text); free(g->pending);
    memset(g, 0, sizeof *g);
    g->next = glyph_free; glyph_free = index;
}

static void atlas_clear(void)
{
    if (glyph_ready) for (int i = 0; i < glyph_top; i++) if (glyphs[i].used) glyph_unlink(i);
    for (int i = 0; i < GLYPH_BUCKETS; i++) glyph_buckets[i] = -1;
    glyph_free = -1; glyph_top = 0; shelf_count = shelf_bottom = 0; glyph_ready = 1;
}

/* Shelves of similar height, filled left to right; a shelf whose glyphs were
 * not used by the queues being built is the unit of eviction. */
static int atlas_place(int w, int h, int *x, int *y, int *shelf)
{
    if (w > ATLAS || h > ATLAS / 4) return 0;
    int height = (h + 3) & ~3, best = -1;
    for (int i = 0; i < shelf_count; i++) {
        shelf_t *s = &shelves[i];
        if (s->h >= height && s->h <= height + height / 2 + 4 && s->x + w <= ATLAS && (best < 0 || s->h < shelves[best].h)) best = i;
    }
    if (best < 0 && shelf_count < MAX_SHELVES && shelf_bottom + height <= ATLAS) {
        best = shelf_count++;
        shelves[best] = (shelf_t){ shelf_bottom, height, 0, 0 };
        shelf_bottom += height + 1;
    }
    if (best < 0) {
        for (int i = 0; i < shelf_count; i++)
            if (shelves[i].h >= height && shelves[i].stamp != use_stamp && (best < 0 || shelves[i].stamp < shelves[best].stamp)) best = i;
        if (best < 0) return 0;
        for (int i = 0; i < glyph_top; i++) if (glyphs[i].used && glyphs[i].shelf == best) glyph_unlink(i);
        shelves[best].x = 0;
    }
    *x = shelves[best].x; *y = shelves[best].y; *shelf = best;
    shelves[best].x += w + 1;
    shelves[best].stamp = use_stamp;
    return 1;
}

static glyph_t *glyph_get(const note_t *n, float s, int scale, int phase, float dx, float dy, int w, int h)
{
    if (!glyph_ready) atlas_clear();
    uint64_t key = glyph_key(n, scale, phase);
    for (int i = glyph_buckets[key & (GLYPH_BUCKETS - 1)]; i >= 0; i = glyphs[i].next) {
        glyph_t *g = &glyphs[i];
        if (g->key == key && g->count == n->count && g->size == n->size && g->weight == n->weight &&
            g->italic == n->italic && g->scale == scale && g->phase == phase && g->w == w && g->h == h &&
            !memcmp(g->text, n->text, (size_t)n->count * 2)) {
            g->stamp = shelves[g->shelf].stamp = use_stamp;
            return g;
        }
    }
    uint8_t *coverage = calloc((size_t)w * h, 1);
    uint16_t *text = malloc((size_t)n->count * 2);
    int ax, ay, shelf;
    if (!coverage || !text || !void_android_text_raster(n->text, n->count, n->size, n->weight, n->italic, s, dx, dy, w, h, coverage) ||
        !atlas_place(w, h, &ax, &ay, &shelf)) { free(coverage); free(text); return NULL; }
    int index = glyph_free;
    if (index >= 0) glyph_free = glyphs[index].next;
    else if (glyph_top < MAX_GLYPHS) index = glyph_top++;
    else {
        /* Table full: the least recently used glyph not needed now. */
        for (int i = 0; i < glyph_top; i++)
            if (glyphs[i].stamp != use_stamp && (index < 0 || glyphs[i].stamp < glyphs[index].stamp)) index = i;
        if (index < 0) { free(coverage); free(text); return NULL; }
        glyph_unlink(index);
        glyph_free = glyphs[index].next;
    }
    glyph_t *g = &glyphs[index];
    memcpy(text, n->text, (size_t)n->count * 2);
    *g = (glyph_t){ key, text, n->count, n->size, n->weight, n->italic, scale, phase, w, h, shelf, ax, ay,
                    glyph_buckets[key & (GLYPH_BUCKETS - 1)], 1, use_stamp, coverage };
    glyph_buckets[key & (GLYPH_BUCKETS - 1)] = index;
    return g;
}

/* Where the run lands in the window: one transform for its whole raster box
 * (a box straddling group 0's top bar and bottom HUD is left alone), a bitmap
 * at a whole pixel with a quarter-pixel origin covering the box plus MARGIN,
 * cut like the raster and to rows r0..r1 of the ink box. */
static int place(const note_t *n, int group, int r0, int r1, quad_t *q)
{
    float vx, vy, s, ex, ey, es;
    float x1 = (float)(n->x + n->w), y1 = (float)(n->y + n->h);
    if (!stronghold_display_layer_view(group, (float)n->tx, (float)n->ty, &vx, &vy, &s) || !(s > .05f && s < 16.f) ||
        !stronghold_display_layer_view(group, x1, y1, &ex, &ey, &es) || fabsf(es - s) > s * 1e-4f ||
        fabsf(ex - (vx + (x1 - n->tx) * s)) > .05f || fabsf(ey - (vy + (y1 - n->ty) * s)) > .05f) return 0;
    float bx = vx - MARGIN * s, by = vy - MARGIN * s;
    int ix = (int)floorf(bx), iy = (int)floorf(by);
    int px = (int)lrintf((bx - ix) * 4), py = (int)lrintf((by - iy) * 4);
    if (px == 4) { ix++; px = 0; }
    if (py == 4) { iy++; py = 0; }
    int w = (int)ceilf(px * .25f + (n->bw + 2 * MARGIN) * s), h = (int)ceilf(py * .25f + (n->bh + 2 * MARGIN) * s);
    if (w < 1 || h < 1 || w > ATLAS || h > ATLAS / 4) return 0;
    int ly0 = n->cy0, ly1 = n->cy1;
    if (r0 > 0 && n->y + r0 > ly0) ly0 = n->y + r0;
    if (r1 < n->h && n->y + r1 < ly1) ly1 = n->y + r1;
    int x0 = (int)lrintf(vx + (n->cx0 - n->tx) * s), y0 = (int)lrintf(vy + (ly0 - n->ty) * s);
    int qx1 = (int)lrintf(vx + (n->cx1 - n->tx) * s), qy1 = (int)lrintf(vy + (ly1 - n->ty) * s);
    if (x0 < ix) x0 = ix;
    if (y0 < iy) y0 = iy;
    if (qx1 > ix + w) qx1 = ix + w;
    if (qy1 > iy + h) qy1 = iy + h;
    if (x0 >= qx1 || y0 >= qy1) return 0;
    glyph_t *g = glyph_get(n, s, (int)lrintf(s * 1024), px | py << 2, px * .25f + MARGIN * s, py * .25f + MARGIN * s, w, h);
    if (!g) return 0;
    unsigned r = n->color >> 11, gg = n->color >> 5 & 63u, b = n->color & 31u;
    *q = (quad_t){ x0, y0, qx1, qy1, g->ax + x0 - ix, g->ay + y0 - iy,
                   (r * 255u + 15) / 31u | ((gg * 255u + 31) / 63u) << 8 | ((b * 255u + 15) / 31u) << 16 | 0xFF000000u };
    return 1;
}

static void queue_reset(int index) { queues[index].count = 0; }

static void queue_add(int index, const quad_t *q)
{
    queue_t *queue = &queues[index];
    if (queue->count == queue->capacity) {
        int capacity = queue->capacity ? queue->capacity * 2 : 64;
        quad_t *grown = capacity <= MAX_QUADS ? realloc(queue->quads, (size_t)capacity * sizeof *grown) : NULL;
        if (!grown) return;
        queue->quads = grown; queue->capacity = capacity;
    }
    queue->quads[queue->count++] = *q;
}

/* Verbose runs only: what was hidden and why the rest kept its raster copy. */
static void report(void)
{
    uint64_t now = void_monotonic_ns();
    if (!void_verbose || now - report_ns < 10000000000ull) return;
    if (stats.noted != reported.noted || stats.shown != reported.shown)
        void_log("[text] crisp: noted %u, shown %u (%u cut to rows); raster kept: unlocated %u, coverage %u, changed %u, under raster %u, unplaced %u; %d notes",
                 stats.noted - reported.noted, stats.shown - reported.shown, stats.partial - reported.partial,
                 stats.unlocated - reported.unlocated, stats.coverage - reported.coverage, stats.changed - reported.changed,
                 stats.blocked - reported.blocked, stats.placed - reported.placed, note_count);
    reported = stats;
    report_ns = now;
}

/* candidates[] are note indices in noting order; mode[] 0 not eligible here,
 * 1 hidden with its coverage, 2 hidden without coverage (unchanged or forced
 * opaque by the HUD copy). A run is shown when its rows there are intact and
 * every later run overlapping it is shown too: their text-free pixels contain
 * the older raster text, its text-free pixels erase theirs. Patching newest
 * first leaves the oldest (text-free) pixels in the overlaps. */
static int hide_candidates(int queue, int group, int *candidates, int count, const unsigned char *mode,
                           uint16_t *pixels, uint8_t *coverage, int stride, int left, int top, int right, int bottom,
                           int keep_restore, int *hidden_rect)
{
    int hidden = 0;
    unsigned char *shown = calloc((size_t)count + 1, 1);
    quad_t *quads = malloc(((size_t)count + 1) * sizeof *quads);
    int *band = malloc(((size_t)count + 1) * 2 * sizeof *band);
    if (!shown || !quads || !band) { free(shown); free(quads); free(band); return 0; }
    for (int c = count - 1; c >= 0; c--) {
        note_t *n = notes[candidates[c]];
        int r0 = 0, r1 = 0;
        if (!mode[c]) { stats.coverage++; continue; }
        if (n->x < left || n->y < top || n->x + n->w > right || n->y + n->h > bottom ||
            !intact_rows(n, pixels + (size_t)n->y * stride + n->x, (size_t)stride, &r0, &r1)) { stats.changed++; continue; }
        int blocked = 0;
        for (int d = c + 1; d < count && !blocked; d++) blocked = !shown[d] && notes_overlap(n, notes[candidates[d]]);
        if (blocked) { stats.blocked++; continue; }
        if (!place(n, group, r0, r1, &quads[c])) { stats.placed++; continue; }
        int higher = 0;
        for (int p = 0; p < SH_UI_LAYERS && group >= 0 && !blocked; p++) {
            int h = SH_UI_ORDER[p];
            if (h == group) { higher = 1; continue; }
            if (higher) blocked = occluder[h] && quads[c].x0 < occluder_rect[h][2] && occluder_rect[h][0] < quads[c].x1 &&
                      quads[c].y0 < occluder_rect[h][3] && occluder_rect[h][1] < quads[c].y1;
        }
        if (blocked) { stats.blocked++; continue; }
        shown[c] = 1; band[2 * c] = r0; band[2 * c + 1] = r1;
        if (r0 || r1 < n->h) stats.partial++;
    }
    if (keep_restore) {
        /* Memory that must keep its raster text gets it back after the upload. */
        size_t need = 0;
        for (int c = 0; c < count; c++) if (shown[c]) need += (size_t)notes[candidates[c]]->w * (band[2 * c + 1] - band[2 * c]);
        if (need > restore_capacity) {
            uint16_t *grown = realloc(restore_pixels, need * 2 * sizeof *grown);
            if (grown) { restore_pixels = grown; restore_capacity = need * 2; }
            else memset(shown, 0, (size_t)count);
        }
    }
    size_t at = 0;
    for (int c = count - 1; c >= 0; c--) if (shown[c]) {
        note_t *n = notes[candidates[c]];
        int r0 = band[2 * c], r1 = band[2 * c + 1];
        if (keep_restore) {
            restore_list[restore_count++] = (typeof(restore_list[0])){ pixels, stride, n->x, n->y + r0, n->w, r1 - r0, at };
            for (int row = r0; row < r1; row++)
                memcpy(restore_pixels + at + (size_t)(row - r0) * n->w, pixels + (size_t)(n->y + row) * stride + n->x, (size_t)n->w * 2);
            at += (size_t)n->w * (r1 - r0);
        }
        for (int row = r0; row < r1; row++) {
            memcpy(pixels + (size_t)(n->y + row) * stride + n->x, n->clean + (size_t)row * n->w, (size_t)n->w * 2);
            if (mode[c] == 1 && n->coverage)
                memcpy(coverage + (size_t)(n->y + row) * stride + n->x, n->coverage + (size_t)row * n->w, (size_t)n->w);
        }
        if (hidden_rect) {
            int *h = hidden_rect, empty = h[0] >= h[2] || h[1] >= h[3];
            if (empty || n->x < h[0]) h[0] = n->x;
            if (empty || n->y + r0 < h[1]) h[1] = n->y + r0;
            if (empty || n->x + n->w > h[2]) h[2] = n->x + n->w;
            if (empty || n->y + r1 > h[3]) h[3] = n->y + r1;
        }
        hidden++;
    }
    for (int c = 0; c < count; c++) if (shown[c]) queue_add(queue, &quads[c]);
    stats.shown += (unsigned)hidden;
    free(shown); free(quads); free(band);
    return hidden;
}

int stronghold_text_overlay_hide_layers(uint16_t *const layers[SH_UI_LAYERS], uint8_t *const coverage[SH_UI_LAYERS], const int bounds[SH_UI_LAYERS][4],
                                        int stride, int width, int height, int hidden_rects[SH_UI_LAYERS][4])
{
    int hidden = 0;
    if (hidden_rects) memset(hidden_rects, 0, sizeof(int[SH_UI_LAYERS][4]));
    if (pending) return 0;
    /* Without a working GL pass nothing may be hidden (gl_failed is set at the first draw). */
    if (!stronghold_display_crisp_text() || gl_failed) {
        forget_notes();
        pthread_mutex_lock(&queue_lock);
        for (int i = 0; i <= SH_UI_LAYERS; i++) queue_reset(i);
        pthread_mutex_unlock(&queue_lock);
        return 0;
    }
    uint32_t published = epoch++;
    int *candidates = malloc(((size_t)note_count + 1) * sizeof *candidates);
    unsigned char *mode = malloc((size_t)note_count + 1);
    pthread_mutex_lock(&queue_lock);
    ++use_stamp;
    for (int i = 0; i <= SH_UI_LAYERS; i++) queue_reset(i);
    for (int g = 0; g < SH_UI_LAYERS && candidates && mode; g++) {
        if (!layers[g]) continue;
        int count = 0;
        for (int i = 0; i < note_count; i++) {
            const note_t *n = notes[i];
            /* The HUD surface reaches group 0 through display.c's copy of its
             * bottom rows (coverage 255 there), and that copy covers whatever
             * the pass itself drew in those rows. */
            int hud_rows = g == 0 && n->y >= height - HUD_ROWS && n->y + n->h <= height;
            int hud = n->kind == SH_UI_LAYERS && hud_rows;
            int mirror = n->kind == SH_UI_LAYERS && n->target == g && n->epoch == published;
            if ((n->kind != g || hud_rows) && !hud && !mirror) continue;
            candidates[count] = i;
            mode[count++] = hud ? 2 : n->target != g ? 0 : !n->coverage ? 2 : coverage && coverage[g] ? 1 : 0;
        }
        int left = bounds[g][0] > 0 ? bounds[g][0] : 0, top = bounds[g][1] > 0 ? bounds[g][1] : 0;
        int right = bounds[g][2] < width ? bounds[g][2] : width, bottom = bounds[g][3] < height ? bounds[g][3] : height;
        hidden += hide_candidates(g, g, candidates, count, mode, layers[g], coverage ? coverage[g] : NULL,
                                  stride, left, top, right, bottom, 0, hidden_rects ? hidden_rects[g] : NULL);
    }
    pthread_mutex_unlock(&queue_lock);
    free(candidates); free(mode);
    /* The group buffers are cleared for the next frame: their notes end here.
     * HUD surface notes stay while that surface still holds them. */
    for (int i = note_count - 1; i >= 0; i--)
        if (notes[i]->kind != SH_UI_LAYERS || !source_alive(notes[i])) drop_note(i);
    report();
    return hidden;
}

int stronghold_text_overlay_hide_frame(uint16_t *pixels, int stride, int width, int height)
{
    restore_count = 0;
    int enabled = stronghold_display_crisp_text() && !gl_failed;
    if (pending || !pixels || !enabled) {
        if (!enabled && note_count) forget_notes();
        pthread_mutex_lock(&queue_lock);
        for (int i = 0; i <= SH_UI_LAYERS; i++) queue_reset(i);
        pthread_mutex_unlock(&queue_lock);
        return 0;
    }
    /* The game presents its HUD/menu surface from 0,0; nothing else can be in the frame. */
    for (int i = note_count - 1; i >= 0; i--) if (notes[i]->kind != SH_UI_LAYERS) drop_note(i);
    int *candidates = malloc(((size_t)note_count + 1) * sizeof *candidates);
    unsigned char *mode = malloc((size_t)note_count + 1);
    int hidden = 0;
    pthread_mutex_lock(&queue_lock);
    ++use_stamp;
    for (int i = 0; i <= SH_UI_LAYERS; i++) queue_reset(i);
    if (candidates && mode) {
        for (int i = 0; i < note_count; i++) { candidates[i] = i; mode[i] = 2; }
        hidden = hide_candidates(SH_UI_LAYERS, -1, candidates, note_count, mode, pixels, NULL, stride, 0, 0, width, height, 1, NULL);
    }
    pthread_mutex_unlock(&queue_lock);
    free(candidates); free(mode);
    /* Notes whose surface no longer holds them are gone for good. */
    for (int i = note_count - 1; i >= 0; i--) if (!source_alive(notes[i])) drop_note(i);
    report();
    return hidden;
}

void stronghold_text_overlay_restore_frame(void)
{
    for (int i = restore_count - 1; i >= 0; i--) {
        typeof(restore_list[0]) *r = &restore_list[i];
        for (int row = 0; row < r->h; row++)
            memcpy(r->pixels + (size_t)(r->y + row) * r->stride + r->x, restore_pixels + r->at + (size_t)row * r->w, (size_t)r->w * 2);
    }
    restore_count = 0;
}

void stronghold_text_overlay_publish(void)
{
    uint16_t *layers[SH_UI_LAYERS];
    uint8_t *coverage[SH_UI_LAYERS];
    int bounds[SH_UI_LAYERS][4], width, height;
    if (!stronghold_display_text_layers(layers, coverage, bounds, &width, &height)) return;
    for (int g = 0; g < SH_UI_LAYERS; g++) {
        float x0, y0, x1, y1, s;
        occluder[g] = layers[g] && bounds[g][0] < bounds[g][2] && bounds[g][1] < bounds[g][3] &&
            stronghold_display_layer_view(g, (float)bounds[g][0], (float)bounds[g][1], &x0, &y0, &s) &&
            stronghold_display_layer_view(g, (float)bounds[g][2], (float)bounds[g][3], &x1, &y1, &s);
        if (occluder[g]) {
            occluder_rect[g][0] = (int)floorf(x0); occluder_rect[g][1] = (int)floorf(y0);
            occluder_rect[g][2] = (int)ceilf(x1); occluder_rect[g][3] = (int)ceilf(y1);
        }
    }
    stronghold_text_overlay_hide_layers(layers, coverage, bounds, width, width, height, NULL);
    memset(occluder, 0, sizeof occluder);
}

/* ------------------------------------------------------------------ drawing */
static GLuint compile(GLenum kind, const char *source)
{
    GLuint s = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(s, 1, &source, NULL); glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log);
        void_log("[text] shader: %s", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

static int gl_ready(void)
{
    if (program && atlas && buffer) return 1;
    if (gl_failed) return 0;
    static const char *vs =
        "attribute vec4 p;attribute vec4 c;uniform vec4 view;varying vec2 t;varying vec4 k;"
        "void main(){gl_Position=vec4(p.x*view.x-1.0,1.0-p.y*view.y,0.0,1.0);t=p.zw*view.zw;k=c;}";
    static const char *fs =
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
        "uniform sampler2D atlas;varying vec2 t;varying vec4 k;"
        "void main(){gl_FragColor=vec4(k.rgb,k.a*texture2D(atlas,t).a);}";
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = v ? compile(GL_FRAGMENT_SHADER, fs) : 0;
    GLint ok = 0;
    if (v && f) {
        program = glCreateProgram();
        glAttachShader(program, v); glAttachShader(program, f);
        glBindAttribLocation(program, 0, "p"); glBindAttribLocation(program, 1, "c");
        glLinkProgram(program);
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
    }
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    GLint largest = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &largest);
    if (!ok || largest < ATLAS) {
        void_log("[text] crisp text unavailable (program %d, max texture %d)", ok, largest);
        if (program) glDeleteProgram(program);
        program = 0; gl_failed = 1; return 0;
    }
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "atlas"), 0);
    view_uniform = glGetUniformLocation(program, "view");
    glGenTextures(1, &atlas);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS, ATLAS, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
    glGenBuffers(1, &buffer);
    /* Glyphs rendered before this texture existed are uploaded below. */
    return 1;
}

void stronghold_text_overlay_draw(int group, int viewport_width, int viewport_height)
{
    if (group < -1 || group >= SH_UI_LAYERS || viewport_width <= 0 || viewport_height <= 0) return;
    pthread_mutex_lock(&queue_lock);
    queue_t *queue = &queues[group < 0 ? SH_UI_LAYERS : group];
    if (!queue->count || !gl_ready()) { pthread_mutex_unlock(&queue_lock); return; }
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < glyph_top; i++) if (glyphs[i].used && glyphs[i].pending) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, glyphs[i].ax, glyphs[i].ay, glyphs[i].w, glyphs[i].h, GL_ALPHA, GL_UNSIGNED_BYTE, glyphs[i].pending);
        free(glyphs[i].pending); glyphs[i].pending = NULL;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    size_t need = (size_t)queue->count * 6 * 20;
    if (need > vertex_capacity) {
        uint8_t *grown = realloc(vertices, need);
        if (!grown) { pthread_mutex_unlock(&queue_lock); return; }
        vertices = grown; vertex_capacity = need;
    }
    uint8_t *v = vertices;
    for (int i = 0; i < queue->count; i++) {
        const quad_t *q = &queue->quads[i];
        float x0 = (float)q->x0, y0 = (float)q->y0, x1 = (float)q->x1, y1 = (float)q->y1;
        float u0 = (float)q->u, v0 = (float)q->v, u1 = u0 + (x1 - x0), v1 = v0 + (y1 - y0);
        const float corner[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
                                    {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
        for (int c = 0; c < 6; c++, v += 20) { memcpy(v, corner[c], 16); memcpy(v + 16, &q->rgba, 4); }
    }
    glUniform4f(view_uniform, 2.f / viewport_width, 2.f / viewport_height, 1.f / ATLAS, 1.f / ATLAS);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)need, vertices, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 20, (const void *)0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 20, (const void *)16);
    /* Destination alpha stays as the presenter left it (an opaque window). */
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLES, 0, queue->count * 6);
    glDisable(GL_BLEND);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    pthread_mutex_unlock(&queue_lock);
}

void stronghold_text_overlay_reset_gl(void)
{
    pthread_mutex_lock(&queue_lock);
    program = atlas = buffer = 0; gl_failed = 0;
    atlas_clear();
    for (int i = 0; i <= SH_UI_LAYERS; i++) queue_reset(i);
    pthread_mutex_unlock(&queue_lock);
}
