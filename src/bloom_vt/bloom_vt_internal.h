/*
 * bloom-vt internal types — not part of the public API.
 *
 * Defines the BvtTerm struct, the page layout, the style intern table,
 * and the grapheme arena. These exist in this header (rather than each
 * .c file) so that all internal translation units share the layout
 * without exposing it through bloom_vt.h.
 */

#ifndef BLOOM_VT_INTERNAL_H
#define BLOOM_VT_INTERNAL_H

#include "bloom_vt.h"

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define BVT_PAGE_BYTES        (64u * 1024u)
#define BVT_OSC_BUF_BYTES     4096u
#define BVT_CSI_PARAM_MAX     32u
#define BVT_INTERMEDIATE_MAX  4u
#define BVT_DEFAULT_SCROLLBACK 1000
#define BVT_SB_PAGE_ROWS      64u  /* rows per scrollback page */

/* Grapheme arena: codepoints / dedup table initial sizes (bytes). */
#define BVT_ARENA_CP_INIT     1024u
#define BVT_ARENA_DEDUP_INIT  64u
#define BVT_STYLES_INIT       16u

/* ------------------------------------------------------------------ */
/* Style intern table                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    BvtStyle *entries;     /* index 0 reserved for the default style */
    uint32_t  count;
    uint32_t  capacity;    /* power of two */
    uint32_t *index;       /* open-addressed: hash slot -> entries[] index */
    uint32_t  index_capacity;
} BvtStyleTable;

/* ------------------------------------------------------------------ */
/* Grapheme arena                                                      */
/* ------------------------------------------------------------------ */

/* Each entry is laid out in `codepoints` as:
 *   [u32 hash][u32 length][u32 cps[length]]
 * The grapheme_id stored on the cell is the offset of the first u32
 * (the hash word). offset 0 is reserved (single-cp cell sentinel).
 */
typedef struct {
    uint32_t *codepoints;
    uint32_t  used;
    uint32_t  capacity;
    uint32_t *dedup_index;     /* open-addressed: hash slot -> arena offset */
    uint32_t  dedup_capacity;  /* power of two */
    uint32_t  dedup_count;     /* live entries in dedup_index */
} BvtGraphemeArena;

/* ------------------------------------------------------------------ */
/* Page                                                                */
/* ------------------------------------------------------------------ */

typedef struct BvtPage {
    struct BvtPage *prev, *next;
    uint16_t cols;
    uint16_t row_count;
    uint16_t row_capacity;
    uint16_t _pad;

    BvtCell *cells;        /* row_capacity * cols */
    uint8_t *row_flags;    /* per-row flags (currently: WRAPLINE on last cell of row) */

    BvtStyleTable    styles;
    BvtGraphemeArena graphemes;

    /* No flexible array yet — initial scaffold uses sub-allocations.
     * The plan calls for a single backing buffer; we'll consolidate
     * once the grid is implemented. */
} BvtPage;

/* ------------------------------------------------------------------ */
/* Parser state                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    BVT_STATE_GROUND = 0,
    BVT_STATE_ESCAPE,
    BVT_STATE_ESCAPE_INTERMEDIATE,
    BVT_STATE_CSI_ENTRY,
    BVT_STATE_CSI_PARAM,
    BVT_STATE_CSI_INTERMEDIATE,
    BVT_STATE_CSI_IGNORE,
    BVT_STATE_DCS_ENTRY,
    BVT_STATE_DCS_PARAM,
    BVT_STATE_DCS_INTERMEDIATE,
    BVT_STATE_DCS_PASSTHROUGH,
    BVT_STATE_DCS_IGNORE,
    BVT_STATE_OSC_STRING,
    BVT_STATE_SOS_PM_APC_STRING,
} BvtParserState;

typedef struct {
    BvtParserState state;
    uint32_t       params[BVT_CSI_PARAM_MAX];
    uint8_t        param_count;
    bool           param_present;     /* whether the current slot has a digit */
    uint8_t        intermediates[BVT_INTERMEDIATE_MAX];
    uint8_t        intermediate_count;
    /* UTF-8 decoder state (Bjoern Hoehrmann) */
    uint32_t       utf8_state;
    uint32_t       utf8_codepoint;
    /* OSC accumulator */
    uint8_t        osc_buf[BVT_OSC_BUF_BYTES];
    uint16_t       osc_len;
    bool           osc_truncated;
    /* DCS streaming state — passthrough emits chunks via callback */
    bool           dcs_initial_sent;
    uint8_t        dcs_intro[BVT_INTERMEDIATE_MAX + 4]; /* params + final */
    uint8_t        dcs_intro_len;
} BvtParser;

/* ------------------------------------------------------------------ */
/* Cursor / pen state                                                  */
/* ------------------------------------------------------------------ */

#define BVT_CLUSTER_MAX 16  /* max codepoints in a single grapheme cluster */

typedef struct {
    int       row, col;
    bool      visible;
    bool      blink;
    bool      pending_wrap;  /* "deferred wrap" — DEC behavior at right margin */
    BvtStyle  pen;           /* current SGR pen */
    uint16_t  hyperlink_id;  /* OSC 8 active link, 0 = none */
    /* Pending grapheme cluster — codepoints accumulated since the last
     * boundary. Committed as a single cell on the next break or on any
     * forced flush (CSI dispatch, C0 control, etc.). */
    uint32_t  cluster_buf[BVT_CLUSTER_MAX];
    uint8_t   cluster_len;
} BvtCursorState;

/* ------------------------------------------------------------------ */
/* The terminal                                                        */
/* ------------------------------------------------------------------ */

struct BvtTerm {
    int rows;
    int cols;

    /* Active grid (visible). For now, exactly one page; later may chain. */
    BvtPage *grid;

    /* Alternate screen — saved when in altscreen. */
    BvtPage *altgrid;
    bool     in_altscreen;

    /* Scrollback page ring (head = most recent). */
    BvtPage *sb_head;
    BvtPage *sb_tail;
    int      sb_lines;
    int      sb_capacity;

    BvtCursorState cursor;
    BvtCursorState saved_cursor;

    /* Scroll region (DECSTBM). Inclusive. */
    int scroll_top;
    int scroll_bottom;

    /* Tab stops. Bit per column. */
    uint8_t *tabstops;

    BvtParser parser;
    BvtCallbacks callbacks;
    void        *callback_user;
    BvtAllocator alloc;

    /* Modes. Indexed by BvtMode enum value. */
    bool modes[32];

    /* Settings. */
    bool reflow_enabled;
    bool ambiguous_wide;
    /* Cursor key application mode (DECCKM). When true, arrow keys
     * emit ESC O <X> instead of ESC [ <X>. */
    bool decckm;
    /* Keypad application mode (DECKPAM). */
    bool deckpam;

    /* Title — owned, NUL-terminated. */
    char *title;

    /* Damage accumulator (rectangular union since last clear). */
    BvtRect damage;
    bool    damage_dirty;
};

/* ------------------------------------------------------------------ */
/* Internal helpers (cross-file)                                       */
/* ------------------------------------------------------------------ */

/* Allocator helpers — route through vt->alloc. */
void *bvt_alloc(BvtTerm *vt, size_t size);
void *bvt_realloc(BvtTerm *vt, void *ptr, size_t size);
void  bvt_dealloc(BvtTerm *vt, void *ptr);

/* Page lifecycle. */
BvtPage *bvt_page_new(BvtTerm *vt, int rows, int cols);
void     bvt_page_free(BvtTerm *vt, BvtPage *page);

/* Style intern. Returns 0 for the default style. */
uint32_t bvt_style_intern(BvtTerm *vt, BvtPage *page, const BvtStyle *style);
const BvtStyle *bvt_style_lookup(const BvtPage *page, uint32_t id);

/* Grapheme arena. */
uint32_t bvt_grapheme_intern(BvtTerm *vt, BvtPage *page,
                             const uint32_t *cps, uint32_t len);
size_t   bvt_grapheme_read(const BvtPage *page, uint32_t id,
                           uint32_t *out, size_t out_cap);

/* Parser entry. */
void bvt_parser_init(BvtParser *p);
void bvt_parser_feed(BvtTerm *vt, const uint8_t *bytes, size_t len);

/* Width helpers (width.c). */
int bvt_codepoint_width(BvtTerm *vt, uint32_t cp);
int bvt_cluster_width(BvtTerm *vt, const uint32_t *cps, uint32_t len);

/* Grid mutators (print.c). */
void bvt_grid_ensure(BvtTerm *vt);
void bvt_print_codepoint(BvtTerm *vt, uint32_t cp);
void bvt_flush_cluster(BvtTerm *vt);
void bvt_execute_c0(BvtTerm *vt, uint8_t b);
void bvt_scroll_up(BvtTerm *vt, int lines);
void bvt_scroll_down(BvtTerm *vt, int lines);
void bvt_erase_in_line(BvtTerm *vt, int mode);
void bvt_erase_in_display(BvtTerm *vt, int mode);

/* Width / grapheme break (width.c). */
bool bvt_grapheme_break_before(uint32_t prev, uint32_t cur, void *state);

/* Palette (palette.c) — resolves 0..255 indexed colors to 0x00RRGGBB. */
uint32_t bvt_palette_lookup(BvtTerm *vt, uint8_t idx);

/* Output emit (keys.c). */
void bvt_emit_bytes(BvtTerm *vt, const uint8_t *bytes, size_t len);

/* Altscreen (modes.c). */
void bvt_set_altscreen(BvtTerm *vt, bool on, bool save_restore_cursor);

/* Reflow (reflow.c). When reflow_enabled is true bvt_reflow walks the
 * grid's logical lines (rows linked by WRAPLINE) and re-wraps them at
 * the new geometry, pushing overflow into scrollback. When disabled
 * (or in altscreen) it falls through to a clamp-only resize. */
void bvt_reflow(BvtTerm *vt, int new_rows, int new_cols);
void bvt_resize_clamp(BvtTerm *vt, int new_rows, int new_cols);

/* Grid edits (print.c). */
void bvt_insert_chars(BvtTerm *vt, int count);
void bvt_delete_chars(BvtTerm *vt, int count);
void bvt_insert_lines(BvtTerm *vt, int count);
void bvt_delete_lines(BvtTerm *vt, int count);
void bvt_erase_chars(BvtTerm *vt, int count);

/* Dispatchers. */
void bvt_csi_dispatch(BvtTerm *vt, uint8_t final);
void bvt_esc_dispatch(BvtTerm *vt, uint8_t final);
void bvt_osc_dispatch(BvtTerm *vt, const uint8_t *data, size_t len);
void bvt_dcs_hook(BvtTerm *vt, uint8_t final);
void bvt_dcs_put(BvtTerm *vt, uint8_t b);
void bvt_dcs_unhook(BvtTerm *vt);

/* Damage helper. */
void bvt_damage_cell(BvtTerm *vt, int row, int col);
void bvt_damage_row(BvtTerm *vt, int row);
void bvt_damage_all(BvtTerm *vt);
void bvt_damage_flush(BvtTerm *vt);

/* Scrollback (scrollback.c). */
void bvt_scrollback_push(BvtTerm *vt, const BvtCell *src_cells, int cols, bool wrapline);
void bvt_scrollback_clear(BvtTerm *vt);

/* Page ownership lookup — used by cell accessors that must resolve
 * a cell's grapheme/style entry against the page that owns it. */
const BvtPage *bvt_find_owner_page(const BvtTerm *vt, const BvtCell *cell);

#endif /* BLOOM_VT_INTERNAL_H */
