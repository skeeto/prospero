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
// at startup and whenever it changes (gui_systheme). It may instead name
// a [theme NAME] section, a theme of the configuration's: a base, dark
// or light, whose palette it starts from, and colors laid over it by
// key (themekeys), as #rgb or #rrggbb. A button or toggle given a color
// of its own whose hover and pressed shades are left unset gets shades
// that differ from it as the base's differ from its own (themebuild), as
// do a field and a scrollbar. A tool's output, left unset, follows code.
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
    u32 frame;      // fields, opaque: at rest, under the mouse, active
    u32 framehover;
    u32 frameactive;
    u32 selrow;     // a selected row (the sidebar's session), opaque
    u32 rowhover;   // a row under the mouse, and pressed, opaque
    u32 rowpress;
    u32 btn;        // buttons: at rest, under the mouse, pressed
    u32 btnhover;
    u32 btnpress;
    u32 btnon;      // a toggle that is on, and it under the mouse: a toggle
    u32 btnonhover; // that is off has no fill, so on stands out from it
    u32 bar;        // progress bars, under paneltext
    u32 tabbar;     // behind panes' tabs (and floating windows' titles)
    u32 tabbaron;   // that of the pane with focus
    u32 tab;        // a pane's tab, and the one selected, under paneltext
    u32 tabsel;
    u32 accent;     // check marks, sliders, the focused tab's line, a
                    // splitter under the mouse, docking's preview
    u32 border;     // borders and separators, opaque on panel
    u32 scroll;     // scrollbars' grab: at rest, under the mouse, pressed
    u32 scrollhover;
    u32 scrollpress;
    u32 scrolltrack; // behind the grab
    u32 menubar;    // the main menu bar, under paneltext
    // The transcript, on panel
    u32 text;
    u32 dim;        // headers and secondary text
    u32 reasoning;  // reasoning and notes (STYLE_DIM)
    u32 link;
    u32 code;       // Markdown's code spans
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
    u32 tooltext;   // a tool's output as plain text, on toolbg
    u32 hl[HL_COUNT];  // code highlight classes on codebg (HL_PLAIN unused)
    // States, as text on panel, toolbg, askbg and selrow
    u32 ok;
    u32 bad;
    u32 amber;      // waiting, asking, stalled
    u32 active;     // running, streaming
    u32 stale;      // the metrics overlay's alarm
    // The terminal's (term.cpp): its 16 colors, text, background, cursor
    u32 ansi[16];
    u32 termfg;
    u32 termbg;
    u32 termcursor;
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
    .framehover  = rgb( 48,  64,  79),
    .frameactive = rgb( 54,  84, 120),
    .selrow    = rgb( 32,  58,  89),
    .rowhover  = rgb( 28,  46,  70),
    .rowpress  = rgb( 36,  54,  80),
    .btn       = rgb( 36,  69, 110),
    .btnhover  = rgb( 46,  88, 140),
    .btnpress  = rgb( 30,  62, 104),
    .btnon     = rgb( 24,  98, 184),
    .btnonhover= rgb( 36, 112, 204),
    .bar       = rgb( 52,  96, 170),
    .tabbar    = rgb( 10,  10,  10),
    .tabbaron  = rgb( 41,  74, 122),
    .tab       = rgb( 45,  87, 144),
    .tabsel    = rgb( 51, 105, 173),
    .accent    = rgb( 66, 150, 250),
    .border    = rgb( 62,  62,  72),
    .scroll    = rgb( 79,  79,  79),
    .scrollhover = rgb(105, 105, 105),
    .scrollpress = rgb(130, 130, 130),
    .scrolltrack = rgb( 10,  10,  10),
    .menubar   = rgb( 36,  36,  36),
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
    .tooltext  = rgb(225, 190, 140),
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
    .ansi = {
        rgb( 40,  42,  46), rgb(205,  49,  49), rgb( 13, 188, 121), rgb(229, 229,  16),
        rgb( 36, 114, 200), rgb(188,  63, 188), rgb( 17, 168, 205), rgb(204, 204, 204),
        rgb(102, 102, 102), rgb(241,  76,  76), rgb( 35, 209, 139), rgb(245, 245,  67),
        rgb( 59, 142, 234), rgb(214, 112, 214), rgb( 41, 184, 219), rgb(229, 229, 229),
    },
    .termfg     = rgb(222, 222, 222),
    .termbg     = rgb( 15,  15,  16),
    .termcursor = rgb(230, 230, 230),
};

static Palette themelight = {
    .dark      = 0,
    .clear     = rgb(214, 214, 218),
    .panel     = rgb(240, 240, 240),
    .paneltext = rgb(  0,   0,   0),
    .paneldim  = rgb( 98,  98,  98),
    .frame     = rgb(255, 255, 255),
    .framehover  = rgb(171, 204, 244),
    .frameactive = rgb(124, 180, 247),
    .selrow    = rgb(186, 212, 243),
    .rowhover  = rgb(206, 224, 246),
    .rowpress  = rgb(190, 212, 240),
    .btn       = rgb(176, 206, 244),
    .btnhover  = rgb(150, 190, 244),
    .btnpress  = rgb(124, 172, 236),
    .btnon     = rgb(100, 160, 240),
    .btnonhover= rgb( 84, 144, 228),
    .bar       = rgb(150, 190, 240),
    .tabbar    = rgb(209, 209, 209),
    .tabbaron  = rgb(255, 255, 255),
    .tab       = rgb(237, 245, 255),
    .tabsel    = rgb(180, 213, 253),
    .accent    = rgb( 66, 150, 250),
    .border    = rgb(153, 153, 153),
    .scroll    = rgb(189, 189, 189),
    .scrollhover = rgb(148, 148, 148),
    .scrollpress = rgb(125, 125, 125),
    .scrolltrack = rgb(245, 245, 245),
    .menubar   = rgb(219, 219, 219),
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
    .tooltext  = rgb(150,  60,   0),
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
    .ansi = {
        rgb(  0,   0,   0), rgb(205,  49,  49), rgb(  0, 138,   0), rgb(128, 118,   0),
        rgb(  4,  81, 165), rgb(188,   5, 188), rgb(  5, 132, 168), rgb( 85,  85,  85),
        rgb(102, 102, 102), rgb(205,  49,  49), rgb( 20, 160,  20), rgb(150, 140,   0),
        rgb(  4,  81, 165), rgb(188,   5, 188), rgb(  5, 152, 188), rgb(165, 165, 165),
    },
    .termfg     = rgb( 32,  33,  36),
    .termbg     = rgb(255, 255, 255),
    .termcursor = rgb( 32,  33,  36),
};

// A [theme NAME] key and the palette color it sets, in config.cpp's
// order there (theme_test checks the two lists agree).
struct ThemeKey {
    Str key;
    iz  off;  // into Palette, of a u32
};

#define TK(k, f)     {k, __builtin_offsetof(Palette, f)}
#define TKA(k, f, i) {k, __builtin_offsetof(Palette, f) + (i)*(iz)sizeof(u32)}
static ThemeKey themekeys[] = {
    TK("gap", clear),
    TK("panel", panel),
    TK("panel_text", paneltext),
    TK("panel_dim", paneldim),
    TK("field", frame),
    TK("field_hover", framehover),
    TK("field_active", frameactive),
    TK("row_selected", selrow),
    TK("row_hover", rowhover),
    TK("row_pressed", rowpress),
    TK("button", btn),
    TK("button_hover", btnhover),
    TK("button_pressed", btnpress),
    TK("toggle_on", btnon),
    TK("toggle_on_hover", btnonhover),
    TK("progress", bar),
    TK("tab_bar", tabbar),
    TK("tab_bar_focused", tabbaron),
    TK("tab", tab),
    TK("tab_selected", tabsel),
    TK("accent", accent),
    TK("border", border),
    TK("scrollbar", scroll),
    TK("scrollbar_hover", scrollhover),
    TK("scrollbar_pressed", scrollpress),
    TK("scrollbar_track", scrolltrack),
    TK("menu_bar", menubar),
    TK("text", text),
    TK("text_dim", dim),
    TK("reasoning", reasoning),
    TK("link", link),
    TK("code", code),
    TK("code_block", codebg),
    TK("code_block_text", codetext),
    TK("table_header", headerbg),
    TK("quote", quote),
    TK("rule", rule),
    TK("list_marker", marker),
    TK("selection", sel),
    TK("selection_text", seltext),
    TK("user_message", userbg),
    TK("tool_row", toolbg),
    TK("ask_card", askbg),
    TK("tool_summary", summary),
    TK("tool_output", tooltext),
    TKA("syntax_comment", hl, 1),
    TKA("syntax_keyword", hl, 2),
    TKA("syntax_type", hl, 3),
    TKA("syntax_string", hl, 4),
    TKA("syntax_number", hl, 5),
    TKA("syntax_preprocessor", hl, 6),
    TKA("syntax_variable", hl, 7),
    TKA("syntax_function", hl, 8),
    TKA("diff_inserted", hl, 9),
    TKA("diff_deleted", hl, 10),
    TKA("diff_header", hl, 11),
    TK("ok", ok),
    TK("error", bad),
    TK("waiting", amber),
    TK("running", active),
    TK("alarm", stale),
    TKA("term_black", ansi, 0),
    TKA("term_red", ansi, 1),
    TKA("term_green", ansi, 2),
    TKA("term_yellow", ansi, 3),
    TKA("term_blue", ansi, 4),
    TKA("term_magenta", ansi, 5),
    TKA("term_cyan", ansi, 6),
    TKA("term_white", ansi, 7),
    TKA("term_bright_black", ansi, 8),
    TKA("term_bright_red", ansi, 9),
    TKA("term_bright_green", ansi, 10),
    TKA("term_bright_yellow", ansi, 11),
    TKA("term_bright_blue", ansi, 12),
    TKA("term_bright_magenta", ansi, 13),
    TKA("term_bright_cyan", ansi, 14),
    TKA("term_bright_white", ansi, 15),
    TK("term_text", termfg),
    TK("term_background", termbg),
    TK("term_cursor", termcursor),
};
#undef TK
#undef TKA

static u32 *themecolor(Palette *p, iz off)
{
    return (u32 *)((u8 *)p + off);
}

// Set a palette's color by its [theme NAME] key. Returns whether the key
// names one and the value is a color: else the palette is unchanged.
static b32 themeput(Palette *p, Str key, Str value)
{
    CfgColor c = cfgcolor(value);
    for (iz i = 0; c.ok && i < countof(themekeys); i++) {
        if (themekeys[i].key == key) {
            *themecolor(p, themekeys[i].off) = rgb(c.r, c.g, c.b);
            return 1;
        }
    }
    return 0;
}

// A color moved as far, channel by channel, as from base to to.
static u32 themeshift(u32 c, u32 from, u32 to)
{
    u32 r = 0xff000000u;
    for (i32 s = 0; s < 24; s += 8) {
        i32 x = (i32)(c>>s & 255) + (i32)(to>>s & 255) - (i32)(from>>s & 255);
        x = x<0 ? 0 : x>255 ? 255 : x;
        r |= (u32)x << s;
    }
    return r;
}

// A theme of the configuration's: its base (dark unless light), then
// the colors get(key) gives, each key of themekeys asked, then the
// shades of a button or toggle colored but not shaded (see the top).
template<typename F>
static Palette themebuild(Str base, F get)
{
    Palette const *b = base=="light" ? &themelight : &themedark;
    Palette        p = *b;
    b32            tooltext = 0;
    for (iz i = 0; i < countof(themekeys); i++) {
        b32 set = themeput(&p, themekeys[i].key, get(themekeys[i].key));
        tooltext |= set && themekeys[i].key=="tool_output";
    }
    // A tool's output was drawn in code's color before it had its own,
    // so a theme that sets only code keeps its look
    p.tooltext = tooltext ? p.tooltext : p.code==b->code ? b->tooltext : p.code;
    // Each color with its shades, as offsets into Palette
    static constexpr iz shades[][3] = {
        #define S(f) __builtin_offsetof(Palette, f)
        {S(btn),    S(btnhover),    S(btnpress)},
        {S(btnon),  S(btnonhover),  -1},
        {S(frame),  S(framehover),  S(frameactive)},
        {S(scroll), S(scrollhover), S(scrollpress)},
        #undef S
    };
    for (iz i = 0; i < countof(shades); i++) {
        u32 c = *themecolor(&p, shades[i][0]);
        u32 o = *themecolor((Palette *)b, shades[i][0]);
        for (iz k = 1; c!=o && k<3 && shades[i][k]>=0; k++) {
            u32 *s  = themecolor(&p, shades[i][k]);
            u32  bs = *themecolor((Palette *)b, shades[i][k]);
            *s = *s==bs ? themeshift(c, o, bs) : *s;
        }
    }
    return p;
}

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
