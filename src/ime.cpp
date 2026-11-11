// Input method composition: the text an input method is composing
//
// While a Japanese or Chinese input method (or a dead key, or macOS's
// press-and-hold accents) composes, the text it has so far is not yet
// the field's: SDL reports it as it changes (gui_preedit), and the
// field draws it over itself at the caret, or in place of the
// selection, which it will replace (imedraw), underlined, the clause
// being converted thicker, until the input method commits it as typed
// text or drops it. On macOS and with Wayland's text-input protocol
// nothing else shows it: the OS draws only the candidate window, which
// ImGui's IME position moves to the clause.
//
// The composition belongs to the first field to draw it after it
// began. Another field taking the keyboard ends it, as does losing the
// window's focus or a frame where no field drew it, and then the input
// method is told to drop it too (os_guiimereset): SDL doesn't, as text
// input stops or the focus moves.
//
// While it lasts, its text and where each byte went are the focused
// field's marked text for assistive tools (a11y_imgui.cpp, axmark).
//
// This is free and unencumbered software released into the public domain.

static void gui_input(Gui *, i32 kind);

enum { PREEDIT_MAX = 1024 };

struct Preedit {
    u8   text[PREEDIT_MAX];
    i32  len;
    i32  sel0, sel1;  // the clause being converted (bytes), or the caret
    u32  owner;       // the field drawing it, 0 until one does
    b32  drawn;       // by a field this frame
    b32  reset;       // tell the input method to drop it, after the frame
    i64  serial;      // bumped by every change, for the checklist
    // Where it went this frame, each byte's top left, and its end
    f32  xs[PREEDIT_MAX+1];
    f32  ys[PREEDIT_MAX+1];
    f32  lineh;
    ImVec2 at;        // where it was to go: its line's top left
    ImVec2 imepos;    // where the candidate window was put, the clause's
};

static Str imetext(Preedit *p)
{
    return Str{p->text, p->len};
}

// Code point n of s, as a byte offset: its end past the last.
static i32 imecpbyte(Str s, i32 n)
{
    iz i = 0;
    for (i32 k = 0; k<n && i<s.len; k++) {
        utf8decode(s, &i);
    }
    return (i32)i;
}

static void imeclear(Preedit *p)
{
    if (p->len || p->owner) {
        p->len   = 0;
        p->sel0  = p->sel1 = 0;
        p->owner = 0;
        p->serial++;
    }
}

// From the platform: the input method's composition is now text, its
// clause the code points [start, start+count), or a caret at start when
// count isn't positive, or at the end when start is negative. Empty
// text ends it.
static void gui_preedit(Gui *g, Str text, i32 start, i32 count)
{
    Preedit *p = g->ime;
    if (!p) {
        return;
    }
    if (!text.len) {
        if (p->len || p->owner) {
            imeclear(p);
            gui_input(g, INPUT_TEXT);
        }
        return;
    }
    iz n = text.len;
    if (n >= PREEDIT_MAX) {
        n = PREEDIT_MAX - 1;
        while (n>0 && (text[n]&0xc0)==0x80) {
            n--;
        }
    }
    Str s = takehead(text, n);
    if (!p->len) {
        p->owner = 0;  // a new one, for whichever field draws it
    }
    for (iz i = 0; i < n; i++) {
        p->text[i] = s[i];
    }
    p->len  = (i32)n;
    p->sel0 = start<0 ? p->len : imecpbyte(s, start);
    p->sel1 = start<0 || count<=0 ? p->sel0 : imecpbyte(s, start+count);
    p->serial++;
    gui_input(g, INPUT_TEXT);
}

// Whether an input method is composing, so keys are its: the platform
// keeps from ImGui the editing keys that leak through while it does.
static b32 gui_composing(Gui *g)
{
    return g->ime && g->ime->len;
}

// Draw the composition for field owner, its line's top left at at:
// over the field, in its colors, within clip. It moves left to fit its
// line; too wide for one, it wraps onto lines below from clip's left
// edge, moved up (as far as clip's top) to keep its last line showing,
// except in a field of one line, where it stays on its line with the
// clause's end in view. With hint, a placeholder showing, the rest of
// its line is covered too. The clause's top left, for the candidate
// window, goes to imepos. Returns 0 when there is none for this field.
static b32 imedraw(Gui *g, u32 owner, ImDrawList *dl, ImFont *font, f32 size,
                   ImVec2 at, f32 lineh, ImRect clip, b32 hint, u32 bg, u32 fg,
                   ImVec2 *imepos)
{
    Preedit *p = g->ime;
    if (!p || !p->len || !font) {
        return 0;
    }
    if (p->owner && p->owner!=owner) {
        // Another field took the keyboard partway: it ends here, and in
        // the input method after the frame.
        imeclear(p);
        p->reset = 1;
        return 0;
    }
    p->owner = owner;
    p->drawn = 1;
    p->lineh = lineh;
    p->at    = at;

    ImFontBaked *baked = font->GetFontBaked(size);
    f32          scale = baked->Size>0 ? size/baked->Size : 1;
    Str          s     = imetext(p);
    f32          w     = 0;
    f32          upto  = 0;  // the clause's end, or the caret
    i32          mark  = p->sel1>p->sel0 ? p->sel1 : p->sel0;
    for (iz i = 0; i < s.len;) {
        iz b = i;
        w += baked->GetCharAdvance((ImWchar)utf8decode(s, &i)) * scale;
        upto = b<mark ? w : upto;
    }
    f32 left  = clip.Min.x;
    f32 right = clip.Max.x>left ? clip.Max.x : left;
    b32 rows  = clip.Max.y-clip.Min.y >= 2*lineh;
    b32 wrap  = rows && w>right-left;
    f32 x     = at.x;
    if (w <= right-left) {
        x = x+w>right ? right-w : x;
        x = x<left ? left : x;
    } else if (wrap) {
        x = left;
    } else {
        x = right-upto<left ? right-upto : left;
    }
    f32 y = at.y;
    for (iz i = 0; i < s.len;) {
        iz  b   = i;
        f32 adv = baked->GetCharAdvance((ImWchar)utf8decode(s, &i)) * scale;
        if (wrap && x+adv>right && x>left) {
            x  = left;
            y += lineh;
        }
        for (iz k = b; k < i; k++) {
            p->xs[k] = x;
            p->ys[k] = y;
        }
        x += adv;
    }
    p->xs[s.len] = x;
    p->ys[s.len] = y;
    f32 over = y + lineh - clip.Max.y;
    f32 room = at.y - clip.Min.y;
    f32 up   = over<room ? over : room;
    if (wrap && up>0) {
        for (iz k = 0; k <= s.len; k++) {
            p->ys[k] -= up;
        }
    }

    // Each character over what is under it, underlined: the clause
    // twice as thick, else a caret where the input method has it.
    ImGuiStyle &st    = ImGui::GetStyle();
    u32         under = ImGui::GetColorU32(ImGuiCol_WindowBg);
    u32         caret = ImGui::GetColorU32(ImGuiCol_InputTextCursor);
    f32         ty    = (lineh - size) / 2;
    dl->PushClipRect(ImVec2(clip.Min.x, -1e30f), ImVec2(clip.Max.x, 1e30f), true);
    if (hint) {
        f32 y0 = p->ys[0];
        dl->AddRectFilled(ImVec2(left, y0), ImVec2(right, y0+lineh), under);
        dl->AddRectFilled(ImVec2(left, y0), ImVec2(right, y0+lineh), bg);
    }
    for (iz i = 0; i < s.len;) {
        iz  b  = i;
        i32 cp = utf8decode(s, &i);
        f32 x0 = p->xs[b];
        f32 y0 = p->ys[b];
        f32 x1 = x0 + baked->GetCharAdvance((ImWchar)cp)*scale;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0+lineh), under);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0+lineh), bg);
        dl->AddText(font, size, ImVec2(x0, y0+ty), fg,
                    (char const *)p->text + b, (char const *)p->text + i);
        b32 clause = p->sel1>p->sel0 && b>=p->sel0 && b<p->sel1;
        f32 thick  = clause ? 2.0f : 1.0f;
        if (x1-x0 > 2) {  // a gap between characters' lines
            dl->AddRectFilled(ImVec2(x0+1, y0+lineh-thick), ImVec2(x1-1, y0+lineh), fg);
        }
    }
    if (p->sel0 == p->sel1) {
        f32 cx = p->xs[p->sel0];
        f32 cy = p->ys[p->sel0];
        dl->AddLine(ImVec2(cx, cy+1), ImVec2(cx, cy+lineh-1), caret, st.InputTextCursorSize);
    }
    dl->PopClipRect();

    *imepos   = ImVec2(p->xs[p->sel0] - 1, p->ys[p->sel0]);
    p->imepos = *imepos;
    return 1;
}

// After the frame: a composition no field drew is over, and the input
// method is told so.
static void imeendframe(Gui *g)
{
    Preedit *p = g->ime;
    if (!p) {
        return;
    }
    if (p->len && !p->drawn) {
        imeclear(p);
        p->reset = 1;
    }
    p->drawn = 0;
    if (p->reset) {
        p->reset = 0;
        os_guiimereset(g->os);
    }
}

// The patched InputTextEx's side (imconfig.h): whether to ask, and the
// composition drawn over the active field.
extern "C" int prospero_preediting(void)
{
    ImGuiContext *c = ImGui::GetCurrentContext();
    Gui          *g = c ? (Gui *)c->IO.UserData : 0;
    return g && gui_composing(g);
}

extern "C" int prospero_preedit(ProsperoPreedit *e)
{
    ImGuiContext *c = ImGui::GetCurrentContext();
    Gui          *g = c ? (Gui *)c->IO.UserData : 0;
    if (!g) {
        return 0;
    }
    ImVec2 pos;
    ImRect clip(e->clip[0], e->clip[1], e->clip[2], e->clip[3]);
    if (!imedraw(g, e->id, ImGui::GetWindowDrawList(), c->Font, c->FontSize,
                 ImVec2(e->x, e->y), c->FontSize, clip, e->hint,
                 ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetColorU32(ImGuiCol_Text), &pos)) {
        return 0;
    }
    e->imex = pos.x;
    e->imey = pos.y;
    return 1;
}
