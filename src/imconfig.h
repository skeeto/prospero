// Dear ImGui compile-time options (IMGUI_USER_CONFIG)
//
// Every translation unit that includes imgui.h sees these, the library
// and the harness alike, so they agree on the data layouts.
//
// This is free and unencumbered software released into the public domain.
#pragma once

#define IMGUI_USE_WCHAR32  // code points past U+FFFF, fixed from day one
#define IMGUI_ENABLE_FREETYPE
#define IMGUI_DISABLE_STB_TRUETYPE
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_DEMO_WINDOWS  // imgui_demo.cpp: empty stubs only

// 32-bit indices: a terminal's grid passes 64K vertices (300x80 cells),
// and the GLES 3.0 renderer has no VtxOffset to split draw lists by.
#define ImDrawIdx unsigned int

// Places the IME's composition and candidate windows at the text cursor.
// ImGui leaves this off for compilers other than MSVC; imm32 is linked.
#define IMGUI_ENABLE_WIN32_DEFAULT_IME_FUNCTIONS

// A failed assertion goes to prospero_imassert (app.cpp), which logs it.
// Debug builds then trap. Release builds continue: the engine shares the
// process, so one GUI mistake must not end every session.
extern "C" void prospero_imassert(char const *expr, char const *file, int line);
#define IM_ASSERT(e) ((e) ? (void)0 : prospero_imassert(#e, __FILE__, __LINE__))

// Accessibility (a11y_imgui.cpp). With the test engine's hooks compiled
// in, ImGui reports each item as it adds it, but only while its context
// has TestEngineHookItems set: otherwise each costs one branch.
#define IMGUI_ENABLE_TEST_ENGINE

// The patched copy of imgui_widgets.cpp (FetchGui.cmake, imgui_a11y.h)
// asks for the edits an assistive tool queued for the active text
// field, and tells where each text field drew its text. Both only while
// TestEngineHookItems is set.

// An edit to a text field: bytes [lo, hi) of its text become the given
// text, then the selection runs from anchor to caret (bytes, in the
// text after). With lo < 0 it only sets the selection.
struct ProsperoAxEdit {
    int         lo, hi;
    char const *text;
    int         len;
    int         anchor, caret;
};
// The next edit for field id, given its text now; 0 when none is left.
extern "C" int prospero_axedit(unsigned id, char const *text, int len, ProsperoAxEdit *);

// A text field as drawn: its text and where each line went, in the
// window's coordinates. Offsets are bytes of text.
struct ProsperoAxField {
    unsigned     id;
    int          flags;         // its ImGuiInputTextFlags
    int          active;        // being edited, with a caret
    int          toolong;       // active, but too long to describe: no text
    char const  *text;          // null for a password field
    int          len;
    char const  *hint;          // its placeholder, shown when it is empty
    float        x, y;          // the first line's top left, scrolled
    float        clip[4];       // what shows of it: x0, y0, x1, y1
    float        lineh;
    int const   *lines;         // where each line starts, when active
    int          nlines;
    int          vis0, vis1;    // the lines that show, [vis0, vis1)
    float const *xs;            // for each line that shows, in order, the
    int          nxs;           // x of each byte from its start through
                                // its end (one more than its length)
    int          anchor, caret; // the selection, when active
    int          caretline;     // the line ImGui draws the caret on, which
                                // at a wrap is either line's, else -1
};
extern "C" void prospero_axfield(ProsperoAxField const *);

// A combo box as BeginCombo added it, which gives no item info of its
// own: its label, the value it shows (null for none), and whether its
// popup is open.
extern "C" void prospero_axcombo(unsigned id, char const *label, char const *preview, int open);

// An input method's composition (ime.cpp), drawn over the active field
// where it will go, its line's top left at x, y; the field's caret is
// hidden meanwhile, and the IME's position is the clause's, imex, imey.
// The InputTextEx patch asks only while prospero_preediting.
struct ProsperoPreedit {
    unsigned id;
    float    x, y;
    float    clip[4];
    int      hint;        // a placeholder shows, to be covered
    float    imex, imey;  // out
};
extern "C" int prospero_preediting(void);
extern "C" int prospero_preedit(ProsperoPreedit *);
