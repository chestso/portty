/*
 * bloom-vt — self-contained virtual terminal engine.
 *
 * Single public header. Embed by including bloom_vt.h and linking
 * libbloom_vt.a. No other internal headers are needed by consumers.
 *
 * Thread-safety: a BvtTerm is not internally synchronized; callers
 * own all synchronization. Allocation: bounded; the input-write hot
 * path is zero-allocation in steady state. See plan for details.
 */

#ifndef BLOOM_VT_H
#define BLOOM_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BvtTerm BvtTerm;

/* ------------------------------------------------------------------ */
/* Cell representation                                                 */
/* ------------------------------------------------------------------ */

/*
 * One grapheme cluster per cell. The visual width of the cluster is
 * stored on the cell so the renderer iterates the grid as a sequence
 * of (col, cell) pairs with no peek-ahead logic.
 *
 * Single-codepoint clusters store the codepoint in `cp` directly with
 * grapheme_id == 0. Multi-codepoint clusters set grapheme_id to a
 * non-zero offset into the page's grapheme arena; the full sequence
 * is retrieved with bvt_cell_get_grapheme().
 *
 * Continuation cells (the second cell of a width-2 cluster) have
 * width == 0 and are skipped by callers.
 */
typedef struct
{
    uint32_t cp;
    uint32_t grapheme_id;
    uint32_t style_id;
    uint8_t width;
    uint8_t flags;
    uint16_t hyperlink_id;
} BvtCell;

/* Cell flags */
enum
{
    BVT_CELL_WRAPLINE = 1u << 0, /* set on the last cell of a soft-wrapped logical row */
};

/* ------------------------------------------------------------------ */
/* Style                                                               */
/* ------------------------------------------------------------------ */

/* Style attribute bits (matching the term.h order to ease term_bvt.c) */
enum
{
    BVT_ATTR_BOLD = 1u << 0,
    BVT_ATTR_ITALIC = 1u << 1,
    BVT_ATTR_BLINK = 1u << 2,
    BVT_ATTR_REVERSE = 1u << 3,
    BVT_ATTR_STRIKETHROUGH = 1u << 4,
    BVT_ATTR_DWL = 1u << 5,        /* double-width line */
    BVT_ATTR_DHL_TOP = 1u << 6,    /* double-height line, top half */
    BVT_ATTR_DHL_BOTTOM = 1u << 7, /* double-height line, bottom half */
};

/* Underline styles: SGR 4:N */
typedef enum
{
    BVT_UL_NONE = 0,
    BVT_UL_SINGLE = 1,
    BVT_UL_DOUBLE = 2,
    BVT_UL_CURLY = 3,
    BVT_UL_DOTTED = 4,
    BVT_UL_DASHED = 5,
} BvtUnderline;

/* Color flags */
enum
{
    BVT_COLOR_DEFAULT_FG = 1u << 0,
    BVT_COLOR_DEFAULT_BG = 1u << 1,
    BVT_COLOR_DEFAULT_UL = 1u << 2,
    BVT_COLOR_INDEXED_FG = 1u << 3, /* fg_rgb low byte is a 0-255 palette index, not RGB */
    BVT_COLOR_INDEXED_BG = 1u << 4,
};

typedef struct
{
    uint32_t fg_rgb; /* 0x00RRGGBB if RGB; low byte = index if indexed */
    uint32_t bg_rgb;
    uint32_t ul_rgb;
    uint16_t attrs;       /* BVT_ATTR_* bitmask */
    uint8_t underline;    /* BvtUnderline */
    uint8_t font;         /* font select (0 = primary, 1-9 alt fonts via SGR 10-19) */
    uint16_t color_flags; /* BVT_COLOR_* */
} BvtStyle;

/* Resolve a cell's style. Returns NULL if the term/cell is invalid. */
const BvtStyle *bvt_cell_style(const BvtTerm *vt, const BvtCell *cell);

/* Copy a cell's grapheme codepoints into `out`. Returns the number of
 * codepoints written. `out_cap` caps the write; a return value equal
 * to `out_cap` indicates truncation. For single-cp cells this returns
 * 1 with cell->cp. */
size_t bvt_cell_get_grapheme(const BvtTerm *vt, const BvtCell *cell,
                             uint32_t *out, size_t out_cap);

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
    int row;
    int col;
} BvtPos;

typedef struct
{
    int start_row, start_col; /* inclusive */
    int end_row, end_col;     /* inclusive */
} BvtRect;

typedef struct
{
    int row;
    int col;
    bool visible;
    bool blink;
} BvtCursor;

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */

typedef enum
{
    BVT_MODE_ALTSCREEN,
    BVT_MODE_CURSOR_VISIBLE,
    BVT_MODE_CURSOR_BLINK,
    BVT_MODE_REVERSE_VIDEO, /* DECSCNM */
    BVT_MODE_BRACKETED_PASTE,
    BVT_MODE_MOUSE_X10,         /* DECSET 9 */
    BVT_MODE_MOUSE_BTN_EVENT,   /* DECSET 1000 */
    BVT_MODE_MOUSE_DRAG,        /* DECSET 1002 */
    BVT_MODE_MOUSE_ANY_EVENT,   /* DECSET 1003 */
    BVT_MODE_MOUSE_SGR,         /* DECSET 1006 */
    BVT_MODE_FOCUS_REPORTING,   /* DECSET 1004 */
    BVT_MODE_GRAPHEME_CLUSTERS, /* mode 2027 (Contour) */
    BVT_MODE_SYNC_OUTPUT,       /* mode 2026 */
} BvtMode;

/* ------------------------------------------------------------------ */
/* Keyboard / mouse                                                    */
/* ------------------------------------------------------------------ */

typedef enum
{
    BVT_MOD_NONE = 0,
    BVT_MOD_SHIFT = 1u << 0,
    BVT_MOD_ALT = 1u << 1,
    BVT_MOD_CTRL = 1u << 2,
    BVT_MOD_META = 1u << 3,
} BvtMods;

typedef enum
{
    BVT_KEY_NONE = 0,
    BVT_KEY_ENTER,
    BVT_KEY_TAB,
    BVT_KEY_BACKSPACE,
    BVT_KEY_ESCAPE,
    BVT_KEY_UP,
    BVT_KEY_DOWN,
    BVT_KEY_LEFT,
    BVT_KEY_RIGHT,
    BVT_KEY_INS,
    BVT_KEY_DEL,
    BVT_KEY_HOME,
    BVT_KEY_END,
    BVT_KEY_PAGEUP,
    BVT_KEY_PAGEDOWN,
    BVT_KEY_F1,
    BVT_KEY_F2,
    BVT_KEY_F3,
    BVT_KEY_F4,
    BVT_KEY_F5,
    BVT_KEY_F6,
    BVT_KEY_F7,
    BVT_KEY_F8,
    BVT_KEY_F9,
    BVT_KEY_F10,
    BVT_KEY_F11,
    BVT_KEY_F12,
    BVT_KEY_KP_0,
    BVT_KEY_KP_1,
    BVT_KEY_KP_2,
    BVT_KEY_KP_3,
    BVT_KEY_KP_4,
    BVT_KEY_KP_5,
    BVT_KEY_KP_6,
    BVT_KEY_KP_7,
    BVT_KEY_KP_8,
    BVT_KEY_KP_9,
    BVT_KEY_KP_MULTIPLY,
    BVT_KEY_KP_PLUS,
    BVT_KEY_KP_COMMA,
    BVT_KEY_KP_MINUS,
    BVT_KEY_KP_PERIOD,
    BVT_KEY_KP_DIVIDE,
    BVT_KEY_KP_ENTER,
    BVT_KEY_KP_EQUAL,
} BvtKey;

typedef enum
{
    BVT_MOUSE_NONE = 0,
    BVT_MOUSE_LEFT,
    BVT_MOUSE_MIDDLE,
    BVT_MOUSE_RIGHT,
    BVT_MOUSE_WHEEL_UP,
    BVT_MOUSE_WHEEL_DOWN,
} BvtMouseButton;

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* damage: a rectangular region of the visible grid changed */
    void (*damage)(BvtRect rect, void *user);

    /* moverect: contents of `src` were moved to `dst` (e.g. scroll). */
    void (*moverect)(BvtRect dst, BvtRect src, void *user);

    /* movecursor: cursor moved or visibility changed */
    void (*movecursor)(BvtCursor cur, void *user);

    /* bell: BEL received */
    void (*bell)(void *user);

    /* set_title: OSC 0/1/2 received. `utf8` is NUL-terminated and
     * valid only during the call. */
    void (*set_title)(const char *utf8, void *user);

    /* set_mode: a tracked mode changed */
    void (*set_mode)(BvtMode mode, bool on, void *user);

    /* output: bytes the application produced to send to the PTY
     * (mouse reports, DA/DSR responses, kitty keyboard responses...) */
    void (*output)(const uint8_t *bytes, size_t len, void *user);

    /* sb_pushline: a row scrolled off the top into scrollback */
    void (*sb_pushline)(const BvtCell *cells, int cols, bool wrapline, void *user);

    /* sb_popline: scrollback bottommost line is popped back onto screen */
    void (*sb_popline)(BvtCell *out_cells, int cols, void *user);

    /* osc: OSC string callback (title is delivered via set_title) */
    void (*osc)(int code, const char *data, size_t len, void *user);

    /* dcs: streamed DCS callback. `final` is true on the final chunk. */
    void (*dcs)(const char *intro, const char *data, size_t len, bool final,
                void *user);
} BvtCallbacks;

/* ------------------------------------------------------------------ */
/* Allocator hooks                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    void *(*alloc)(size_t size, void *user);
    void *(*realloc)(void *ptr, size_t size, void *user);
    void (*free)(void *ptr, void *user);
    void *user;
} BvtAllocator;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Create a terminal. Returns NULL on allocation failure. */
BvtTerm *bvt_new(int rows, int cols);

/* Variant that takes an allocator. The allocator is copied; the user
 * pointer must outlive the term. Pass NULL to use the stdlib. */
BvtTerm *bvt_new_with_allocator(int rows, int cols, const BvtAllocator *alloc);

void bvt_free(BvtTerm *vt);

/* Wire callbacks. May be called multiple times; the table is copied. */
void bvt_set_callbacks(BvtTerm *vt, const BvtCallbacks *cb, void *user);

/* Resize and reflow if reflow is enabled. Cursor position is preserved. */
void bvt_resize(BvtTerm *vt, int rows, int cols);

/* Configuration. */
void bvt_set_reflow(BvtTerm *vt, bool enabled);
void bvt_set_ambiguous_wide(BvtTerm *vt, bool wide);
void bvt_set_scrollback_size(BvtTerm *vt, int lines);

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

/* Feed PTY bytes to the parser. Returns the number consumed (always
 * `len` unless the term is in an error state). */
size_t bvt_input_write(BvtTerm *vt, const uint8_t *bytes, size_t len);

void bvt_send_key(BvtTerm *vt, BvtKey key, BvtMods mods);
void bvt_send_text(BvtTerm *vt, const char *utf8, size_t len, BvtMods mods);
void bvt_send_mouse(BvtTerm *vt, int row, int col, BvtMouseButton b,
                    bool pressed, BvtMods mods);

void bvt_paste_begin(BvtTerm *vt);
void bvt_paste_end(BvtTerm *vt);

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/* Returns NULL for out-of-range coordinates. Continuation cells
 * (width == 0) are returned as-is; callers skip them. */
const BvtCell *bvt_get_cell(const BvtTerm *vt, int row, int col);

void bvt_get_dimensions(const BvtTerm *vt, int *out_rows, int *out_cols);

int bvt_get_scrollback_lines(const BvtTerm *vt);
const BvtCell *bvt_get_scrollback_cell(const BvtTerm *vt, int sb_row, int col);
bool bvt_get_scrollback_wrapline(const BvtTerm *vt, int sb_row);

BvtCursor bvt_get_cursor(const BvtTerm *vt);
const char *bvt_get_title(const BvtTerm *vt);
bool bvt_is_altscreen(const BvtTerm *vt);
bool bvt_get_mode(const BvtTerm *vt, BvtMode mode);
bool bvt_get_line_continuation(const BvtTerm *vt, int row);

#ifdef __cplusplus
}
#endif

#endif /* BLOOM_VT_H */
