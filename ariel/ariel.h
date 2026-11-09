// ariel: terminal emulation (xterm-256color class) in C11, no allocation
//
// ariel parses a child program's output into screens and history, answers
// its queries, and encodes keys, mouse and pastes for it. It allocates
// nothing: the caller hands it one block of memory per terminal, sized by
// ariel_size. It does no I/O, keeps no global state and calls no
// callbacks, so it needs nothing from libc but memcpy, memmove, memset
// and memcmp, which compilers may emit.
//
// The loop on child output:
//
//     for (ariel_Str in = bytes;;) {
//         ariel_Fed f = ariel_feed(t, in);
//         if (f.ev.kind == ARIEL_EV_NONE) break;
//         handle(f.ev);             // e.g. REPLY: write f.ev.data to the child
//         in = f.rest;
//     }
//
// One thread at a time per terminal. Strings passed in are copied when
// kept; strings returned are borrowed, valid until the next call into the
// same terminal unless noted otherwise.
//
// This is free and unencumbered software released into the public domain.
#ifndef ARIEL_H
#define ARIEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARIEL_VERSION "0.1.0"

typedef struct ariel_Term ariel_Term;   // the start of the caller's block
typedef struct { uint8_t *data; ptrdiff_t len; } ariel_Str;


// Setup

typedef struct {                // zero fields take the defaults shown
    int32_t   cols, rows;       // size (80 x 24)
    int32_t   capcols, caprows; // grid capacity (the size), at most 4096
    int32_t   histlines;        // history lines (10000), negative for none
    ptrdiff_t histbytes;        // history ring bytes (4 MiB)
    uint32_t  flags;            // ARIEL_F_*
    ariel_Str name;             // XTVERSION text, copied ("ariel(0.1.0)")
    ariel_Str delims;           // word-selection delimiters, copied
} ariel_Config;

enum {
    ARIEL_F_WIDEAMBIG   = 1<<0, // East Asian Ambiguous is two columns
    ARIEL_F_LEGACYWIDTH = 1<<1, // start with mode 2027 reset
    ARIEL_F_BOLDBRIGHT  = 1<<2, // bold on palette 0-7 draws 8-15
    ARIEL_F_CLIPREAD    = 1<<3, // pass OSC 52 reads to the host
    ARIEL_F_NOREPLY     = 1<<4, // never ARIEL_EV_REPLY
};

// Block size for a configuration, 0 if invalid. 16-byte alignment.
ptrdiff_t   ariel_size(ariel_Config *);

// A new terminal in mem, 0 if len is too small. The block needn't be
// zeroed, and only the parts in use are touched.
ariel_Term *ariel_init(void *mem, ptrdiff_t len, ariel_Config *);

// Move a terminal into a new block laid out for another capacity or
// history size (cols and rows are ignored), 0 if len is too small. The
// old block may be reused once this returns.
ariel_Term *ariel_move(ariel_Term *, void *mem, ptrdiff_t len, ariel_Config *);


// Child output

enum {
    ARIEL_EV_NONE,      // the input is used up and nothing is pending
    ARIEL_EV_REPLY,     // data: bytes for the child, in query order
    ARIEL_EV_BELL,
    ARIEL_EV_TITLE,     // arg 0 window, 1 icon; data UTF-8
    ARIEL_EV_CWD,       // data: OSC 7 URI, or an OSC 9;9 path
    ARIEL_EV_CLIPSET,   // OSC 52 write: arg selections, part, data bytes
    ARIEL_EV_CLIPGET,   // OSC 52 read (ARIEL_F_CLIPREAD): ariel_clipreply
    ARIEL_EV_NOTIFY,    // OSC 9 or 777: data title, more body
    ARIEL_EV_PROGRESS,  // OSC 9;4: arg state 0-4, value percent
    ARIEL_EV_MARK,      // OSC 133: arg 'A'-'D', value exit status or -1
    ARIEL_EV_SYNC,      // mode 2026: arg 1 begun, 0 ended
};
enum { ARIEL_PART_BEGIN = 1, ARIEL_PART_DATA, ARIEL_PART_END, ARIEL_PART_ABORT };

typedef struct {
    int32_t   kind, arg, value, part;
    ariel_Str data, more;
} ariel_Event;

typedef struct {
    ariel_Str   rest;   // input not yet consumed
    ariel_Event ev;
} ariel_Fed;

// Consume child output until it is used up or an event occurs, at any
// split point (mid UTF-8, mid sequence). Call again with rest until the
// event is ARIEL_EV_NONE: queued replies may follow the last byte. Event
// data stays valid until the next ariel_feed. The terminal is consistent
// at every event, so the host may draw or encode before continuing.
ariel_Fed ariel_feed(ariel_Term *, ariel_Str in);


// Rendering, by viewport row: rows from history when scrolled back

enum {
    ARIEL_A_BOLD      = 1<<0,
    ARIEL_A_FAINT     = 1<<1,
    ARIEL_A_ITALIC    = 1<<2,
    ARIEL_A_ULINE     = 7<<3,   // 1 single, 2 double, 3 curly, 4 dotted, 5 dashed
    ARIEL_A_BLINK     = 1<<6,
    ARIEL_A_STRIKE    = 1<<7,
    ARIEL_A_OVERLINE  = 1<<8,
    ARIEL_A_SELECTED  = 1<<9,   // the host paints its selection colors
    ARIEL_A_DEFBG     = 1<<10,  // bg is the default background
    ARIEL_A_HIDDEN    = 1<<11,  // invisible (SGR 8): no glyph
};
#define ARIEL_ULINE(attr) ((attr)>>3 & 7)

typedef struct {
    uint32_t cp;        // base code point, 0 if blank
    uint32_t fg, bg;    // 0xRRGGBB: palette, inverse, DECSCNM applied
    uint32_t ul;        // underline color
    uint32_t link;      // hyperlink handle, 0 if none
    uint16_t attr;      // ARIEL_A_*
    uint8_t  width;     // 1, 2, or 0 on the right half of a wide character
    uint8_t  extra;     // further code points in the cluster
} ariel_Cell;

// Fill up to cap cells of viewport row y, returning the column count.
int32_t ariel_row(ariel_Term *, int32_t y, ariel_Cell *, int32_t cap);

// Every code point of the cluster at viewport cell (x, y), base first,
// up to cap; returns the full count.
ptrdiff_t ariel_cluster(ariel_Term *, int32_t x, int32_t y, uint32_t *, ptrdiff_t cap);

// A hyperlink's URI, empty once it is gone.
ariel_Str ariel_link(ariel_Term *, uint32_t handle);

enum {
    ARIEL_R_WRAPPED = 1<<0,     // continues on the next row
    ARIEL_R_PROMPT  = 1<<1,     // OSC 133 A started here
    ARIEL_R_HISTORY = 1<<8,     // the row comes from history
};
typedef struct {
    int64_t  id;        // stable while the row scrolls
    uint32_t gen;       // changes whenever the row's content does
    uint32_t flags;     // ARIEL_R_*
} ariel_RowInfo;
ariel_RowInfo ariel_rowinfo(ariel_Term *, int32_t y);

// Changes whenever anything visible does.
uint32_t ariel_gen(ariel_Term *);

enum { ARIEL_CUR_BLOCK = 1, ARIEL_CUR_UNDERLINE, ARIEL_CUR_BAR };
typedef struct {
    int32_t  x, y;      // viewport cell, y -1 when out of view
    int32_t  shape;     // ARIEL_CUR_*
    uint32_t color;     // 0xRRGGBB
    uint8_t  visible, blink, wide, pad;
} ariel_Cursor;
ariel_Cursor ariel_cursor(ariel_Term *);

typedef struct {
    int32_t cols, rows;
    int32_t mouse;      // 0, 9, 1000, 1002, 1003
    int32_t mouseenc;   // 0 (X10), 1005, 1006, 1015, 1016
    int32_t kitty;      // current kitty keyboard flags
    uint8_t altscreen, sync, appcursor, appkeypad;
    uint8_t paste, focus, altscroll, reverse;
} ariel_Modes;
ariel_Modes ariel_modes(ariel_Term *);

// Window (0) or icon (1) title.
ariel_Str ariel_title(ariel_Term *, int32_t which);


// Input for the child, encoded per the current modes. Results are
// borrowed until the next encoder call; they never disturb pending events.

enum {
    ARIEL_SHIFT = 1, ARIEL_ALT = 2, ARIEL_CTRL = 4, ARIEL_SUPER = 8,
    ARIEL_HYPER = 16, ARIEL_META = 32, ARIEL_CAPSLOCK = 64, ARIEL_NUMLOCK = 128,
};
enum { ARIEL_PRESS = 1, ARIEL_REPEAT, ARIEL_RELEASE, ARIEL_MOTION };
enum {
    ARIEL_K_ENTER = 1, ARIEL_K_TAB, ARIEL_K_BACKSPACE, ARIEL_K_ESCAPE,
    ARIEL_K_INSERT, ARIEL_K_DELETE, ARIEL_K_HOME, ARIEL_K_END,
    ARIEL_K_PAGEUP, ARIEL_K_PAGEDOWN,
    ARIEL_K_UP, ARIEL_K_DOWN, ARIEL_K_LEFT, ARIEL_K_RIGHT,
    ARIEL_K_F1, ARIEL_K_F35 = ARIEL_K_F1 + 34,
    ARIEL_K_KP0, ARIEL_K_KP9 = ARIEL_K_KP0 + 9,
    ARIEL_K_KPDECIMAL, ARIEL_K_KPDIVIDE, ARIEL_K_KPMULTIPLY,
    ARIEL_K_KPSUBTRACT, ARIEL_K_KPADD, ARIEL_K_KPENTER, ARIEL_K_KPEQUAL,
    ARIEL_K_MENU,
    ARIEL_K_LSHIFT, ARIEL_K_LCTRL, ARIEL_K_LALT, ARIEL_K_LSUPER,
    ARIEL_K_RSHIFT, ARIEL_K_RCTRL, ARIEL_K_RALT, ARIEL_K_RSUPER,
};
typedef struct {
    int32_t   key;      // ARIEL_K_*, or 0 for a text key
    int32_t   mods;     // ARIEL_SHIFT...
    int32_t   action;   // ARIEL_PRESS (0 means press), _REPEAT, _RELEASE
    uint32_t  cp;       // a text key's unshifted code point: 'a' for Ctrl+Shift+A
    uint32_t  shifted;  // its shifted code point, 0 if none
    uint32_t  base;     // its code point on a US layout, 0 if the same
    ariel_Str text;     // text the key produced, possibly empty
} ariel_Key;
ariel_Str ariel_key(ariel_Term *, ariel_Key *);       // may be empty
ariel_Str ariel_text(ariel_Term *, ariel_Str);        // IME or composed text

typedef struct {
    int32_t action;     // ARIEL_PRESS, _RELEASE, _MOTION
    int32_t button;     // 0 none, 1-3, 4-7 wheel up/down/left/right, 8-11
    int32_t mods;       // ARIEL_SHIFT, _ALT, _CTRL
    int32_t x, y;       // viewport cell
    int32_t px, py;     // pixel, for mode 1016
} ariel_Mouse;
ariel_Str ariel_mouse(ariel_Term *, ariel_Mouse *);   // empty: not reported
ariel_Str ariel_focus(ariel_Term *, int32_t in);      // empty unless 1004

// A paste, in pieces: BEGIN on the first, END on the last (both for one
// piece). Sanitizes it and brackets it under mode 2004. cap >= in.len+6
// per marker. Returns the length written.
enum { ARIEL_PASTE_BEGIN = 1, ARIEL_PASTE_END = 2 };
ptrdiff_t ariel_paste(ariel_Term *, uint8_t *out, ptrdiff_t cap, ariel_Str in, int32_t flags);
int32_t   ariel_pastecheck(ariel_Term *, ariel_Str);  // nonzero: confirm first

ariel_Str ariel_clipreply(ariel_Term *, int32_t sel, ariel_Str data);
ariel_Str ariel_scheme(ariel_Term *, int32_t dark);   // empty unless 2031


// Viewport, selection, copy

enum { ARIEL_TO_BOTTOM, ARIEL_TO_TOP, ARIEL_TO_PREVMARK, ARIEL_TO_NEXTMARK };
void ariel_scroll(ariel_Term *, int32_t rows);        // negative: older
void ariel_scrollto(ariel_Term *, int32_t where);
typedef struct { int32_t offset, total; } ariel_View; // rows above the bottom, of total
ariel_View ariel_view(ariel_Term *);

enum {
    ARIEL_SEL_CHAR = 1, ARIEL_SEL_WORD, ARIEL_SEL_LINE, ARIEL_SEL_EXTEND,
    ARIEL_SEL_ALL, ARIEL_SEL_CLEAR,
    ARIEL_SEL_BLOCK = 1<<8,     // with CHAR or EXTEND: a rectangle
};
// x is a column boundary, 0 to cols, y a viewport row.
void      ariel_select(ariel_Term *, int32_t op, int32_t x, int32_t y);
ptrdiff_t ariel_copy(ariel_Term *, uint8_t *out, ptrdiff_t cap); // length needed


// Control

// Resize within capacity, reflowing the main screen. cellw and cellh are
// the cell size in pixels, for size reports. Returns 0, changing nothing,
// beyond capacity.
int32_t ariel_resize(ariel_Term *, int32_t cols, int32_t rows, int32_t cellw, int32_t cellh);

enum { ARIEL_C_FG = 256, ARIEL_C_BG, ARIEL_C_CURSOR, ARIEL_C_COUNT };
void     ariel_setcolor(ariel_Term *, int32_t index, uint32_t rgb);  // a default
uint32_t ariel_color(ariel_Term *, int32_t index);  // effective, OSC overrides too
void     ariel_setflags(ariel_Term *, uint32_t);    // WIDEAMBIG, BOLDBRIGHT, CLIPREAD

enum { ARIEL_RESET_FULL = 1, ARIEL_RESET_HISTORY = 2, ARIEL_RESET_SCREEN = 4 };
void ariel_reset(ariel_Term *, int32_t what);

// The state as a VT stream that rebuilds it when fed to a fresh terminal
// of the same size, with up to histlines of history. Returns the length
// needed. The replay prints history through the screen, where a cluster
// with more than one combining mark takes a slot of a pool sized by
// capacity: give the fresh terminal capcols*caprows of at least
// 4*cols*(2*rows+1) or such clusters may lose marks. Rows a shrinking
// resize lent to history come back as ordinary history.
ptrdiff_t ariel_export(ariel_Term *, int32_t histlines, uint8_t *out, ptrdiff_t cap);

// A diagnostic text dump of the screen (flags 0) or more. Returns the
// length needed.
enum { ARIEL_DUMP_ATTRS = 1, ARIEL_DUMP_HISTORY = 2, ARIEL_DUMP_STATE = 4 };
ptrdiff_t ariel_dump(ariel_Term *, int32_t flags, uint8_t *out, ptrdiff_t cap);

#ifdef __cplusplus
}
#endif
#endif
