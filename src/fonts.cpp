// The GUI's embedded fonts, and which characters they have
//
// Prose is Roboto Medium, code JetBrains Mono. Roboto lacks most of the
// symbols models write in prose (arrows, check marks, box drawing), so
// each Roboto face falls back on JetBrains Mono, and both on small
// subsets of Noto Sans Symbols 2 and Noto Sans Math for what neither has
// (heavy check marks, ballot boxes, stars, the return arrow), merged
// into one ImFont in that order (app.cpp, loadfonts). assets/FONTS says
// where each comes from; tools/symbols.py makes the subsets.
//
// Buttons' icons are Codicons, merged after them all over its private
// use range alone (U+EA60-F102), so it shadows nothing; ICON_* name the
// ones used, as UTF-8.
//
// Emoji are a subset of Twemoji (Mozilla's COLRv0 build: layers of
// outlines, colored from a palette, which FreeType draws with the
// loader's LoadColor), merged last, so a symbol any font ahead of it has
// (✔ ★ ⚠) stays text, as in a terminal; tools/symbols.py leaves those
// out of it, and the sequences (flags, skin tones, joined families),
// which ImGui, without shaping, cannot form.
//
// fontcovers reads a TrueType file's character map (cmap formats 4 and
// 12), so that the unit tests can check the fonts against the symbols
// models commonly write (fontsymbolsused) without ImGui or FreeType, and
// the emoji they do (fontemojiused).
//
// This is free and unencumbered software released into the public domain.

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static u8 const robotottf[] = {
    #embed "../assets/Roboto-Medium.ttf"
};
static u8 const monottf[] = {
    #embed "../assets/JetBrainsMono-Regular.ttf"
};
static u8 const symbolsttf[] = {
    #embed "../assets/NotoSansSymbols2-subset.ttf"
};
static u8 const mathttf[] = {
    #embed "../assets/NotoSansMath-subset.ttf"
};
static u8 const codiconttf[] = {
    #embed "../assets/codicon.ttf"
};
static u8 const emojittf[] = {
    #embed "../assets/TwemojiMozilla-subset.ttf"
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

// Prose's fallbacks after Roboto, in order: JetBrains Mono, then the
// symbol subsets, which are also code's fallbacks.
static Str fontroboto()  { return {(u8 *)robotottf, countof(robotottf)}; }
static Str fontmono()    { return {(u8 *)monottf, countof(monottf)}; }
static Str fontsymbols() { return {(u8 *)symbolsttf, countof(symbolsttf)}; }
static Str fontmath()    { return {(u8 *)mathttf, countof(mathttf)}; }
static Str fonticons()   { return {(u8 *)codiconttf, countof(codiconttf)}; }
static Str fontemoji()   { return {(u8 *)emojittf, countof(emojittf)}; }

// The icons buttons show, by their Codicons names
#define ICON_ADD        "\xee\xa9\xa0"  // U+EA60
#define ICON_TRASH      "\xee\xaa\x81"  // U+EA81
#define ICON_HISTORY    "\xee\xaa\x82"  // U+EA82
#define ICON_CHECK      "\xee\xaa\xb2"  // U+EAB2
#define ICON_DISCARD    "\xee\xab\xa2"  // U+EAE2
#define ICON_EXTERNAL   "\xee\xac\x94"  // U+EB14, link-external
#define ICON_LIGHTBULB  "\xee\xa9\xa1"  // U+EA61
#define ICON_INFO       "\xee\xa9\xb4"  // U+EA74
#define ICON_CLOSE      "\xee\xa9\xb6"  // U+EA76
#define ICON_FILE       "\xee\xa9\xbb"  // U+EA7B
#define ICON_REPLY      "\xee\xa9\xbd"  // U+EA7D
#define ICON_TERMINAL   "\xee\xaa\x85"  // U+EA85
#define ICON_DISCONNECT "\xee\xab\x90"  // U+EAD0, debug-disconnect
#define ICON_STOP       "\xee\xab\x97"  // U+EAD7, debug-stop
#define ICON_PAUSE      "\xee\xab\x91"  // U+EAD1, debug-pause
#define ICON_CONTINUE   "\xee\xab\x8f"  // U+EACF, debug-continue
#define ICON_FOLDER     "\xee\xab\xb7"  // U+EAF7, folder-opened
#define ICON_PLUG       "\xee\xac\xad"  // U+EB2D
#define ICON_REFRESH    "\xee\xac\xb7"  // U+EB37
#define ICON_TOOLS      "\xee\xad\xad"  // U+EB6D
#define ICON_SHELL      "\xee\xaf\x8a"  // U+EBCA, terminal-bash
#define ICON_COPY       "\xee\xaf\x8c"  // U+EBCC
#define ICON_PLAY       "\xee\xac\xac"  // U+EB2C
#define ICON_SEND       "\xee\xb0\x8f"  // U+EC0F
#define ICON_ATTACH     "\xee\xb0\xb4"  // U+EC34
#define ICON_UP         "\xee\xaa\xa1"  // U+EAA1, arrow-up
#define ICON_SESSION    "\xee\xab\x87"  // U+EAC7, comment-discussion
#define ICON_SORT       "\xee\xad\x95"  // U+EB55, sort-precedence
#define ICON_TREE       "\xee\xae\x86"  // U+EB86, list-tree
#define ICON_ARCHIVE    "\xee\xaa\x98"  // U+EA98
#define ICON_UNARCHIVE  "\xee\xb1\xb6"  // U+EC76
#define ICON_GRIPPER    "\xee\xac\x84"  // U+EB04
#define ICON_NEWFOLDER  "\xee\xaa\x80"  // U+EA80, new-folder

// Their characters, for the unit tests
static Str fonticonsused =
    ICON_ADD ICON_TRASH ICON_HISTORY ICON_CHECK ICON_DISCARD ICON_EXTERNAL ICON_LIGHTBULB ICON_INFO ICON_CLOSE ICON_FILE ICON_REPLY
    ICON_TERMINAL ICON_DISCONNECT ICON_STOP ICON_PAUSE ICON_CONTINUE ICON_FOLDER ICON_PLUG
    ICON_REFRESH ICON_TOOLS ICON_COPY ICON_SEND ICON_ATTACH ICON_UP ICON_SESSION ICON_SORT ICON_TREE
    ICON_ARCHIVE ICON_UNARCHIVE ICON_GRIPPER ICON_NEWFOLDER;

// Symbols models commonly write in prose: arrows, check marks and
// crosses, bullets, math, shapes, ballot boxes, keyboard symbols, box
// drawing, dashes and curly quotes.
static Str fontsymbolsused =
    "→ ← ↑ ↓ ↔ ⇒ ⇐ ⇔ ↵ ↳ ↻ ⟶ ✓ ✗ ✔ ✘ • … ≤ ≥ ≠ ≈ × ± ° ∞ √ ≡ ∈ ∅ "
    "▶ ▸ ▾ ▼ ▲ ◀ ● ○ ■ □ ◆ ★ ☆ ☐ ☑ ☒ ⚠ ⏳ ✎ ✂ "
    "⌘ ⇧ ⌥ ⌃ ⏎ ⌫ ⇥ ─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼ ═ ║ — – ‘ ’ “ ” « » ′ ″";

// Emoji models commonly write, each of which Twemoji's subset must have
static Str fontemojiused =
    "✅ ❌ ❎ ✨ ❗ ❓ ⭐ ⛔ ➕ ➖ 🚀 🎉 🔥 👍 👎 👀 💡 🐛 🔧 🔨 📝 📌 📎 📁 📄 "
    "📊 📈 📦 🔍 🔒 🔑 🔗 💻 🧪 🧹 🧩 🎯 🏁 ⏰ 🟢 🟡 🔴 🟥 🟩 🚧 🛑 🤔 🤖 😀 😅 🙂 "
    "💬 🔄 🗑 💾 🌐 🧠 🦀";

// Code points that draw nothing: variation selectors (U+FE0F after ⚠
// asks for its emoji form, which ImGui, without shaping, cannot pick),
// joiners, invisible operators and tags. Each face draws them as U+FEFF,
// which Roboto and JetBrains Mono both have, empty and zero-width
// (app.cpp, addface), rather than as the "?" of a missing glyph.
static i32 const fontinvisible[][2] = {
    {0x200c, 0x200d}, {0x2060, 0x2064}, {0xfe00, 0xfe0f},
    {0xe0020, 0xe007f}, {0xe0100, 0xe01ef},
};

static u32 fontbe16(Str s, iz at)
{
    return at<0 || at+2>s.len ? 0 : (u32)s[at]<<8 | s[at+1];
}

static u32 fontbe32(Str s, iz at)
{
    return fontbe16(s, at)<<16 | fontbe16(s, at+2);
}

// A face's table directory in a font file: at its start, or in a
// collection (.ttc, "ttcf") the index'th face's. -1 if there is none.
static iz fontface(Str file, i32 index)
{
    if (fontbe32(file, 0) != 0x74746366) {  // "ttcf"
        return index==0 && file.len>=12 ? 0 : -1;
    }
    u32 n = fontbe32(file, 8);
    if (index<0 || (u32)index>=n) {
        return -1;
    }
    u32 dir = fontbe32(file, 12 + 4*(iz)index);
    return dir && (i64)dir+12<=(i64)file.len ? (iz)dir : -1;
}

// How many faces a font file holds: a collection's count, else one.
static i32 fontfaces(Str file)
{
    u32 n = fontbe32(file, 0)==0x74746366 ? fontbe32(file, 8) : 1;
    return n<1024 ? (i32)n : 1024;
}

// A face's table, by its tag, or empty. A table reaching past the file
// is none.
static Str fonttable(Str file, iz dir, u32 tag)
{
    if (dir < 0) {
        return {};
    }
    u32 n = fontbe16(file, dir+4);
    for (u32 i = 0; i < n; i++) {
        iz rec = dir + 12 + 16*(iz)i;
        if (fontbe32(file, rec) != tag) {
            continue;
        }
        i64 off = fontbe32(file, rec+8);
        i64 len = fontbe32(file, rec+12);
        if (rec+16>file.len || off+len>(i64)file.len) {
            return {};
        }
        return {file.data+off, (iz)len};
    }
    return {};
}

// Whether a character map (a face's cmap table) maps the code point to
// a glyph: a Unicode subtable of format 12 if any, else of format 4.
// Bounds are checked, so a damaged table covers nothing rather than
// reading past it.
static b32 cmapcovers(Str cmap, i32 cp)
{
    iz fmt4 = -1, fmt12 = -1;
    u32 subs = fontbe16(cmap, 2);
    for (u32 i = 0; i < subs; i++) {
        iz  rec  = 4 + 8*(iz)i;
        u32 plat = fontbe16(cmap, rec);
        u32 enc  = fontbe16(cmap, rec+2);
        i64 at   = fontbe32(cmap, rec+4);
        b32 uni  = plat==0 || (plat==3 && (enc==1 || enc==10));
        if (!uni || at>=(i64)cmap.len) {
            continue;
        }
        u32 fmt = fontbe16(cmap, (iz)at);
        fmt12 = fmt==12 && fmt12<0 ? (iz)at : fmt12;
        fmt4  = fmt==4  && fmt4<0  ? (iz)at : fmt4;
    }

    if (fmt12 >= 0) {
        u32 groups = fontbe32(cmap, fmt12+12);
        for (u32 i = 0; i < groups; i++) {
            iz  g     = fmt12 + 16 + 12*(iz)i;
            u32 first = fontbe32(cmap, g);
            u32 last  = fontbe32(cmap, g+4);
            u32 glyph = fontbe32(cmap, g+8);
            if (g+12 > cmap.len) {
                return 0;
            } else if ((u32)cp>=first && (u32)cp<=last) {
                return glyph + ((u32)cp - first) != 0;
            }
        }
        return 0;
    }

    if (fmt4<0 || cp>0xffff) {
        return 0;
    }
    u32 segs   = fontbe16(cmap, fmt4+6) / 2;
    iz  ends   = fmt4 + 14;
    iz  starts = ends + 2*(iz)segs + 2;
    iz  deltas = starts + 2*(iz)segs;
    iz  ranges = deltas + 2*(iz)segs;
    for (u32 i = 0; i < segs; i++) {
        u32 end   = fontbe16(cmap, ends + 2*(iz)i);
        u32 start = fontbe16(cmap, starts + 2*(iz)i);
        if ((u32)cp > end) {
            continue;
        } else if ((u32)cp < start) {
            return 0;
        }
        u32 delta = fontbe16(cmap, deltas + 2*(iz)i);
        iz  roff  = ranges + 2*(iz)i;
        u32 range = fontbe16(cmap, roff);
        u32 glyph = 0;
        if (!range) {
            glyph = ((u32)cp + delta) & 0xffff;
        } else {
            u32 idx = fontbe16(cmap, roff + range + 2*(iz)((u32)cp - start));
            glyph = idx ? (idx + delta) & 0xffff : 0;
        }
        return glyph != 0;
    }
    return 0;
}

// Whether a font file's first face maps the code point to a glyph.
static b32 fontcovers(Str ttf, i32 cp)
{
    return cmapcovers(fonttable(ttf, fontface(ttf, 0), 0x636d6170), cp);  // "cmap"
}

// A face's PostScript name (its name table's ID 6), by which CoreText
// tells a collection's faces apart, or empty.
static Str fontpsname(Arena *a, Str file, iz dir)
{
    Str name = fonttable(file, dir, 0x6e616d65);  // "name"
    u32 n    = fontbe16(name, 2);
    iz  strs = fontbe16(name, 4);
    for (u32 i = 0; i < n; i++) {
        iz  rec  = 6 + 12*(iz)i;
        u32 plat = fontbe16(name, rec);
        iz  len  = fontbe16(name, rec+8);
        iz  at   = strs + fontbe16(name, rec+10);
        if (fontbe16(name, rec+6)!=6 || rec+12>name.len || at+len>name.len) {
            continue;
        }
        b32 wide = plat==0 || plat==3;  // UTF-16BE, else Mac Roman, ASCII here
        Buf b(a, len);
        for (iz j = wide; j < len; j += 1+wide) {
            putbyte(&b, name[at+j]);
        }
        return finish(&b);
    }
    return {};
}

// The scripts a system's fonts fill in, by a character of each (app.cpp,
// findfonts): Han ideographs as simplified Chinese writes them, and as
// traditional Chinese and Japanese do, which a Japanese font alone
// lacks, then Japanese kana and Korean Hangul.
static i32 const fontcjk[] = {0x95ee, 0x554f, 0x3042, 0xd55c};  // 问 問 あ 한

// A face's line height in ems: its hhea ascender less its descender, over
// its units per em. ImGui sizes a font by that height (FreeType's real
// dimensions), so fonts merged at one size differ in em by it; 0 if the
// tables are missing.
static f32 fontlineems(Str file, iz dir)
{
    Str hhea = fonttable(file, dir, 0x68686561);  // "hhea"
    Str head = fonttable(file, dir, 0x68656164);  // "head"
    i32 asc  = (i32)(fontbe16(hhea, 4) ^ 0x8000) - 0x8000;  // signed
    i32 desc = (i32)(fontbe16(hhea, 6) ^ 0x8000) - 0x8000;
    u32 upem = fontbe16(head, 18);
    return hhea.len && upem && asc>desc ? (f32)(asc - desc)/(f32)upem : 0;
}

// Whether FreeType can draw a face: TrueType or CFF outlines. Not, say,
// Apple's HVF (an hvgl table), which macOS's PingFang has.
static b32 fontdrawable(Str file, iz dir)
{
    return fonttable(file, dir, 0x676c7966).len ||  // "glyf"
           fonttable(file, dir, 0x43464620).len ||  // "CFF "
           fonttable(file, dir, 0x43464632).len;    // "CFF2"
}

// Which of fontcjk a face has, a bit each.
static u32 fontcjkmask(Str file, iz dir)
{
    Str cmap = fonttable(file, dir, 0x636d6170);
    u32 mask = 0;
    for (i32 i = 0; i < countof(fontcjk); i++) {
        mask |= cmapcovers(cmap, fontcjk[i]) ? 1u<<i : 0;
    }
    return mask;
}

// Whether prose shows the code point: Roboto, or one of its fallbacks.
static b32 fontprose(i32 cp)
{
    return fontcovers(fontroboto(), cp) || fontcovers(fontmono(), cp) ||
           fontcovers(fontsymbols(), cp) || fontcovers(fontmath(), cp) ||
           fontcovers(fontemoji(), cp);
}
