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
// fontcovers reads a TrueType file's character map (cmap formats 4 and
// 12), so that the unit tests can check the fonts against the symbols
// models commonly write (fontsymbolsused) without ImGui or FreeType. Emoji
// are not among them: they need color glyphs, out of scope.
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
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

// Prose's fallbacks after Roboto, in order: JetBrains Mono, then the
// symbol subsets, which are also code's fallbacks.
static Str fontroboto()  { return {(u8 *)robotottf, countof(robotottf)}; }
static Str fontmono()    { return {(u8 *)monottf, countof(monottf)}; }
static Str fontsymbols() { return {(u8 *)symbolsttf, countof(symbolsttf)}; }
static Str fontmath()    { return {(u8 *)mathttf, countof(mathttf)}; }

// Symbols models commonly write in prose: arrows, check marks and
// crosses, bullets, math, shapes, ballot boxes, keyboard symbols, box
// drawing, dashes and curly quotes.
static Str fontsymbolsused =
    "→ ← ↑ ↓ ↔ ⇒ ⇐ ⇔ ↵ ↳ ↻ ⟶ ✓ ✗ ✔ ✘ • … ≤ ≥ ≠ ≈ × ± ° ∞ √ ≡ ∈ ∅ "
    "▶ ▸ ▾ ▼ ▲ ◀ ● ○ ■ □ ◆ ★ ☆ ☐ ☑ ☒ ⚠ ⏳ ✎ ✂ "
    "⌘ ⇧ ⌥ ⌃ ⏎ ⌫ ⇥ ─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼ ═ ║ — – ‘ ’ “ ” « » ′ ″";

static u32 fontbe16(Str s, iz at)
{
    return at<0 || at+2>s.len ? 0 : (u32)s[at]<<8 | s[at+1];
}

static u32 fontbe32(Str s, iz at)
{
    return fontbe16(s, at)<<16 | fontbe16(s, at+2);
}

// Whether a TrueType font maps the code point to a glyph, by its cmap:
// a Unicode subtable of format 12 if any, else of format 4. Bounds are
// checked, so a damaged file covers nothing rather than reading past it.
static b32 fontcovers(Str ttf, i32 cp)
{
    iz  cmap = -1;
    u32 n    = fontbe16(ttf, 4);
    for (u32 i = 0; i < n; i++) {
        iz rec = 12 + 16*(iz)i;
        if (fontbe32(ttf, rec) == 0x636d6170) {  // "cmap"
            cmap = fontbe32(ttf, rec+8);
        }
    }
    if (cmap<0 || cmap>=ttf.len) {
        return 0;
    }

    iz fmt4 = -1, fmt12 = -1;
    u32 subs = fontbe16(ttf, cmap+2);
    for (u32 i = 0; i < subs; i++) {
        iz  rec  = cmap + 4 + 8*(iz)i;
        u32 plat = fontbe16(ttf, rec);
        u32 enc  = fontbe16(ttf, rec+2);
        iz  at   = cmap + fontbe32(ttf, rec+4);
        b32 uni  = plat==0 || (plat==3 && (enc==1 || enc==10));
        if (!uni || at>=ttf.len) {
            continue;
        }
        u32 fmt = fontbe16(ttf, at);
        fmt12 = fmt==12 && fmt12<0 ? at : fmt12;
        fmt4  = fmt==4  && fmt4<0  ? at : fmt4;
    }

    if (fmt12 >= 0) {
        u32 groups = fontbe32(ttf, fmt12+12);
        for (u32 i = 0; i < groups; i++) {
            iz  g     = fmt12 + 16 + 12*(iz)i;
            u32 first = fontbe32(ttf, g);
            u32 last  = fontbe32(ttf, g+4);
            u32 glyph = fontbe32(ttf, g+8);
            if (g+12 > ttf.len) {
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
    u32 segs   = fontbe16(ttf, fmt4+6) / 2;
    iz  ends   = fmt4 + 14;
    iz  starts = ends + 2*(iz)segs + 2;
    iz  deltas = starts + 2*(iz)segs;
    iz  ranges = deltas + 2*(iz)segs;
    for (u32 i = 0; i < segs; i++) {
        u32 end   = fontbe16(ttf, ends + 2*(iz)i);
        u32 start = fontbe16(ttf, starts + 2*(iz)i);
        if ((u32)cp > end) {
            continue;
        } else if ((u32)cp < start) {
            return 0;
        }
        u32 delta = fontbe16(ttf, deltas + 2*(iz)i);
        iz  roff  = ranges + 2*(iz)i;
        u32 range = fontbe16(ttf, roff);
        u32 glyph = 0;
        if (!range) {
            glyph = ((u32)cp + delta) & 0xffff;
        } else {
            u32 idx = fontbe16(ttf, roff + range + 2*(iz)((u32)cp - start));
            glyph = idx ? (idx + delta) & 0xffff : 0;
        }
        return glyph != 0;
    }
    return 0;
}

// Whether prose shows the code point: Roboto, or one of its fallbacks.
static b32 fontprose(i32 cp)
{
    return fontcovers(fontroboto(), cp) || fontcovers(fontmono(), cp) ||
           fontcovers(fontsymbols(), cp) || fontcovers(fontmath(), cp);
}
