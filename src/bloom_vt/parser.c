/*
 * bloom-vt — DEC ANSI state machine parser.
 *
 * Implements Paul Williams' state machine for VT500 series escape
 * sequences (https://vt100.net/emu/dec_ansi_parser) plus Bjoern
 * Hoehrmann's table-driven UTF-8 decoder for printable bytes in the
 * GROUND state.
 *
 * The state machine is implemented as a per-state switch. Anywhere
 * transitions (0x18, 0x1A, 0x1B, ESC variants, 0x80-0x9F C1 controls)
 * are checked first on every byte. UTF-8 decoding only runs on bytes
 * that GROUND would have printed; all other bytes flow through the
 * state machine in their raw 8-bit form, matching xterm/foot behavior.
 */

#include "bloom_vt_internal.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Hoehrmann UTF-8 decoder                                             */
/* ------------------------------------------------------------------ */

#define UTF8_ACCEPT 0
#define UTF8_REJECT 12

static const uint8_t utf8d[] = {
    /* The first part of the table maps bytes to character classes. */
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    8,
    8,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    10,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    4,
    3,
    3,
    11,
    6,
    6,
    6,
    5,
    8,
    8,
    8,
    8,
    8,
    8,
    8,
    8,
    8,
    8,
    8,
    /* The second part maps a state and class to a new state. */
    0,
    12,
    24,
    36,
    60,
    96,
    84,
    12,
    12,
    12,
    48,
    72,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    0,
    12,
    12,
    12,
    12,
    12,
    0,
    12,
    0,
    12,
    12,
    12,
    24,
    12,
    12,
    12,
    12,
    12,
    24,
    12,
    24,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    24,
    12,
    12,
    12,
    12,
    12,
    24,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    24,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    36,
    12,
    36,
    12,
    12,
    12,
    36,
    12,
    12,
    12,
    12,
    12,
    36,
    12,
    36,
    12,
    12,
    12,
    36,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
};

/* Returns the new state. UTF8_ACCEPT means a full codepoint is now in
 * `*codep`. UTF8_REJECT means the byte sequence is invalid. */
static uint32_t utf8_decode(uint32_t *state, uint32_t *codep, uint8_t byte)
{
    uint32_t type = utf8d[byte];
    *codep = (*state != UTF8_ACCEPT)
                 ? (byte & 0x3fu) | (*codep << 6)
                 : (0xffu >> type) & byte;
    *state = utf8d[256 + *state + type];
    return *state;
}

/* ------------------------------------------------------------------ */
/* Action helpers                                                      */
/* ------------------------------------------------------------------ */

static void clear_seq(BvtParser *p)
{
    p->param_count = 0;
    p->param_present = false;
    p->params[0] = 0;
    p->intermediate_count = 0;
}

static void param_digit(BvtParser *p, uint8_t b)
{
    if (p->param_count == 0) {
        p->param_count = 1;
        p->params[0] = 0;
    }
    uint32_t *slot = &p->params[p->param_count - 1];
    /* Cap at 65535 like xterm. */
    if (*slot < 65535) {
        *slot = (*slot * 10u) + (uint32_t)(b - '0');
        if (*slot > 65535)
            *slot = 65535;
    }
    p->param_present = true;
}

static void param_separator(BvtParser *p)
{
    if (p->param_count < BVT_CSI_PARAM_MAX) {
        p->param_count = (p->param_count == 0) ? 2 : p->param_count + 1;
        p->params[p->param_count - 1] = 0;
    }
    p->param_present = false;
}

static void collect_intermediate(BvtParser *p, uint8_t b)
{
    if (p->intermediate_count < BVT_INTERMEDIATE_MAX) {
        p->intermediates[p->intermediate_count++] = b;
    }
}

static void osc_start(BvtParser *p)
{
    p->osc_len = 0;
    p->osc_truncated = false;
}

static void osc_put(BvtParser *p, uint8_t b)
{
    if (p->osc_len < BVT_OSC_BUF_BYTES) {
        p->osc_buf[p->osc_len++] = b;
    } else {
        p->osc_truncated = true;
    }
}

/* ------------------------------------------------------------------ */
/* DCS streaming                                                       */
/* ------------------------------------------------------------------ */

static void dcs_finish(BvtTerm *vt)
{
    /* Final empty chunk to signal end of DCS to the consumer. */
    if (vt->parser.dcs_initial_sent && vt->callbacks.dcs) {
        vt->callbacks.dcs((const char *)vt->parser.dcs_intro,
                          NULL, 0, true, vt->callback_user);
    }
    vt->parser.dcs_initial_sent = false;
    vt->parser.dcs_intro_len = 0;
}

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */

static bool is_intermediate(uint8_t b) { return b >= 0x20 && b <= 0x2f; }
static bool is_final(uint8_t b) { return b >= 0x40 && b <= 0x7e; }

/*
 * "Anywhere" transitions per the Williams diagram. Returns true if the
 * byte was handled and no further state-specific processing is needed.
 *
 * In UTF-8 mode (the only mode bloom-vt supports) bare 0x80-0x9F bytes
 * are NOT treated as C1 controls in GROUND — they are continuation
 * bytes for multi-byte UTF-8. C1 semantics are only reachable via the
 * 7-bit "ESC X" forms. This matches xterm's UTF-8 behavior.
 *
 * Inside escape/CSI/DCS states we never see 0x80-0x9F as a C1 because
 * the parser only consumes 7-bit-clean control bytes in those paths;
 * payload bytes in OSC/DCS pass through their state handlers directly.
 */
static bool anywhere_transition(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;

    /* CAN, SUB → cancel current sequence and return to ground */
    if (b == 0x18 || b == 0x1a) {
        if (p->state == BVT_STATE_DCS_PASSTHROUGH)
            dcs_finish(vt);
        if (b == 0x1a)
            bvt_execute_c0(vt, b);
        p->state = BVT_STATE_GROUND;
        clear_seq(p);
        return true;
    }
    /* ESC: enter ESCAPE state regardless. */
    if (b == 0x1b) {
        if (p->state == BVT_STATE_DCS_PASSTHROUGH)
            dcs_finish(vt);
        p->state = BVT_STATE_ESCAPE;
        clear_seq(p);
        p->utf8_state = UTF8_ACCEPT;
        return true;
    }
    return false;
}

static void state_ground(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20) {
        bvt_execute_c0(vt, b);
        return;
    }
    if (b == 0x7f) {
        /* DEL ignored in ground per the diagram. */
        return;
    }
    /* UTF-8 decode for 0x20..0x7e ASCII and 0xC0+ multibyte leads. */
    if (b < 0x80) {
        p->utf8_state = UTF8_ACCEPT;
        bvt_print_codepoint(vt, (uint32_t)b);
        return;
    }
    uint32_t st = utf8_decode(&p->utf8_state, &p->utf8_codepoint, b);
    if (st == UTF8_ACCEPT) {
        bvt_print_codepoint(vt, p->utf8_codepoint);
    } else if (st == UTF8_REJECT) {
        /* Replacement character. */
        p->utf8_state = UTF8_ACCEPT;
        bvt_print_codepoint(vt, 0xfffd);
    }
    /* Otherwise still waiting for continuation bytes. */
}

static void state_escape(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20) {
        bvt_execute_c0(vt, b);
        return;
    }
    if (is_intermediate(b)) {
        collect_intermediate(p, b);
        p->state = BVT_STATE_ESCAPE_INTERMEDIATE;
        return;
    }
    /* Final byte. */
    switch (b) {
    case 0x5b:
        p->state = BVT_STATE_CSI_ENTRY;
        clear_seq(p);
        return; /* [ */
    case 0x5d:
        p->state = BVT_STATE_OSC_STRING;
        osc_start(p);
        return; /* ] */
    case 0x50:
        p->state = BVT_STATE_DCS_ENTRY;
        clear_seq(p);
        return; /* P */
    case 0x58:
    case 0x5e:
    case 0x5f:
        p->state = BVT_STATE_SOS_PM_APC_STRING;
        return; /* X ^ _ */
    default:
        bvt_esc_dispatch(vt, b);
        p->state = BVT_STATE_GROUND;
        return;
    }
}

static void state_escape_intermediate(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20) {
        bvt_execute_c0(vt, b);
        return;
    }
    if (is_intermediate(b)) {
        collect_intermediate(p, b);
        return;
    }
    /* Final */
    bvt_esc_dispatch(vt, b);
    p->state = BVT_STATE_GROUND;
}

static void state_csi_entry(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20) {
        bvt_execute_c0(vt, b);
        return;
    }
    if (b >= '0' && b <= '9') {
        param_digit(p, b);
        p->state = BVT_STATE_CSI_PARAM;
        return;
    }
    if (b == ';' || b == ':') {
        param_separator(p);
        p->state = BVT_STATE_CSI_PARAM;
        return;
    }
    if (b >= '<' && b <= '?') { /* private markers: < = > ? */
        collect_intermediate(p, b);
        p->state = BVT_STATE_CSI_PARAM;
        return;
    }
    if (is_intermediate(b)) {
        collect_intermediate(p, b);
        p->state = BVT_STATE_CSI_INTERMEDIATE;
        return;
    }
    if (is_final(b)) {
        bvt_csi_dispatch(vt, b);
        p->state = BVT_STATE_GROUND;
        return;
    }
    p->state = BVT_STATE_CSI_IGNORE;
}

static void state_csi_param(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20) {
        bvt_execute_c0(vt, b);
        return;
    }
    if (b >= '0' && b <= '9') {
        param_digit(p, b);
        return;
    }
    if (b == ';' || b == ':') {
        param_separator(p);
        return;
    }
    if (b >= '<' && b <= '?') {
        /* Mid-parameter private markers are ignored after CSI_ENTRY. */
        p->state = BVT_STATE_CSI_IGNORE;
        return;
    }
    if (is_intermediate(b)) {
        collect_intermediate(p, b);
        p->state = BVT_STATE_CSI_INTERMEDIATE;
        return;
    }
    if (is_final(b)) {
        bvt_csi_dispatch(vt, b);
        p->state = BVT_STATE_GROUND;
        return;
    }
    p->state = BVT_STATE_CSI_IGNORE;
}

static void state_csi_intermediate(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20) {
        bvt_execute_c0(vt, b);
        return;
    }
    if (is_intermediate(b)) {
        collect_intermediate(p, b);
        return;
    }
    if (b >= 0x30 && b <= 0x3f) {
        p->state = BVT_STATE_CSI_IGNORE;
        return;
    }
    if (is_final(b)) {
        bvt_csi_dispatch(vt, b);
        p->state = BVT_STATE_GROUND;
        return;
    }
    p->state = BVT_STATE_CSI_IGNORE;
}

static void state_csi_ignore(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20) {
        bvt_execute_c0(vt, b);
        return;
    }
    if (is_final(b)) {
        p->state = BVT_STATE_GROUND;
        return;
    }
    /* swallow */
}

static void state_dcs_entry(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20)
        return; /* ignore */
    if (b >= '0' && b <= '9') {
        param_digit(p, b);
        p->state = BVT_STATE_DCS_PARAM;
        return;
    }
    if (b == ';' || b == ':') {
        param_separator(p);
        p->state = BVT_STATE_DCS_PARAM;
        return;
    }
    if (b >= '<' && b <= '?') {
        collect_intermediate(p, b);
        p->state = BVT_STATE_DCS_PARAM;
        return;
    }
    if (is_intermediate(b)) {
        collect_intermediate(p, b);
        p->state = BVT_STATE_DCS_INTERMEDIATE;
        return;
    }
    if (is_final(b)) {
        bvt_dcs_hook(vt, b);
        p->state = BVT_STATE_DCS_PASSTHROUGH;
        return;
    }
    p->state = BVT_STATE_DCS_IGNORE;
}

static void state_dcs_param(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20)
        return;
    if (b >= '0' && b <= '9') {
        param_digit(p, b);
        return;
    }
    if (b == ';' || b == ':') {
        param_separator(p);
        return;
    }
    if (is_intermediate(b)) {
        collect_intermediate(p, b);
        p->state = BVT_STATE_DCS_INTERMEDIATE;
        return;
    }
    if (is_final(b)) {
        bvt_dcs_hook(vt, b);
        p->state = BVT_STATE_DCS_PASSTHROUGH;
        return;
    }
    if (b >= '<' && b <= '?') {
        p->state = BVT_STATE_DCS_IGNORE;
        return;
    }
    p->state = BVT_STATE_DCS_IGNORE;
}

static void state_dcs_intermediate(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    if (b < 0x20)
        return;
    if (is_intermediate(b)) {
        collect_intermediate(p, b);
        return;
    }
    if (is_final(b)) {
        bvt_dcs_hook(vt, b);
        p->state = BVT_STATE_DCS_PASSTHROUGH;
        return;
    }
    if (b >= 0x30 && b <= 0x3f) {
        p->state = BVT_STATE_DCS_IGNORE;
        return;
    }
    p->state = BVT_STATE_DCS_IGNORE;
}

static void state_dcs_passthrough(BvtTerm *vt, uint8_t b)
{
    /* ESC and 0x9C terminate; both are handled by anywhere_transition. */
    bvt_dcs_put(vt, b);
}

static void state_dcs_ignore(BvtTerm *vt, uint8_t b)
{
    (void)vt;
    (void)b;
    /* Swallow until ST (handled by anywhere_transition). */
}

static void state_osc_string(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    /* BEL terminates per xterm extension. ST (ESC \ or 0x9C) handled
     * by anywhere_transition / state_escape. */
    if (b == 0x07) {
        bvt_osc_dispatch(vt, p->osc_buf, p->osc_len);
        p->state = BVT_STATE_GROUND;
        return;
    }
    if (b < 0x20) {
        /* xterm tolerates CR/LF in OSC; otherwise the C0 control aborts. */
        if (b == 0x0a || b == 0x0d)
            return;
        p->state = BVT_STATE_GROUND;
        return;
    }
    osc_put(p, b);
}

static void state_sos_pm_apc(BvtTerm *vt, uint8_t b)
{
    (void)vt;
    (void)b;
    /* Swallow until terminated by ESC \ / 0x9C / 0x18 / 0x1A. */
}

/* Special: ESCAPE state on 0x5C (\) terminates strings (ST). */
static void escape_st_check(BvtTerm *vt, uint8_t b, bool *handled)
{
    BvtParser *p = &vt->parser;
    if (p->state != BVT_STATE_ESCAPE || b != 0x5c) {
        *handled = false;
        return;
    }
    /* ESC \ is the 7-bit ST. Terminate any active string. */
    *handled = true;
    /* Look at where we came from: the parser doesn't record that
     * directly, so we infer from any open string state at ESC entry. */
    /* In practice: ESC fires anywhere_transition which clears DCS,
     * so the only string state surviving ESC is OSC, which we handle
     * here. */
    (void)vt;
    p->state = BVT_STATE_GROUND;
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

void bvt_parser_init(BvtParser *p)
{
    memset(p, 0, sizeof(*p));
    p->state = BVT_STATE_GROUND;
    p->utf8_state = UTF8_ACCEPT;
}

void bvt_parser_feed(BvtTerm *vt, const uint8_t *bytes, size_t len)
{
    BvtParser *p = &vt->parser;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = bytes[i];

        /* OSC accepts 8-bit bytes through ST/BEL termination, so the
         * anywhere check for ESC is the only one that should fire
         * inside an OSC string. */
        if (p->state == BVT_STATE_OSC_STRING) {
            if (b == 0x1b) {
                /* ESC \ pending — finish OSC, then go to ESCAPE. */
                bvt_osc_dispatch(vt, p->osc_buf, p->osc_len);
                p->state = BVT_STATE_ESCAPE;
                clear_seq(p);
                p->utf8_state = UTF8_ACCEPT;
                continue;
            }
            /* Note: in UTF-8 mode (always, for bvt) we do NOT treat bare
             * 0x9C as the C1 ST. Many UTF-8 codepoints encode 0x9C as
             * their trailing continuation byte (e.g. U+201C "left double
             * quotation mark" is 0xE2 0x80 0x9C); aborting the OSC there
             * would emit a partial codepoint and break consumers like
             * GTK/Pango that validate UTF-8 strictly. Only ESC \ and BEL
             * terminate. */
            state_osc_string(vt, b);
            continue;
        }

        if (anywhere_transition(vt, b))
            continue;

        /* ESC then 0x5C → ST. Handled before state dispatch so we
         * don't try to esc_dispatch a backslash. */
        if (p->state == BVT_STATE_ESCAPE && b == 0x5c) {
            p->state = BVT_STATE_GROUND;
            continue;
        }

        switch (p->state) {
        case BVT_STATE_GROUND:
            state_ground(vt, b);
            break;
        case BVT_STATE_ESCAPE:
            state_escape(vt, b);
            break;
        case BVT_STATE_ESCAPE_INTERMEDIATE:
            state_escape_intermediate(vt, b);
            break;
        case BVT_STATE_CSI_ENTRY:
            state_csi_entry(vt, b);
            break;
        case BVT_STATE_CSI_PARAM:
            state_csi_param(vt, b);
            break;
        case BVT_STATE_CSI_INTERMEDIATE:
            state_csi_intermediate(vt, b);
            break;
        case BVT_STATE_CSI_IGNORE:
            state_csi_ignore(vt, b);
            break;
        case BVT_STATE_DCS_ENTRY:
            state_dcs_entry(vt, b);
            break;
        case BVT_STATE_DCS_PARAM:
            state_dcs_param(vt, b);
            break;
        case BVT_STATE_DCS_INTERMEDIATE:
            state_dcs_intermediate(vt, b);
            break;
        case BVT_STATE_DCS_PASSTHROUGH:
            state_dcs_passthrough(vt, b);
            break;
        case BVT_STATE_DCS_IGNORE:
            state_dcs_ignore(vt, b);
            break;
        case BVT_STATE_OSC_STRING: /* handled above */
            break;
        case BVT_STATE_SOS_PM_APC_STRING:
            state_sos_pm_apc(vt, b);
            break;
        }
    }
    /* Suppress unused warning for the helper kept for documentation. */
    (void)escape_st_check;
}
