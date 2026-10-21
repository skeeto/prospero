// Color themes: the GUI's dark and light palettes, and the choice
// between them
//
// A palette holds every color the GUI draws itself: the transcript's
// text, links, code spans and code blocks with their highlight classes
// (diffs among them), message backgrounds, the selection, the colors of
// states in tool rows, ask cards, the liveness strip and the sidebar,
// and the metrics overlay's alarm. It also holds the few of ImGui's own
// colors that the stock styles get wrong here, which ui.cpp lays over
// ImGui's dark or light style: the panels' background and text, dimmed
// text (hints, disabled labels) that still reads on a field, rows
// selected, hovered and pressed, buttons, and progress bars. Selected
// text has a color of its own, since no one color of the transcript's
// reads on every background, the selection's among them. Colors are packed as ImGui packs them (IM_COL32,
// red in the low byte, alpha in the high), so this file needs no ImGui,
// and the unit tests check that every text color reads against the
// backgrounds it is drawn on, by the WCAG's measure: a contrast ratio of
// at least 4.5 (AA for normal text).
//
// The setting, [ui] theme, is dark, light, or system, the default, which
// follows the operating system's preference as the platform reports it,
// at startup and whenever it changes (gui_systheme).
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    THEME_SYSTEM,
    THEME_DARK,
    THEME_LIGHT,
    THEME_COUNT,
};

static Str themenames[] = {"system", "dark", "light"};
static_assert(countof(themenames) == THEME_COUNT);

struct Palette {
    b32 dark;
    // ImGui's, over its stock style, and the window's clear color
    u32 clear;      // between docked windows
    u32 panel;      // window background
    u32 paneltext;
    u32 paneldim;   // disabled text and hints
    u32 frame;      // fields, opaque
    u32 selrow;     // a selected row (the sidebar's session), opaque
    u32 rowhover;   // a row under the mouse, and pressed, opaque
    u32 rowpress;
    u32 btn;        // buttons: at rest, under the mouse, pressed
    u32 btnhover;
    u32 btnpress;
    u32 bar;        // progress bars, under paneltext
    // The transcript, on panel
    u32 text;
    u32 dim;        // headers and secondary text
    u32 reasoning;  // reasoning and notes (STYLE_DIM)
    u32 link;
    u32 code;       // code spans
    u32 codetext;   // plain text in code blocks, on codebg
    u32 codebg;
    u32 headerbg;   // table header rows
    u32 quote;      // a quote's bar
    u32 rule;       // rules and table grids
    u32 marker;     // list bullets and numbers
    u32 sel;        // selected text's background
    u32 seltext;    // selected text, whatever its color otherwise
    u32 userbg;     // the user's messages
    u32 toolbg;     // tool rows
    u32 askbg;      // ask cards
    u32 summary;    // a tool row's arguments, an ask's path
    u32 hl[HL_COUNT];  // code highlight classes on codebg (HL_PLAIN unused)
    // States, as text on panel, toolbg, askbg and selrow
    u32 ok;
    u32 bad;
    u32 amber;      // waiting, asking, stalled
    u32 active;     // running, streaming
    u32 stale;      // the metrics overlay's alarm
};

static constexpr u32 rgb(i32 r, i32 g, i32 b)
{
    return 0xff000000u | (u32)b<<16 | (u32)g<<8 | (u32)r;
}

static Palette themedark = {
    .dark      = 1,
    .clear     = rgb( 20,  20,  23),
    .panel     = rgb( 15,  15,  16),
    .paneltext = rgb(255, 255, 255),
    .paneldim  = rgb(153, 153, 153),
    .frame     = rgb( 30,  47,  74),
    .selrow    = rgb( 32,  58,  89),
    .rowhover  = rgb( 28,  46,  70),
    .rowpress  = rgb( 36,  54,  80),
    .btn       = rgb( 36,  69, 110),
    .btnhover  = rgb( 46,  88, 140),
    .btnpress  = rgb( 30,  62, 104),
    .bar       = rgb( 52,  96, 170),
    .text      = rgb(222, 222, 222),
    .dim       = rgb(160, 170, 190),
    .reasoning = rgb(150, 150, 150),
    .link      = rgb(100, 160, 240),
    .code      = rgb(225, 190, 140),
    .codetext  = rgb(212, 212, 212),
    .codebg    = rgb( 30,  30,  34),
    .headerbg  = rgb( 45,  45,  52),
    .quote     = rgb( 90,  90, 100),
    .rule      = rgb( 80,  80,  88),
    .marker    = rgb(160, 160, 170),
    .sel       = rgb( 38,  79, 120),
    .seltext   = rgb(255, 255, 255),
    .userbg    = rgb( 40,  44,  52),
    .toolbg    = rgb( 34,  36,  40),
    .askbg     = rgb( 48,  42,  30),
    .summary   = rgb(200, 200, 205),
    .hl = {
        0,
        rgb(118, 160, 115),  // comment
        rgb(198, 140, 228),  // keyword
        rgb( 86, 182, 194),  // type
        rgb(214, 157, 133),  // string
        rgb(181, 206, 168),  // number
        rgb(197, 134, 192),  // preprocessor
        rgb(156, 220, 254),  // variable
        rgb(220, 220, 170),  // function
        rgb(120, 200, 120),  // diff inserted
        rgb(230, 110, 110),  // diff deleted
        rgb(150, 170, 230),  // diff header
    },
    .ok        = rgb(110, 200, 120),
    .bad       = rgb(250, 132, 132),
    .amber     = rgb(235, 180,  80),
    .active    = rgb(110, 170, 240),
    .stale     = rgb(255, 102,  77),
};

static Palette themelight = {
    .dark      = 0,
    .clear     = rgb(214, 214, 218),
    .panel     = rgb(240, 240, 240),
    .paneltext = rgb(  0,   0,   0),
    .paneldim  = rgb( 98,  98,  98),
    .frame     = rgb(255, 255, 255),
    .selrow    = rgb(186, 212, 243),
    .rowhover  = rgb(206, 224, 246),
    .rowpress  = rgb(190, 212, 240),
    .btn       = rgb(176, 206, 244),
    .btnhover  = rgb(150, 190, 244),
    .btnpress  = rgb(124, 172, 236),
    .bar       = rgb(150, 190, 240),
    .text      = rgb( 32,  33,  36),
    .dim       = rgb( 80,  88, 100),
    .reasoning = rgb( 92,  92,  96),
    .link      = rgb(  0,  88, 190),
    .code      = rgb(150,  60,   0),
    .codetext  = rgb( 36,  41,  47),
    .codebg    = rgb(228, 230, 235),
    .headerbg  = rgb(222, 225, 231),
    .quote     = rgb(176, 181, 190),
    .rule      = rgb(196, 199, 206),
    .marker    = rgb( 90,  94, 104),
    .sel       = rgb(180, 213, 255),
    .seltext   = rgb(  0,   0,   0),
    .userbg    = rgb(221, 229, 241),
    .toolbg    = rgb(230, 231, 234),
    .askbg     = rgb(252, 243, 220),
    .summary   = rgb( 52,  56,  64),
    .hl = {
        0,
        rgb( 89,  99, 110),  // comment
        rgb(176,  18,  40),  // keyword
        rgb(146,  54,   0),  // type
        rgb( 10,  48, 105),  // string
        rgb(  5,  80, 174),  // number
        rgb(110,  60, 190),  // preprocessor
        rgb(  0,  92, 130),  // variable
        rgb(102,  57, 186),  // function
        rgb( 17,  99,  41),  // diff inserted
        rgb(130,   7,  30),  // diff deleted
        rgb(  5,  80, 174),  // diff header
    },
    .ok        = rgb( 16, 100,  42),
    .bad       = rgb(168,  24,  34),
    .amber     = rgb(125,  78,   0),
    .active    = rgb(  0,  80, 176),
    .stale     = rgb(180,  40,  20),
};

// The setting's value from its name, or -1 if it names none.
static i32 themeparse(Str s)
{
    for (i32 i = 0; i < THEME_COUNT; i++) {
        if (themenames[i] == s) {
            return i;
        }
    }
    return -1;
}

// The palette in effect for a setting, given the system's preference.
static Palette *themepalette(i32 setting, b32 sysdark)
{
    switch (setting) {
    case THEME_DARK:  return &themedark;
    case THEME_LIGHT: return &themelight;
    }
    return sysdark ? &themedark : &themelight;
}
