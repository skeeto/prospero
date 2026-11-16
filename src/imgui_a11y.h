// Accessibility hooks inside InputTextEx
//
// Compiled only into the patched copy of imgui_widgets.cpp that
// cmake/FetchGui.cmake makes, which includes this just ahead of
// InputTextEx, so these see its static helpers: stb_textedit's undo
// records, the input filter, and the line index. InputTextEx calls the
// accessibility hooks only while the context has TestEngineHookItems
// set, which a11y_imgui.cpp turns on with the rest of its hooks. The two
// halves are prospero_axedit and prospero_axfield (imconfig.h). The
// others (selection color, composition, word drags) always run.
//
// This is free and unencumbered software released into the public domain.

// Apply the edits an assistive tool queued for the active field, as
// InputTextEx applies a paste: filtered by the field's flags and
// callback, so a filter sees each character, and each edit one undo
// record. InputTextEx's writeback and callbacks then see the result as
// they see any edit.
static void prospero_axinput(ImGuiContext &g, ImGuiInputTextState *state, ImGuiID id,
                             ImGuiInputTextCallback callback, void *user)
{
    ProsperoAxEdit e;
    for (int n = 0; n<64 && prospero_axedit(id, state->TextA.Data, state->TextLen, &e); n++) {
        int len = state->TextLen;
        if (e.lo < 0) {
            state->SetSelection(ImClamp(e.anchor, 0, len), ImClamp(e.caret, 0, len));
        } else {
            int lo = ImClamp(e.lo, 0, len);
            int hi = ImClamp(e.hi, lo, len);

            ImVector<char> f;
            f.reserve(e.len + 1);
            for (char const *s = e.text, *end = e.text+e.len; s < end;) {
                unsigned int c;
                int          n = ImTextCharFromUtf8(&c, s, end);
                s += n>0 ? n : 1;
                if (InputTextFilterCharacter(&g, state, &c, callback, user, true)) {
                    char u[5];
                    int  ulen = ImTextCharToUtf8(u, c);
                    for (int i = 0; i < ulen; i++) {
                        f.push_back(u[i]);
                    }
                }
            }

            int del = hi - lo;
            int add = f.Size;
            if (!(state->Flags & ImGuiInputTextFlags_CallbackResize)) {
                int avail = state->BufCapacity - 1 - (len - del);
                avail = avail<0 ? 0 : avail;
                if (add > avail) {
                    char const *cut = ImTextFindValidUtf8CodepointEnd(f.Data, f.Data+add, f.Data+avail);
                    add = (int)(cut - f.Data);
                }
            }
            if (del || add) {
                ImStb::stb_text_makeundo_replace(state, state->Stb, lo, del, add);
                if (del) {
                    ImStb::STB_TEXTEDIT_DELETECHARS(state, lo, del);
                }
                if (add) {
                    add = ImStb::STB_TEXTEDIT_INSERTCHARS(state, lo, f.Data, add);
                }
            }

            // The selection was given for the text unfiltered, so what
            // was at or past the replacement's end moves with its end,
            // and none is left within a character.
            int shift = add - e.len;
            auto fix = [&](int p) {
                p = p>=lo+e.len && p>lo ? p+shift : p>lo+add ? lo+add : p;
                p = ImClamp(p, 0, state->TextLen);
                while (p>0 && p<state->TextLen && (state->TextA[p]&0xc0)==0x80) {
                    p--;
                }
                return p;
            };
            state->SetSelection(fix(e.anchor), fix(e.caret));
        }
        state->Stb->has_preferred_x = 0;
        state->CursorFollow = true;
        state->CursorAnimReset();
    }
}

// Tell where the field drew its text, just before InputTextEx draws the
// caret: the text and its origin (scrolled) and clip, and for the active
// field the start of every line and the x of each byte of the lines
// that show. The active field's lines are kept while its text and wrap
// width are unchanged.
static void prospero_axdraw(ImGuiContext &g, ImGuiInputTextState *state, ImGuiID id,
                            ImGuiInputTextFlags flags, bool password, char const *hint,
                            char const *buf, char const *end, ImVec2 origin, ImRect clip,
                            float wrap)
{
    static ImGuiTextIndex  lines;
    static ImVector<float> xs;
    static ImGuiID         lineid;
    static int             linelen = -1;
    static float           linewrap;
    static ImU64           linehash;
    static ImFont         *linefont;
    static float           linesize;

    ProsperoAxField f = {};
    f.id      = id;
    f.flags   = flags;
    f.active  = g.ActiveId==id && state && state->ID==id;
    bool showing = hint && buf==hint;
    f.hint    = hint;
    f.text    = password ? NULL : showing ? "" : buf;
    f.len     = password || showing ? 0 : (int)(end - buf);
    f.x       = origin.x;
    f.y       = origin.y;
    f.clip[0] = clip.Min.x;
    f.clip[1] = clip.Min.y;
    f.clip[2] = clip.Max.x;
    f.clip[3] = clip.Max.y;
    f.lineh   = g.FontSize;
    f.caretline = -1;

    // Past a megabyte the active field is too long to describe each
    // frame, and while its lines show, only so many bytes of them.
    enum { MAXTEXT = 1<<20, MAXXS = 1<<16 };
    f.toolong = f.active && f.text && f.len>MAXTEXT;
    if (f.active && f.text && !f.toolong) {
        ImU64 h = 0x100;
        for (int i = 0; i < f.len; i++) {
            h ^= (unsigned char)buf[i];
            h *= 1111111111111111111u;
        }
        if (id!=lineid || f.len!=linelen || wrap!=linewrap || h!=linehash ||
                g.Font!=linefont || g.FontSize!=linesize) {
            lines.Offsets.resize(0);
            InputTextLineIndexBuild(flags, &lines, buf, buf+f.len, wrap, INT_MAX, NULL);
            lines.EndOffset = f.len;
            lineid   = id;
            linelen  = f.len;
            linewrap = wrap;
            linehash = h;
            linefont = g.Font;
            linesize = g.FontSize;
        }
        f.lines  = lines.Offsets.Data;
        f.nlines = lines.Offsets.Size;

        int n0 = (int)ImFloor((clip.Min.y - origin.y) / g.FontSize);
        int n1 = (int)ImCeil((clip.Max.y - origin.y) / g.FontSize);
        f.vis0 = ImClamp(n0, 0, f.nlines);
        f.vis1 = ImClamp(n1, f.vis0, f.nlines);

        xs.resize(0);
        ImFontBaked *baked = g.Font->GetFontBaked(g.FontSize);
        float        scale = g.FontSize / baked->Size;
        for (int k = f.vis0; k < f.vis1; k++) {
            int   b = f.lines[k];
            int   e = k+1<f.nlines ? f.lines[k+1] : f.len;
            float x = 0.0f;
            if (xs.Size + (e - b) + 1 > MAXXS) {
                f.vis1 = k;  // the lines after show, but go undescribed
                break;
            }
            for (char const *s = buf+b; s < buf+e;) {
                unsigned int c;
                int n = ImTextCharFromUtf8(&c, s, buf+e);
                n = n>0 ? n : 1;
                for (int i = 0; i < n; i++) {
                    xs.push_back(x);
                }
                if (c!='\n' && c!='\r') {
                    x += baked->GetCharAdvance((ImWchar)c) * scale;
                }
                s += n;
            }
            xs.push_back(x);
        }
        f.xs  = xs.Data;
        f.nxs = xs.Size;

        ImStbTexteditState *stb = state->Stb;
        f.caret  = stb->cursor;
        f.anchor = stb->select_start==stb->select_end ? stb->cursor
                 : stb->cursor==stb->select_end       ? stb->select_start
                 :                                      stb->select_end;

        // As InputTextLineIndexGetPosOffset places it: at a wrap, on the
        // line before after a move rightward.
        int        c  = f.caret;
        int const *l0 = f.lines;
        int const *l1 = f.lines + f.nlines;
        int const *it = ImLowerBound((int *)l0, (int *)l1, c);
        if (it>l0 && (it==l1 || *it!=c || (state->WrapWidth>0.0f &&
                state->LastMoveDirectionLR==ImGuiDir_Right && c>0 &&
                buf[c-1]!='\n' && buf[c-1]!=0))) {
            it--;
        }
        f.caretline = it==l0 ? 0 : (int)(it - l0);
    }
    prospero_axfield(&f);
}

// Draw an input method's composition over the active field (ime.cpp),
// where it goes: in place of the selection, or at the caret, placed as
// ImGui places the caret. Then the caret is hidden, and the IME's
// position is the composition's clause, imepos.
static bool prospero_axpreedit(ImGuiContext &g, ImGuiInputTextState *state, ImGuiID id,
                               ImGuiTextIndex *line_index, char const *buf, char const *end,
                               ImVec2 origin, ImVec2 cursor_offset, ImRect clip, bool hint,
                               ImVec2 *imepos)
{
    if (!state || state->ID!=id || !prospero_preediting()) {
        return false;
    }
    ImStbTexteditState *stb = state->Stb;
    int    lo  = ImMin(stb->select_start, stb->select_end);
    ImVec2 off = cursor_offset;
    if (stb->select_start!=stb->select_end && lo!=stb->cursor) {
        // Where the selection starts, as ImGui highlights it: the caret's
        // rule for a wrap (the line before, after a move rightward) isn't
        // for it.
        ImS8 dir = state->LastMoveDirectionLR;
        state->LastMoveDirectionLR = ImGuiDir_Left;
        off = InputTextLineIndexGetPosOffset(g, state, line_index, buf, end, lo);
        state->LastMoveDirectionLR = dir;
    }
    ImVec2 at = ImTrunc(origin + off);
    ProsperoPreedit e = {};
    e.id      = id;
    e.x       = at.x;
    e.y       = at.y - g.FontSize;
    e.clip[0] = clip.Min.x;
    e.clip[1] = clip.Min.y;
    e.clip[2] = clip.Max.x;
    e.clip[3] = clip.Max.y;
    e.hint    = hint;
    if (!prospero_preedit(&e)) {
        return false;
    }
    *imepos = ImVec2(e.imex, e.imey);
    return true;
}

// Selected text in the theme's color for it (prospero_seltext), as the
// transcript draws it, where InputTextEx draws it in its text color on
// the selection's background (#64). Over the text drawn, each line's
// selected run gets the background again, then its characters in that
// color, clipped to it, so no stroke of the first color shows at their
// edges. A color of 0 leaves the field as ImGui drew it.
static void prospero_seldraw(ImGuiContext &g, ImGuiInputTextState *state, ImDrawList *dl,
                             ImGuiTextIndex *line_index, char const *buf, char const *end,
                             int n0, int n1, ImVec2 origin, ImRect clip, bool multiline,
                             bool active)
{
    unsigned col = prospero_seltext();
    if (!col || !state || state->Stb->select_start==state->Stb->select_end) {
        return;
    }
    ImU32       bg = ImGui::GetColorU32(ImGuiCol_TextSelectedBg, active ? 1.0f : 0.6f);
    float       up = multiline ? 0.0f : -1.0f;  // as InputTextEx's
    float       dn = multiline ? 0.0f :  2.0f;
    char const *sb = buf + ImMin(state->Stb->select_start, state->Stb->select_end);
    char const *se = buf + ImMax(state->Stb->select_start, state->Stb->select_end);
    for (int n = n0; n < n1; n++) {
        char const *p   = line_index->get_line_begin(buf, n);
        char const *eol = line_index->get_line_end(buf, n);
        if (eol<end && *eol!='\n') {
            eol++;  // a wrap's blank
        }
        char const *lb = sb>p ? sb : p;
        char const *le = se<eol ? se : eol;
        if (lb >= le) {
            continue;
        }
        float  y  = origin.y + (float)n*g.FontSize;
        float  x  = origin.x + ImGui::CalcTextSize(p, lb).x;
        ImRect r(x, y + up, x + ImGui::CalcTextSize(lb, le).x, y + g.FontSize + dn);
        r.ClipWith(clip);
        if (r.Min.x>=r.Max.x || r.Min.y>=r.Max.y) {
            continue;
        }
        dl->AddRectFilled(r.Min, r.Max, bg);
        g.Font->RenderText(dl, g.FontSize, ImVec2(x, y), ImGui::GetColorU32(col),
                           ImVec4(r.Min.x, r.Min.y, r.Max.x, r.Max.y), lb, le, 0.0f,
                           ImDrawTextFlags_CpuFineClip);
    }
}

// A double-click selects a word and a triple-click a line (InputTextEx),
// and a drag from either extends the selection by whole words or lines,
// as the transcript's does, where ImGui extends it by characters (#70).
// The click keeps the unit and the word or line selected, and each drag
// after ImGui's snaps the selection out to the unit's ends, the first
// word or line always in it, whichever way the mouse goes. Lines are
// logical, with their newline, as ImGui's triple-click selects them.
// Words are as the transcript's: a run of word characters or of blanks,
// or one separator. ImGui's double-click took the separators after a
// word with it ("again," of "again, please"), so the double-click is
// redone by that rule too, so that it and the drag agree.
enum { PROSPERO_DRAG_CHAR, PROSPERO_DRAG_WORD, PROSPERO_DRAG_LINE };
static struct {
    ImGuiContext *ctx;
    ImGuiID       id;
    int           unit;
    int           lo, hi;
} prospero_drag;

// 0 blank, 1 separator, 2 word, 3 newline (no word's)
static int prospero_charclass(ImGuiInputTextState *state, int i)
{
    unsigned c = 0;
    ImTextCharFromUtf8(&c, state->TextSrc + i, state->TextSrc + state->TextLen);
    return c=='\n' ? 3 : ImCharIsBlankW(c) ? 0 : ImStb::ImCharIsSeparatorW(c) ? 1 : 2;
}

// The end of the unit holding the character before p
static int prospero_unitend(ImGuiInputTextState *state, int unit, int p)
{
    char const *s   = state->TextSrc;
    int         len = state->TextLen;
    if (p<=0 || p>=len || s[p-1]=='\n') {
        return p;
    }
    if (unit == PROSPERO_DRAG_LINE) {
        while (p<len && s[p]!='\n') p++;
        return p<len ? p+1 : p;
    }
    int cls = prospero_charclass(state, ImStb::IMSTB_TEXTEDIT_GETPREVCHARINDEX(state, p));
    while (cls!=1 && p<len && prospero_charclass(state, p)==cls) {
        p = ImStb::IMSTB_TEXTEDIT_GETNEXTCHARINDEX(state, p);
    }
    return p<len ? p : len;
}

// The start of the unit holding the character at p
static int prospero_unitstart(ImGuiInputTextState *state, int unit, int p)
{
    char const *s = state->TextSrc;
    if (p<=0 || p>=state->TextLen) {
        return p;
    }
    if (unit == PROSPERO_DRAG_LINE) {
        while (p>0 && s[p-1]!='\n') p--;
        return p;
    }
    int cls = prospero_charclass(state, p);
    while (p>0 && (cls==0 || cls==2)) {
        int q = ImStb::IMSTB_TEXTEDIT_GETPREVCHARINDEX(state, p);
        if (prospero_charclass(state, q) != cls) {
            break;
        }
        p = q;
    }
    return p;
}

// The word at the place p nearest the mouse: of the characters either
// side, the one that is a word's over a separator, over a blank. None,
// on an empty line, is an empty word there.
static void prospero_wordat(ImGuiInputTextState *state, int p, int *lo, int *hi)
{
    int len = state->TextLen;
    int at  = -1, best = -1;
    if (p > 0) {
        int q   = ImStb::IMSTB_TEXTEDIT_GETPREVCHARINDEX(state, p);
        int cls = prospero_charclass(state, q);
        if (cls != 3) {
            at = q, best = cls;
        }
    }
    if (p < len) {
        int cls = prospero_charclass(state, p);
        if (cls!=3 && cls>best) {
            at = p, best = cls;
        }
    }
    if (at < 0) {
        *lo = *hi = p;
        return;
    }
    *lo = prospero_unitstart(state, PROSPERO_DRAG_WORD, at);
    *hi = prospero_unitend(state, PROSPERO_DRAG_WORD,
                           ImStb::IMSTB_TEXTEDIT_GETNEXTCHARINDEX(state, at));
    if (*hi > len) *hi = len;
}

// After InputTextEx's mouse handling, in the active field, at the
// mouse's place in its text as InputTextEx found it
static void prospero_dragunit(ImGuiContext &g, ImGuiInputTextState *state, ImGuiID id,
                              bool hovered, float mouse_x, float mouse_y)
{
    ImGuiIO            &io  = g.IO;
    ImStbTexteditState *stb = state->Stb;
    if (io.MouseClicked[0]) {
        prospero_drag.unit = PROSPERO_DRAG_CHAR;
        if (hovered && io.MouseClickedCount[0]>=2 && !io.KeyShift &&
            !state->SelectedAllMouseLock && !(state->Flags & ImGuiInputTextFlags_Password)) {
            prospero_drag.ctx  = &g;
            prospero_drag.id   = id;
            prospero_drag.unit = (io.MouseClickedCount[0] - 2)%2 ? PROSPERO_DRAG_LINE
                                                                 : PROSPERO_DRAG_WORD;
            if (prospero_drag.unit == PROSPERO_DRAG_WORD) {
                int side = 0;
                int p    = ImStb::stb_text_locate_coord(state, mouse_x,
                                                        stb->single_line ? 0.0f : mouse_y, &side);
                int lo, hi;
                prospero_wordat(state, p, &lo, &hi);
                stb->select_start = lo;
                stb->cursor = stb->select_end = hi;
            }
            prospero_drag.lo = ImMin(stb->select_start, stb->select_end);
            prospero_drag.hi = ImMax(stb->select_start, stb->select_end);
        }
        return;
    }
    if (!prospero_drag.unit || prospero_drag.ctx!=&g || prospero_drag.id!=id) {
        return;
    }
    if (!io.MouseDown[0]) {
        prospero_drag.unit = PROSPERO_DRAG_CHAR;
        return;
    }
    if (io.MouseDelta.x==0 && io.MouseDelta.y==0) {
        return;  // ImGui dragged nothing
    }
    int unit = prospero_drag.unit;
    int lo   = ImMin(prospero_drag.lo, state->TextLen);
    int hi   = ImMin(prospero_drag.hi, state->TextLen);
    int p    = stb->cursor;
    if (p > hi) {
        stb->select_start = lo;
        stb->cursor       = prospero_unitend(state, unit, p);
    } else if (p < lo) {
        stb->select_start = hi;
        stb->cursor       = prospero_unitstart(state, unit, p);
    } else {
        stb->select_start = lo;
        stb->cursor       = hi;
    }
    stb->select_end      = stb->cursor;
    stb->has_preferred_x = 0;
}
