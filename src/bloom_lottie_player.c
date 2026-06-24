/*
 * bloom-lottie-player — Play Lottie animations in bloom-terminal.
 *
 * A pure TUI application that emits APC escape sequences to the terminal.
 * No dependency on SDL3, FreeType, bloom-vt, or any bloom-terminal internals.
 * Only needs libc.
 *
 * Usage:
 *   bloom-lottie-player [options] <file.json>
 *
 * Options:
 *   -l, --loop          Loop playback (default)
 *   -L, --no-loop       Play once and exit
 *   -s, --speed <rate>  Playback speed multiplier (default: 1.0)
 *   --bg                Render as background layer (behind text)
 *   --opacity <val>     Opacity 0.0-1.0 (default: 1.0)
 *   -h, --help          Show help
 *
 * Controls:
 *   space    Pause / resume
 *   ←/→      Seek -5 / +5 frames
 *   +/-      Speed up / slow down
 *   r        Restart from frame 0
 *   L        Toggle loop
 *   b        Toggle background/foreground layer
 *   q/Esc    Quit
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define ANIM_ID           1
#define DEFAULT_CELL_W_PX 10
#define DEFAULT_CELL_H_PX 20
#define MAX_APC_PAYLOAD   65536
#define BASE64_CHUNK_SIZE 4096
#define INFO_BAR_HEIGHT   1
#define BORDER_TOP        1
#define BORDER_BOTTOM     1

/* ------------------------------------------------------------------ */
/*  Base64 encoder                                                    */
/* ------------------------------------------------------------------ */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *src, size_t len, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t n = (uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8 |
                     src[i + 2];
        out[j++] = b64_table[(n >> 18) & 63];
        out[j++] = b64_table[(n >> 12) & 63];
        out[j++] = b64_table[(n >> 6) & 63];
        out[j++] = b64_table[n & 63];
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)src[i] << 16;
        out[j++] = b64_table[(n >> 18) & 63];
        out[j++] = b64_table[(n >> 12) & 63];
        out[j++] = '=';
        out[j++] = '=';
    } else if (rem == 2) {
        uint32_t n = (uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8;
        out[j++] = b64_table[(n >> 18) & 63];
        out[j++] = b64_table[(n >> 12) & 63];
        out[j++] = b64_table[(n >> 6) & 63];
        out[j++] = '=';
    }
    out[j] = '\0';
    return j;
}

/* ------------------------------------------------------------------ */
/*  APC emission                                                      */
/* ------------------------------------------------------------------ */

/* Emit an APC sequence: ESC _ <base64-json> ESC \ */
static void apc(const char *json, size_t json_len)
{
    size_t b64_size = ((json_len + 2) / 3) * 4 + 1;
    char *b64 = malloc(b64_size);
    if (!b64)
        return;
    base64_encode((const uint8_t *)json, json_len, b64);

    /* Use write() to avoid stdio buffering issues with escape sequences */
    char header[] = "\x1b_";
    char trailer[] = "\x1b\\";
    write(STDOUT_FILENO, header, sizeof(header) - 1);
    write(STDOUT_FILENO, b64, strlen(b64));
    write(STDOUT_FILENO, trailer, sizeof(trailer) - 1);

    free(b64);
}

/*
 * Emit a load command with chunked upload for large payloads.
 * The APC protocol supports load-chunk for files that exceed the
 * practical single-APC size limit.
 */
static void apc_load(const char *json, size_t json_len, int row, int col,
                     int rows, int cols, const char *layer, double opacity,
                     double speed, bool loop, bool autostart)
{
    /* Build the JSON command (without the lottie payload yet) to check size */
    /* For small files, send a single load command */
    size_t b64_size = ((json_len + 2) / 3) * 4 + 1;
    if (b64_size < MAX_APC_PAYLOAD) {
        char cmd[MAX_APC_PAYLOAD];
        int n = snprintf(cmd, sizeof(cmd),
                         "{\"cmd\":\"load\",\"id\":%d,"
                         "\"lottie\":%s,"
                         "\"placement\":{\"row\":%d,\"col\":%d,"
                         "\"rows\":%d,\"cols\":%d},"
                         "\"layer\":\"%s\",\"opacity\":%.2f,"
                         "\"play\":{\"speed\":%.2f,\"loop\":%s,"
                         "\"autostart\":%s}}",
                         ANIM_ID, json, row, col, rows, cols,
                         layer, opacity, speed,
                         loop ? "true" : "false",
                         autostart ? "true" : "false");
        if (n > 0 && (size_t)n < sizeof(cmd))
            apc(cmd, (size_t)n);
        return;
    }

    /* Chunked upload for large files */
    size_t b64_len = base64_encode((const uint8_t *)json, json_len, NULL);
    (void)b64_len;
    char *b64 = malloc(((json_len + 2) / 3) * 4 + 1);
    if (!b64)
        return;
    size_t actual_b64 = base64_encode((const uint8_t *)json, json_len, b64);

    int total_chunks = (int)(actual_b64 / BASE64_CHUNK_SIZE) + 1;
    for (int seq = 0; seq < total_chunks; seq++) {
        size_t offset = (size_t)seq * BASE64_CHUNK_SIZE;
        size_t chunk_len = BASE64_CHUNK_SIZE;
        if (offset + chunk_len > actual_b64)
            chunk_len = actual_b64 - offset;

        char cmd[BASE64_CHUNK_SIZE + 512];
        char chunk_data[BASE64_CHUNK_SIZE + 1];
        memcpy(chunk_data, b64 + offset, chunk_len);
        chunk_data[chunk_len] = '\0';

        int n = snprintf(cmd, sizeof(cmd),
                         "{\"cmd\":\"load-chunk\",\"id\":%d,"
                         "\"seq\":%d,\"total\":%d,\"data\":\"%s\"}",
                         ANIM_ID, seq, total_chunks, chunk_data);
        if (n > 0 && (size_t)n < sizeof(cmd))
            apc(cmd, (size_t)n);
    }

    /* After chunks are sent, emit a place command to position the animation */
    char place_cmd[512];
    int n = snprintf(place_cmd, sizeof(place_cmd),
                     "{\"cmd\":\"place\",\"id\":%d,"
                     "\"placement\":{\"row\":%d,\"col\":%d,"
                     "\"rows\":%d,\"cols\":%d},"
                     "\"layer\":\"%s\",\"opacity\":%.2f}",
                     ANIM_ID, row, col, rows, cols, layer, opacity);
    if (n > 0 && (size_t)n < sizeof(place_cmd))
        apc(place_cmd, (size_t)n);

    /* Start playback */
    char play_cmd[256];
    n = snprintf(play_cmd, sizeof(play_cmd),
                 "{\"cmd\":\"play\",\"id\":%d,\"speed\":%.2f,\"loop\":%s}",
                 ANIM_ID, speed, loop ? "true" : "false");
    if (n > 0 && (size_t)n < sizeof(play_cmd))
        apc(play_cmd, (size_t)n);

    free(b64);
}

static void apc_pause(void)
{
    const char cmd[] = "{\"cmd\":\"pause\",\"id\":1}";
    apc(cmd, sizeof(cmd) - 1);
}

static void apc_play(double speed, bool loop)
{
    char cmd[256];
    int n = snprintf(cmd, sizeof(cmd),
                     "{\"cmd\":\"play\",\"id\":%d,\"speed\":%.2f,\"loop\":%s}",
                     ANIM_ID, speed, loop ? "true" : "false");
    if (n > 0)
        apc(cmd, (size_t)n);
}

static void apc_seek(int frame)
{
    char cmd[128];
    int n = snprintf(cmd, sizeof(cmd),
                     "{\"cmd\":\"seek\",\"id\":%d,\"frame\":%d}",
                     ANIM_ID, frame);
    if (n > 0)
        apc(cmd, (size_t)n);
}

static void apc_stop(void)
{
    const char cmd[] = "{\"cmd\":\"stop\",\"id\":1}";
    apc(cmd, sizeof(cmd) - 1);
}

static void apc_delete(void)
{
    const char cmd[] = "{\"cmd\":\"delete\",\"id\":1}";
    apc(cmd, sizeof(cmd) - 1);
}

static void apc_place(int row, int col, int rows, int cols,
                      const char *layer, double opacity)
{
    char cmd[512];
    int n = snprintf(cmd, sizeof(cmd),
                     "{\"cmd\":\"place\",\"id\":%d,"
                     "\"placement\":{\"row\":%d,\"col\":%d,"
                     "\"rows\":%d,\"cols\":%d},"
                     "\"layer\":\"%s\",\"opacity\":%.2f}",
                     ANIM_ID, row, col, rows, cols, layer, opacity);
    if (n > 0 && (size_t)n < sizeof(cmd))
        apc(cmd, (size_t)n);
}

/* ------------------------------------------------------------------ */
/*  Terminal control                                                  */
/* ------------------------------------------------------------------ */

static struct termios saved_termios;
static bool raw_mode_active = false;

static void enter_raw_mode(void)
{
    tcgetattr(STDIN_FILENO, &saved_termios);
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = true;
}

static void exit_raw_mode(void)
{
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        raw_mode_active = false;
    }
}

static void get_terminal_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
        ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

/* ------------------------------------------------------------------ */
/*  TUI drawing                                                       */
/* ------------------------------------------------------------------ */

static void enter_alt_screen(void)
{
    write(STDOUT_FILENO, "\x1b[?1049h", 8);
}

static void exit_alt_screen(void)
{
    write(STDOUT_FILENO, "\x1b[?1049l", 8);
}

static void clear_screen(void)
{
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
}

static void hide_cursor(void)
{
    write(STDOUT_FILENO, "\x1b[?25l", 6);
}

static void show_cursor(void)
{
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}

static void move_cursor(int row, int col)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    if (n > 0)
        write(STDOUT_FILENO, buf, (size_t)n);
}

/*
 * Draw the TUI frame: a box border around the animation area and an
 * info bar at the bottom. The animation placement is the interior of
 * the box.
 */
static void draw_frame(const char *filename, int total_frames, int fps,
                       double speed, bool playing, bool loop, bool bg_layer,
                       double opacity,
                       int term_rows, int term_cols,
                       int anim_row, int anim_col,
                       int anim_rows, int anim_cols)
{
    (void)anim_rows;
    (void)anim_cols;

    clear_screen();

    /* Top border */
    move_cursor(1, 1);
    write(STDOUT_FILENO, "\x1b[38;5;117m", 12);
    write(STDOUT_FILENO, "\xe2\x94\x8c", 3); /* ┌ */
    for (int i = 0; i < term_cols - 2; i++)
        write(STDOUT_FILENO, "\xe2\x94\x80", 3); /* ─ */
    write(STDOUT_FILENO, "\xe2\x94\x90", 3);     /* ┐ */

    /* Title in top border */
    move_cursor(1, 4);
    char title[256];
    int n = snprintf(title, sizeof(title),
                     "\x1b[38;5;117m bloom-lottie-player \x1b[0m"
                     "\x1b[38;5;245m%s\x1b[0m",
                     filename);
    if (n > 0)
        write(STDOUT_FILENO, title, (size_t)n);

    /* Side borders */
    for (int r = 2; r < term_rows - 1; r++) {
        move_cursor(r, 1);
        write(STDOUT_FILENO, "\x1b[38;5;240m", 12);
        write(STDOUT_FILENO, "\xe2\x94\x82", 3); /* │ */
        move_cursor(r, term_cols);
        write(STDOUT_FILENO, "\xe2\x94\x82", 3); /* │ */
    }

    /* Bottom border (above info bar) */
    move_cursor(term_rows - 1, 1);
    write(STDOUT_FILENO, "\x1b[38;5;117m", 12);
    write(STDOUT_FILENO, "\xe2\x94\x94", 3); /* └ */
    for (int i = 0; i < term_cols - 2; i++)
        write(STDOUT_FILENO, "\xe2\x94\x80", 3); /* ─ */
    write(STDOUT_FILENO, "\xe2\x94\x98", 3);     /* ┘ */

    /* Info bar */
    move_cursor(term_rows, 1);
    write(STDOUT_FILENO, "\x1b[38;5;250m\x1b[44m", 16);
    char bar[512];
    const char *play_icon = playing ? "\xe2\x8f\xb5" : "\xe2\x8f\xb8"; /* ⏵ / ⏸ */
    const char *loop_icon = loop ? "loop" : "once";
    const char *layer_icon = bg_layer ? "bg" : "fg";
    n = snprintf(bar, sizeof(bar),
                 " %s  %dfps  %.1fx  %s  %s  %.0f%%  [space] pause  [q] quit ",
                 play_icon, fps, speed, loop_icon, layer_icon, opacity * 100);
    if (n > 0) {
        write(STDOUT_FILENO, bar, (size_t)n);
        /* Pad rest of bar with spaces */
        int pad = term_cols - n - 1;
        for (int i = 0; i < pad; i++)
            write(STDOUT_FILENO, " ", 1);
    }
    write(STDOUT_FILENO, "\x1b[0m", 4);

    (void)total_frames;
    (void)fps;
    (void)anim_row;
    (void)anim_col;
}

static void redraw_info_bar(int current_frame, int total_frames, int fps,
                            double speed, bool playing, bool loop,
                            bool bg_layer, double opacity,
                            int term_rows, int term_cols)
{
    move_cursor(term_rows, 1);
    char bar[512];
    const char *play_icon =
        playing ? "\xe2\x8f\xb5" : "\xe2\x8f\xb8"; /* ⏵ / ⏸ */
    const char *loop_str = loop ? "loop" : "once";
    const char *layer_str = bg_layer ? "bg" : "fg";
    int n = snprintf(bar, sizeof(bar),
                     "\x1b[38;5;250m\x1b[44m %s  frame: %d/%d  %dfps  %.1fx  "
                     "%s  %s  %.0f%% \x1b[0m",
                     play_icon, current_frame, total_frames, fps, speed,
                     loop_str, layer_str, opacity * 100);
    if (n > 0) {
        write(STDOUT_FILENO, bar, (size_t)n);
        int pad = term_cols - n - 1;
        for (int i = 0; i < pad; i++)
            write(STDOUT_FILENO, " ", 1);
        write(STDOUT_FILENO, "\x1b[0m", 4);
    }
}

/* ------------------------------------------------------------------ */
/*  JSON helpers (minimal — just extract w, h, fr, ip, op)            */
/* ------------------------------------------------------------------ */

struct LottieMeta
{
    int w;
    int h;
    int fr;
    int ip;
    int op;
};

/*
 * Minimal JSON value extraction. Finds "key":number and returns the value.
 * Not a real JSON parser — just enough for Lottie metadata fields.
 */
static int json_find_int(const char *json, size_t len, const char *key)
{
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0)
        return -1;

    const char *p = json;
    const char *end = json + len;
    while (p < end) {
        const char *found = memmem(p, (size_t)(end - p), pattern, (size_t)n);
        if (!found)
            return -1;
        const char *colon = found + n;
        while (colon < end && (*colon == ' ' || *colon == '\t' ||
                               *colon == ':'))
            colon++;
        if (colon >= end)
            return -1;

        /* Handle negative numbers */
        int sign = 1;
        if (*colon == '-') {
            sign = -1;
            colon++;
        }
        if (*colon < '0' || *colon > '9') {
            p = found + n;
            continue;
        }
        int val = 0;
        while (colon < end && *colon >= '0' && *colon <= '9') {
            val = val * 10 + (*colon - '0');
            colon++;
        }
        return val * sign;
    }
    return -1;
}

static void parse_lottie_meta(const char *json, size_t len,
                              struct LottieMeta *meta)
{
    meta->w = json_find_int(json, len, "w");
    meta->h = json_find_int(json, len, "h");
    meta->fr = json_find_int(json, len, "fr");
    meta->ip = json_find_int(json, len, "ip");
    meta->op = json_find_int(json, len, "op");

    if (meta->w <= 0)
        meta->w = 100;
    if (meta->h <= 0)
        meta->h = 100;
    if (meta->fr <= 0)
        meta->fr = 30;
    if (meta->ip < 0)
        meta->ip = 0;
    if (meta->op <= 0)
        meta->op = meta->ip + 60;
}

/* ------------------------------------------------------------------ */
/*  File reading                                                      */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open '%s': %s\n", path,
                strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';
    *out_len = nread;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Placement computation                                             */
/* ------------------------------------------------------------------ */

static void compute_placement(int canvas_w, int canvas_h,
                              int term_rows, int term_cols,
                              int *place_row, int *place_col,
                              int *place_rows, int *place_cols)
{
    /* Available area: inside the box border (top + bottom) and info bar */
    int avail_rows = term_rows - BORDER_TOP - BORDER_BOTTOM - INFO_BAR_HEIGHT;
    int avail_cols = term_cols - 2; /* left + right border */

    /* Terminal cells are typically ~2:1 (height:width). Convert the
     * Lottie canvas to cell units using that ratio so the aspect ratio
     * is approximately preserved. */
    int cols, rows;

    /* Fit within available area, preserving aspect ratio.
     * Assume each cell is approximately DEFAULT_CELL_W_PX wide and
     * DEFAULT_CELL_H_PX tall. */
    cols = (canvas_w + DEFAULT_CELL_W_PX - 1) / DEFAULT_CELL_W_PX;
    rows = (canvas_h + DEFAULT_CELL_H_PX - 1) / DEFAULT_CELL_H_PX;

    /* Scale down to fit available area, preserving aspect ratio */
    if (cols > avail_cols || rows > avail_rows) {
        float scale_w = (float)avail_cols / (float)cols;
        float scale_h = (float)avail_rows / (float)rows;
        float scale = scale_w < scale_h ? scale_w : scale_h;
        cols = (int)((float)cols * scale);
        rows = (int)((float)rows * scale);
        if (cols < 1)
            cols = 1;
        if (rows < 1)
            rows = 1;
    }

    /* Center in available area */
    int start_row = BORDER_TOP + 1;
    int start_col = 2;
    *place_row = start_row + (avail_rows - rows) / 2;
    *place_col = start_col + (avail_cols - cols) / 2;
    *place_rows = rows;
    *place_cols = cols;

    if (*place_row < 1)
        *place_row = 1;
    if (*place_col < 1)
        *place_col = 1;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <file.json>\n\n"
            "Options:\n"
            "  -l, --loop          Loop playback (default)\n"
            "  -L, --no-loop       Play once and exit\n"
            "  -s, --speed <rate>  Playback speed (default: 1.0)\n"
            "      --bg            Render as background layer\n"
            "      --opacity <val> Opacity 0.0-1.0 (default: 1.0)\n"
            "  -h, --help          Show this help\n\n"
            "Controls:\n"
            "  space    Pause / resume\n"
            "  ←/→      Seek -5 / +5 frames\n"
            "  +/-      Speed up / slow down\n"
            "  r        Restart from frame 0\n"
            "  L        Toggle loop\n"
            "  b        Toggle background/foreground\n"
            "  [ / ]    Decrease / increase opacity (10%% steps)\n"
            "  q/Esc    Quit\n",
            prog);
}

int main(int argc, char **argv)
{
    bool loop = true;
    double speed = 1.0;
    double opacity = 1.0;
    bool bg_layer = false;

    static struct option long_opts[] = {
        { "loop", no_argument, NULL, 'l' },
        { "no-loop", no_argument, NULL, 'L' },
        { "speed", required_argument, NULL, 's' },
        { "bg", no_argument, NULL, 1001 },
        { "opacity", required_argument, NULL, 1002 },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "lLs:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'l':
            loop = true;
            break;
        case 'L':
            loop = false;
            break;
        case 's':
            speed = atof(optarg);
            if (speed <= 0)
                speed = 1.0;
            break;
        case 1001:
            bg_layer = true;
            break;
        case 1002:
            opacity = atof(optarg);
            if (opacity < 0)
                opacity = 0;
            if (opacity > 1)
                opacity = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "ERROR: No input file specified\n\n");
        usage(argv[0]);
        return 1;
    }

    const char *filepath = argv[optind];

    /* Read the lottie file */
    size_t json_len = 0;
    char *json = read_file(filepath, &json_len);
    if (!json)
        return 1;

    /* Parse metadata */
    struct LottieMeta meta;
    parse_lottie_meta(json, json_len, &meta);
    int total_frames = meta.op - meta.ip;
    if (total_frames <= 0)
        total_frames = 60;

    /* Get terminal size */
    int term_rows, term_cols;
    get_terminal_size(&term_rows, &term_cols);

    /* Compute placement (centered in the box interior) */
    int place_row, place_col, place_rows, place_cols;
    compute_placement(meta.w, meta.h, term_rows, term_cols,
                      &place_row, &place_col, &place_rows, &place_cols);

    const char *layer_str = bg_layer ? "background" : "foreground";

    /* Setup terminal */
    enter_alt_screen();
    enter_raw_mode();
    hide_cursor();
    clear_screen();

    /* Draw TUI frame */
    const char *basename = strrchr(filepath, '/');
    basename = basename ? basename + 1 : filepath;
    draw_frame(basename, total_frames, meta.fr, speed, true, loop, bg_layer,
               opacity,
               term_rows, term_cols, place_row, place_col,
               place_rows, place_cols);

    /* Load and start the animation */
    apc_load(json, json_len, place_row, place_col,
             place_rows, place_cols, layer_str, opacity,
             speed, loop, true);

    /* Main loop */
    bool playing = true;
    bool running = true;
    int current_frame = 0;
    struct timespec last_time;
    clock_gettime(CLOCK_MONOTONIC, &last_time);

    while (running) {
        /* Read key with 100ms timeout */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

        if (ret > 0) {
            char buf[16];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    char ch = buf[i];
                    /* Handle escape sequences for arrow keys */
                    if (ch == '\x1b' && i + 2 < n) {
                        if (buf[i + 1] == '[') {
                            if (buf[i + 2] == 'D') {
                                /* Left arrow: seek -5 */
                                current_frame -= 5;
                                if (current_frame < 0)
                                    current_frame = 0;
                                apc_seek(current_frame);
                                i += 2;
                                continue;
                            } else if (buf[i + 2] == 'C') {
                                /* Right arrow: seek +5 */
                                current_frame += 5;
                                if (current_frame >= total_frames)
                                    current_frame = total_frames - 1;
                                apc_seek(current_frame);
                                i += 2;
                                continue;
                            }
                        }
                    }
                    switch (ch) {
                    case ' ':
                        if (playing) {
                            apc_pause();
                            playing = false;
                        } else {
                            apc_play(speed, loop);
                            playing = true;
                        }
                        break;
                    case 'q':
                    case '\x1b':
                        running = false;
                        break;
                    case '+':
                    case '=':
                        speed = speed < 8.0 ? speed * 1.5 : speed;
                        if (playing)
                            apc_play(speed, loop);
                        break;
                    case '-':
                    case '_':
                        speed = speed > 0.1 ? speed / 1.5 : speed;
                        if (playing)
                            apc_play(speed, loop);
                        break;
                    case 'r':
                        apc_stop();
                        apc_play(speed, loop);
                        playing = true;
                        current_frame = 0;
                        break;
                    case 'L':
                        loop = !loop;
                        if (playing)
                            apc_play(speed, loop);
                        break;
                    case 'b':
                        bg_layer = !bg_layer;
                        layer_str = bg_layer ? "background" : "foreground";
                        apc_place(place_row, place_col, place_rows,
                                  place_cols, layer_str, opacity);
                        break;
                    case '[':
                        opacity -= 0.1;
                        if (opacity < 0.1)
                            opacity = 0.1;
                        apc_place(place_row, place_col, place_rows,
                                  place_cols, layer_str, opacity);
                        break;
                    case ']':
                        opacity += 0.1;
                        if (opacity > 1.0)
                            opacity = 1.0;
                        apc_place(place_row, place_col, place_rows,
                                  place_cols, layer_str, opacity);
                        break;
                    default:
                        break;
                    }
                }
                /* Redraw info bar after any key event */
                redraw_info_bar(current_frame, total_frames, meta.fr,
                                speed, playing, loop, bg_layer, opacity,
                                term_rows, term_cols);
            }
        }

        /* Update local frame counter (approximate) */
        if (playing) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - last_time.tv_sec) +
                             (now.tv_nsec - last_time.tv_nsec) / 1e9;
            if (elapsed > 0.1) {
                current_frame += (int)(elapsed * meta.fr * speed);
                if (loop)
                    current_frame %= total_frames;
                else if (current_frame >= total_frames) {
                    current_frame = total_frames - 1;
                    playing = false;
                }
                last_time = now;
            }
        } else {
            clock_gettime(CLOCK_MONOTONIC, &last_time);
        }
    }

    /* Cleanup */
    apc_delete();
    show_cursor();
    clear_screen();
    exit_raw_mode();
    exit_alt_screen();

    free(json);
    return 0;
}
