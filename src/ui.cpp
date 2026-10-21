// ImGui user interface: the transcript widget with its tool rows and ask
// cards, the sessions sidebar, the liveness strip and the prompt, the
// settings window, the new session and close dialogs, the dock layout,
// the metrics overlay, the theme's colors, and the scheduler's view of
// ImGui
//
// All ImDrawList code lives here, since its signatures shift between
// ImGui releases. uiscan and uideadlines read ImGui's internals after
// each frame to tell the scheduler what it is waiting on, and drawhash
// fingerprints the frame's draw data.
//
// Every color comes from the palette in effect (g->pal, theme.cpp), and
// ImGui's own from its stock style for the palette with a few replaced
// (themestyle). A change of theme recolors both at once.
//
// Nothing here may make a frame differ from the last without declaring
// why: a value drawn from the clock needs a deadline for when it next
// changes, or the staleness probe reports it. The strip's clocks and an
// ask's countdown declare theirs only while drawn, so a session out of
// sight costs no frames.
//
// This is free and unencumbered software released into the public domain.

static ImVec2 v2(f32 x, f32 y)
{
    ImVec2 r;
    r.x = x;
    r.y = y;
    return r;
}

static char *cstr0(Arena *a, Str s)  // null-terminated copy for ImGui
{
    char *r = alloc<char>(a, s.len+1);
    copybytes(r, s.data, s.len);
    return r;
}

static void text(Str s)
{
    ImGui::TextUnformatted(s.cdata, s.cdata+s.len);
}

// Push an ID made from a string, which may be empty.
static void pushstr(Str s)
{
    if (s.len) {
        ImGui::PushID(s.cdata, s.cdata+s.len);
    } else {
        ImGui::PushID("##empty");
    }
}

// Remember where the last item went, for the scripted checklist.
static void itemrect(Gui *g, i32 which)
{
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 z = ImGui::GetItemRectMax();
    g->rects[which] = {a.x, a.y, z.x, z.y};
}

static b32 checkbox(char const *label, b32 *v)
{
    bool b = *v;
    b32  r = ImGui::Checkbox(label, &b);
    *v = b;
    return r;
}


// Fonts and measurement

static f32 stylesize(Fonts *f, i32 style)
{
    f32 base = f->size;
    switch ((style & STYLE_HMASK) >> STYLE_HSHIFT) {
    case 1: return base * 1.60f;
    case 2: return base * 1.35f;
    case 3: return base * 1.17f;
    case 4: return base * 1.05f;
    }
    return base;
}

static ImFont *stylefont(Fonts *f, i32 style)
{
    b32 bold   = style&(STYLE_BOLD|STYLE_HEADER|STYLE_HMASK);
    b32 italic = style&STYLE_ITALIC;
    if (style & STYLE_CODE) {
        return bold ? f->monobold : f->mono;
    }
    return bold&&italic ? f->bolditalic : bold ? f->bold :
           italic ? f->italic : f->prose;
}

// Baked fonts per style, looked up once per frame: switching sizes
// otherwise costs a hash lookup per character. Slots are only valid in
// the frame that filled them, since ImGui may discard unused bakes.
static ImFontBaked *stylebaked(Fonts *f, i32 style)
{
    i32 key = style & (STYLE_BOLD|STYLE_ITALIC|STYLE_CODE|STYLE_HEADER|STYLE_HMASK);
    u32 i   = (u32)key*0x9e3779b1u >> 27;
    for (i32 n = 0; n < countof(f->baked); n++, i = (i+1)%countof(f->baked)) {
        BakedSlot *s = &f->baked[i];
        if (s->gen==f->gen && s->key==key) {
            return s->baked;
        } else if (s->gen != f->gen) {
            s->gen   = f->gen;
            s->key   = key;
            s->baked = stylefont(f, style)->GetFontBaked(stylesize(f, style));
            return s->baked;
        }
    }
    return stylefont(f, style)->GetFontBaked(stylesize(f, style));  // full
}

static f32 fontadvance(void *ctx, i32 style, i32 cp)
{
    Fonts *f = (Fonts *)ctx;
    return stylebaked(f, style)->GetCharAdvance((ImWchar)cp);
}

static FontMetrics fontmetrics(void *ctx, i32 style)
{
    Fonts      *f = (Fonts *)ctx;
    FontMetrics r = {};
    f32 size = stylesize(f, style);
    r.lineh  = (f32)(i32)(size*1.3f + 0.5f);
    r.ascent = (f32)(i32)(stylebaked(f, style)->Ascent + (r.lineh - size)/2 + 0.5f);
    return r;
}


// Draw-data hashing

// Hash everything that reaches the screen: sizes, vertices, indices,
// and each command's clip rectangle, texture and ranges. Per-list
// hashes, by owner window, explain a stale frame.
static u64 drawhash(ImDrawData *dd, ListHash *lists, i32 *nlists, i32 cap,
                    u64 *header)
{
    Hash64 h = {};
    f32 hdr[] = {
        dd->DisplayPos.x, dd->DisplayPos.y, dd->DisplaySize.x, dd->DisplaySize.y,
        dd->FramebufferScale.x, dd->FramebufferScale.y,
    };
    hashbytes(&h, hdr, sizeof(hdr));
    Hash64 hh = {};
    hashbytes(&hh, hdr, sizeof(hdr));
    *header = hashfinal(&hh);
    *nlists = 0;
    for (i32 i = 0; i < dd->CmdLists.Size; i++) {
        ImDrawList *dl = dd->CmdLists[i];
        Hash64 lh = {};
        hashbytes(&lh, dl->VtxBuffer.Data, (iz)dl->VtxBuffer.Size*(iz)sizeof(ImDrawVert));
        hashbytes(&lh, dl->IdxBuffer.Data, (iz)dl->IdxBuffer.Size*(iz)sizeof(ImDrawIdx));
        for (i32 c = 0; c < dl->CmdBuffer.Size; c++) {
            ImDrawCmd *cmd = &dl->CmdBuffer[c];
            hashbytes(&lh, &cmd->ClipRect, sizeof(cmd->ClipRect));
            hashword(&lh, (u64)(uz)cmd->TexRef._TexData);
            hashword(&lh, (u64)cmd->TexRef._TexID);
            hashword(&lh, (u64)cmd->ElemCount<<32 | cmd->VtxOffset);
            hashword(&lh, (u64)cmd->IdxOffset);
            hashword(&lh, (u64)(uz)cmd->UserCallback);
        }
        u64 lv = hashfinal(&lh);
        hashword(&h, lv);
        if (*nlists < cap) {
            lists[*nlists].owner = dl->_OwnerName;
            lists[*nlists].hash  = lv;
            (*nlists)++;
        }
    }
    return hashfinal(&h);
}


// The scheduler's view of ImGui

static b32 modifierkey(i32 k)
{
    return (k>=ImGuiKey_LeftCtrl && k<=ImGuiKey_RightSuper) ||
           (k>=ImGuiKey_ReservedForModCtrl && k<=ImGuiKey_ReservedForModSuper);
}

// A window's content size as the next frame will compute it from this
// one's layout, as ImGui's CalcWindowContentSizes does.
static b32 contentmoves(ImGuiWindow *w)
{
    if (w->Collapsed || w->Hidden) {
        return 0;
    }
    f32 x = w->ContentSizeExplicit.x!=0 ? w->ContentSizeExplicit.x
          : (f32)(i64)(w->DC.CursorMaxPos.x - w->DC.CursorStartPos.x);
    f32 y = w->ContentSizeExplicit.y!=0 ? w->ContentSizeExplicit.y
          : (f32)(i64)(w->DC.CursorMaxPos.y - w->DC.CursorStartPos.y);
    return x!=w->ContentSize.x || y!=w->ContentSize.y;
}

// Reasons the frame just rendered needs a successor.
static u32 uiscan()
{
    ImGuiContext &c  = *GImGui;
    ImGuiIO      &io = c.IO;
    u32 busy = 0;
    for (i32 i = 0; i < 5; i++) {
        busy |= io.MouseDown[i] ? (u32)BUSY_MOUSE : 0;
    }
    for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        if (k>=ImGuiKey_MouseLeft && k<=ImGuiKey_MouseWheelY) continue;
        if (!modifierkey(k) && ImGui::IsKeyDown((ImGuiKey)k)) {
            busy |= BUSY_KEY;
            break;
        }
    }
    if (c.DimBgRatio>0 && c.DimBgRatio<1) busy |= BUSY_DIM;
    if (c.NavWindowingTarget) busy |= BUSY_NAVWIN;
    if (c.NavWindowingTargetAnim && c.NavWindowingHighlightAlpha>0) busy |= BUSY_NAVWIN;
    if (c.DragDropActive) busy |= BUSY_DRAG;
    if (c.InputEventsQueue.Size) busy |= BUSY_QUEUE;
    if (c.DockContext.Requests.Size) busy |= BUSY_DOCK;
    for (i32 i = 0; i < c.Windows.Size; i++) {
        ImGuiWindow *w = c.Windows[i];
        if (!w->Active && !w->WasActive) continue;
        // A window measuring itself before it appears (new windows,
        // reopened popups and tooltips, a tab about to be selected) is
        // drawn next frame. Windows hidden for good (other dock tabs,
        // clipped children) and auto-fitting ones are not pending:
        // the latter change the hash as they go.
        if (w->HiddenFramesCannotSkipItems>0 || w->HiddenFramesForRenderOnly>0) {
            busy |= BUSY_HIDDEN;
        }
        if (w->ScrollTarget.x<FLT_MAX || w->ScrollTarget.y<FLT_MAX) {
            busy |= BUSY_SCROLL;
        }
        // Content grown or shrunk this frame shows in the scrollbar only
        // next frame, even when nothing visible moved now: a line added
        // below a scrolled window's bottom edge.
        if (w->Active && contentmoves(w)) {
            busy |= BUSY_LAYOUT;
        }
    }
    for (i32 i = 0; i < c.TabBars.GetMapSize(); i++) {
        ImGuiTabBar *tb = c.TabBars.TryGetMapData(i);
        if (tb && (tb->ScrollingAnim!=tb->ScrollingTarget || tb->ReorderRequestTabId)) {
            busy |= BUSY_TABS;
        }
    }
    return busy;
}

// Timers ImGui runs without any event: tooltips, the caret, the
// settings save. Declares a deadline for each pending one.
static void uideadlines(Gui *g, Sched *s)
{
    ImGuiContext &c  = *GImGui;
    ImGuiIO      &io = c.IO;
    ImGuiStyle   &st = c.Style;
    i64 now = s->now;

    // Hover delays restart whenever the mouse moves, or whenever what is
    // under it changes without moving it (a resize, a scroll): splitters
    // light up after WINDOWS_RESIZE_FROM_EDGES_FEEDBACK_TIMER, 0.04 s.
    if (io.MouseDelta.x!=0 || io.MouseDelta.y!=0 || c.HoveredId!=g->lasthover) {
        g->lastmove  = now;
        g->lasthover = c.HoveredId;
    }
    if (g->lastmove) {
        i64 slack = 20000;
        f32 delays[] = {
            0.04f,
            st.HoverStationaryDelay,
            st.HoverStationaryDelay + st.HoverDelayShort,
            st.HoverStationaryDelay + st.HoverDelayNormal,
        };
        for (iz i = 0; i < countof(delays); i++) {
            i64 t = g->lastmove + (i64)(delays[i]*1e6f) + slack;
            if (t > now) {
                schedat(s, t, i>1 ? DUE_TOOLTIP : DUE_HOVER);
            }
        }
    }

    // The caret blinks: on for 0.8 s, off for 0.4 s, after 0.3 s held on
    // following each edit. Blinking stops 10 s after the last key.
    if (c.ActiveId && c.InputTextState.ID==c.ActiveId && io.ConfigInputTextCursorBlink) {
        f32 a  = c.InputTextState.CursorAnim;
        f32 dt = 0;
        if (a < 0) {
            dt = 0.8f - a;
        } else {
            f32 r = ImFmod(a, 1.2f);
            dt = r<0.8f ? 0.8f-r : 1.2f-r;
        }
        schedat(s, now + (i64)(dt*1e6f) + 5000, DUE_CARET);
        schedat(s, g->lastkey + CARET_STOP, DUE_CARET);
    }

    if (c.SettingsDirtyTimer > 0) {
        schedat(s, now + (i64)(c.SettingsDirtyTimer*1e6f) + 1000, DUE_SETTINGS);
    }
}


// Themes

// ImGui's colors for a palette: its stock dark or light style, with the
// palette's panels, text, fields, selected rows and bars.
static void themestyle(ImGuiStyle *st, Palette *p)
{
    if (p->dark) {
        ImGui::StyleColorsDark(st);
    } else {
        ImGui::StyleColorsLight(st);
    }
    ImVec4 *c = st->Colors;
    c[ImGuiCol_WindowBg]      = ImGui::ColorConvertU32ToFloat4(p->panel);
    c[ImGuiCol_Text]          = ImGui::ColorConvertU32ToFloat4(p->paneltext);
    c[ImGuiCol_TextDisabled]  = ImGui::ColorConvertU32ToFloat4(p->paneldim);
    c[ImGuiCol_FrameBg]       = ImGui::ColorConvertU32ToFloat4(p->frame);
    c[ImGuiCol_Header]        = ImGui::ColorConvertU32ToFloat4(p->selrow);
    c[ImGuiCol_PlotHistogram] = ImGui::ColorConvertU32ToFloat4(p->bar);
}

// Put the theme in effect: the one configured, or the system's. A change
// recolors everything in one presented frame, and the window's frame
// follows where the platform can make it do so; no change, no frame.
static void applytheme(Gui *g)
{
    Palette *p = themepalette(g->themeset, g->sysdark);
    if (p == g->pal) {
        return;
    }
    g->pal = p;
    themestyle(&g->basestyle, p);  // where a new DPI scale starts
    ImGuiStyle &st = ImGui::GetStyle();
    for (i32 i = 0; i < ImGuiCol_COUNT; i++) {
        st.Colors[i] = g->basestyle.Colors[i];
    }
    g->titleok = os_guitheme(g->os, p->dark);
    g->woke |= WOKE_FORCE;
    schedforce(&g->sched);
}

// The configured theme: THEME_*, or -1 for an unknown name, which means
// the system's.
static void settheme(Gui *g, i32 theme)
{
    g->themeset = theme<0 ? THEME_SYSTEM : theme;
    applytheme(g);
}


// Transcript

// A color at a fraction of its opacity.
static u32 fade(u32 col, f32 alpha)
{
    u32 a = (u32)((f32)(col >> IM_COL32_A_SHIFT & 0xff) * alpha);
    return (col & ~IM_COL32_A_MASK) | a<<IM_COL32_A_SHIFT;
}

// A piece's color, in a code block or elsewhere.
static u32 piececolor(Palette *pal, Piece *p, b32 codeblock)
{
    if (p->hl) {
        return pal->hl[p->hl];
    } else if (codeblock) {
        return pal->codetext;
    } else if (p->style & STYLE_LINK) {
        return pal->link;
    } else if (p->style & STYLE_DIM) {
        return pal->reasoning;
    } else if (p->style & STYLE_CODE) {
        return pal->code;
    }
    return pal->text;
}

static Str rolename(i32 role)
{
    switch (role) {
    case VIEW_USER:      return "You";
    case VIEW_ASSISTANT: return "Agent";
    case VIEW_THINK:     return "Reasoning";
    case VIEW_TOOL:      return "Tool";
    case VIEW_NOTE:      return "Note";
    case VIEW_ASK:       return "Permission";
    }
    return "";
}

static u32 toolcolor(Palette *p, i32 status)
{
    switch (status) {
    case VTOOL_OK:        return p->ok;
    case VTOOL_ERROR:
    case VTOOL_TIMEOUT:
    case VTOOL_UNKNOWN:   return p->bad;
    case VTOOL_DENIED:
    case VTOOL_ASKING:    return p->amber;
    case VTOOL_RUNNING:
    case VTOOL_STREAMING: return p->active;
    }
    return p->dim;
}

static u32 livecolor(Palette *p, i32 state)
{
    switch (state) {
    case LIVE_IDLE:
    case LIVE_CLOSED:  return p->dim;
    case LIVE_ASK:
    case LIVE_BACKOFF: return p->amber;
    case LIVE_ERROR:   return p->bad;
    }
    return p->active;
}

enum : i32 {
    CH_BG,    // code and header backgrounds
    CH_SEL,   // selection highlight
    CH_TEXT,  // text, markers, rules
    CH_COUNT,
};

// Draw one placed block of a message whose content starts at origin.
static void drawblock(Gui *g, ImDrawList *dl, ImDrawListSplitter *sp,
                      Doc *d, i32 msg, Placed *p, ImVec2 origin, Range sel,
                      f32 cliptop, f32 clipbot, f32 alpha)
{
    BlockLayout *bl     = p->bl;
    DocBlock    *b      = &d->blocks[p->block];
    Piece       *pieces = d->pieces.data + b->piece0;
    f32          ox     = origin.x;
    f32          oy     = origin.y + p->y;
    Fonts       *f      = &g->fonts;
    Palette     *pal    = g->pal;
    b32          code   = b->type == DOC_CODE;

    for (iz i = 0; i < bl->decos.len; i++) {
        Deco *dc = &bl->decos[i];
        ImVec2 a = v2(ox+dc->x, oy+dc->y);
        ImVec2 z = v2(ox+dc->x+dc->w, oy+dc->y+dc->h);
        switch (dc->kind) {
        case DECO_CODEBG:
            sp->SetCurrentChannel(dl, CH_BG);
            dl->AddRectFilled(a, z, fade(pal->codebg, alpha), 4*g->scale);
            break;
        case DECO_HEADERBG:
            sp->SetCurrentChannel(dl, CH_BG);
            dl->AddRectFilled(a, z, fade(pal->headerbg, alpha));
            break;
        case DECO_QUOTE:
            sp->SetCurrentChannel(dl, CH_TEXT);
            dl->AddRectFilled(a, z, fade(pal->quote, alpha));
            break;
        case DECO_RULE:
        case DECO_GRID:
            sp->SetCurrentChannel(dl, CH_TEXT);
            dl->AddRectFilled(a, v2(z.x>a.x+1 ? z.x : a.x+1, z.y>a.y+1 ? z.y : a.y+1),
                              fade(pal->rule, alpha));
            break;
        case DECO_MARKER: {
            sp->SetCurrentChannel(dl, CH_TEXT);
            ImFontBaked *bk = stylebaked(f, 0);
            ImVec2 at = v2(ox+dc->x, oy+dc->y - bk->Ascent);
            dl->AddText(stylefont(f, 0), stylesize(f, 0), at,
                        fade(pal->marker, alpha), dc->text.cdata,
                        dc->text.cdata+dc->text.len);
        } break;
        }
    }

    b32 selected = !poseq(sel.beg, sel.end) && sel.beg.msg<=msg && msg<=sel.end.msg;
    i32 s0 = sel.beg.msg<msg ? 0 : sel.beg.off;
    i32 s1 = sel.end.msg>msg ? 0x7fffffff : sel.end.off;
    for (iz li = 0; li < bl->lines.len; li++) {
        Line *l  = &bl->lines[li];
        f32   ly = oy + l->y;
        if (ly+l->h<cliptop || ly>clipbot) {
            continue;
        }
        for (i32 k = 0; k < l->nrun; k++) {
            Run   *r  = &bl->runs[l->run0+k];
            Piece *pc = &pieces[r->piece];
            if (selected) {
                i32 rb = runbeg(pieces, r), re = runend(pieces, r);
                b32 synth = rb == re;
                if ((!synth && rb<s1 && re>s0) || (synth && rb>=s0 && rb<s1)) {
                    f32 x0 = runx(&g->measure, pieces, r, s0);
                    f32 x1 = runx(&g->measure, pieces, r, s1);
                    if (synth) {
                        x0 = r->x;
                        x1 = r->x + (r->w>0 ? r->w : 4*g->scale);
                    }
                    sp->SetCurrentChannel(dl, CH_SEL);
                    dl->AddRectFilled(v2(ox+x0, ly), v2(ox+x1, ly+l->h), pal->sel);
                }
            }
            if (r->flags & RUN_NODRAW) {
                continue;
            }
            ImFontBaked *bk  = stylebaked(f, pc->style);
            ImVec2       at  = v2(ox + r->x, ly + l->base - bk->Ascent);
            char const  *beg = pc->text.cdata + r->b0;
            char const  *end = pc->text.cdata + r->b1;
            u32          col = fade(piececolor(pal, pc, code), alpha);
            sp->SetCurrentChannel(dl, CH_TEXT);
            dl->AddText(stylefont(f, pc->style), stylesize(f, pc->style), at,
                        col, beg, end);
            if (pc->style & (STYLE_STRIKE|STYLE_LINK)) {
                f32 y = pc->style&STYLE_STRIKE ? ly + l->base - bk->Ascent*0.35f
                                               : ly + l->base + 1;
                dl->AddLine(v2(ox+r->x, y), v2(ox+r->x+r->w, y), col, 1);
            }
        }
    }
}

// Copy the selection to the clipboard.
static void copyout(Gui *g, i32 how)
{
    Arena a = g->frame;
    Str   s = copysel(g->view, &a, how);
    if (s.len) {
        ImGui::SetClipboardText(cstr0(&a, s));
    }
}

static void copymessage(Gui *g, i32 msg)
{
    Arena a = g->frame;
    ImGui::SetClipboardText(cstr0(&a, g->view->msgs[msg].text));
}

// Text drawn left to right along a line, clipped at its right end.
struct Pen {
    ImDrawList *dl;
    Fonts      *f;
    f32         x, y;
    f32         right;
    f32         size;
};

static void pen(Pen *p, Str s, i32 style, u32 col)
{
    if (!s.len || p->x>=p->right) {
        return;
    }
    ImFont *font = stylefont(p->f, style);
    ImVec4  clip = ImVec4(p->x, p->y, p->right, p->y + p->size*2);
    p->dl->AddText(font, p->size, v2(p->x, p->y), col, s.cdata, s.cdata+s.len, 0, &clip);
    p->x += font->CalcTextSizeA(p->size, 1e9f, 0, s.cdata, s.cdata+s.len).x;
}

// Milliseconds as "850 ms", "1.2 s" or "3 min 5 s".
static Str duration(Arena *a, i64 ms)
{
    Buf b(a, 32);
    if (ms < 1000) {
        print(&b, ms);
        print(&b, " ms");
    } else if (ms < 60000) {
        printfloat(&b, (f64)ms/1000, 1);
        print(&b, " s");
    } else {
        print(&b, ms/60000);
        print(&b, " min ");
        print(&b, ms/1000%60);
        print(&b, " s");
    }
    return finish(&b);
}

static Str bytesize(Arena *a, i64 n)
{
    Buf b(a, 32);
    if (n < 1024) {
        print(&b, n);
        print(&b, " B");
    } else if (n < (i64)1<<20) {
        printfloat(&b, (f64)n/1024, 1);
        print(&b, " KiB");
    } else {
        printfloat(&b, (f64)n/(1<<20), 1);
        print(&b, " MiB");
    }
    return finish(&b);
}

// Whole seconds left until an epoch-ms time, rounded up, and a deadline
// at the next change of that number, only while it is drawn.
static i64 countdown(Gui *g, i64 at, i64 nowms)
{
    i64 left = at - nowms;
    if (left <= 0) {
        return 0;
    }
    i64 secs = (left + 999) / 1000;
    i64 next = left - (secs - 1)*1000;  // ms until secs drops
    schedat(&g->sched, g->sched.now + next*1000 + 1000, DUE_CLOCK);
    return secs;
}

// A message's header line: who, or what a tool did, or an ask's state.
static void drawheader(Gui *g, Pen *p, View *v, ViewMsg *m, i64 nowms)
{
    Arena     a   = g->frame;
    ViewTool *t   = m->tool;
    b32       cl  = m->role==VIEW_TOOL || m->role==VIEW_THINK;
    f32       sz  = p->size;
    Palette  *pal = g->pal;
    if (cl) {
        // A triangle for whether the body shows
        f32    fs = ImGui::GetFontSize();
        ImVec2 at = v2(p->x, p->y + (sz - 0.7f*fs)/2);
        ImGui::RenderArrow(p->dl, at, pal->dim, m->collapsed ? ImGuiDir_Right : ImGuiDir_Down, 0.7f);
        p->x += fs;
    }
    u32 dim = fade(pal->dim, m->faded ? 0.5f : 1);
    switch (m->role) {
    case VIEW_TOOL: {
        pen(p, t->name.len ? t->name : Str("call"), STYLE_BOLD, dim);
        pen(p, "  ", 0, dim);
        if (t->status == VTOOL_STREAMING) {
            Buf b(&a, 64);
            print(&b, "writing arguments, ");
            print(&b, bytesize(&a, t->args.len));
            pen(p, finish(&b), 0, pal->active);
            break;
        }
        // The outcome first, so that a long summary cannot hide it
        pen(p, vtoolnames[t->status], STYLE_BOLD, toolcolor(pal, t->status));
        if (t->hasexit) {
            Buf b(&a, 32);
            print(&b, "  exit ");
            print(&b, (i64)t->exit);
            pen(p, finish(&b), 0, t->exit ? pal->bad : dim);
        }
        if (t->status==VTOOL_RUNNING && t->outbytes) {
            pen(p, "  ", 0, dim);
            pen(p, bytesize(&a, t->outbytes), 0, dim);
        } else if (t->status>=VTOOL_OK && (t->ms || t->status!=VTOOL_UNKNOWN)) {
            pen(p, "  ", 0, dim);
            pen(p, duration(&a, t->ms), 0, dim);
        }
        pen(p, "  ", 0, dim);
        pen(p, t->summary, STYLE_CODE, pal->summary);
    } break;
    case VIEW_THINK: {
        pen(p, "Reasoning", STYLE_BOLD, dim);
        if (m->collapsed) {
            Buf b(&a, 32);
            print(&b, "  ");
            print(&b, m->nl + 1);
            print(&b, m->nl ? Str(" lines") : Str(" line"));
            pen(p, finish(&b), 0, dim);
        }
    } break;
    case VIEW_ASK: {
        Buf b(&a, 256);
        print(&b, t->tool);
        print(&b, " ");
        print(&b, t->path);
        pen(p, "Allow ", STYLE_BOLD, pal->amber);
        pen(p, finish(&b), STYLE_CODE, pal->summary);
        pen(p, "?  ", STYLE_BOLD, pal->amber);
        if (t->answer == VASK_PENDING) {
            if (v->readonly) {
                pen(p, "waiting in another process", 0, dim);
                break;
            }
            i64 secs = countdown(g, t->deadline, nowms);
            Buf c(&a, 64);
            if (secs) {
                print(&c, "denied in ");
                print(&c, secs);
                print(&c, " s unless allowed");
            } else {
                print(&c, "denying");
            }
            pen(p, finish(&c), 0, pal->amber);
        } else {
            b32 ok = t->answer == VASK_ALLOWED;
            Buf c(&a, 256);
            print(&c, ok ? Str("allowed") : Str("denied"));
            print(&c, t->by=="user"    ? Str(" by you") :
                      t->by=="timeout" ? Str(": nobody answered in time") :
                      t->by=="steer"   ? Str(": you sent a message instead") :
                      t->by=="cancel"  ? Str(": the turn was cancelled") :
                      t->by=="policy"  ? Str(": nobody could answer") :
                      t->by=="interrupted" ? Str(": the harness stopped while asking") :
                      t->by=="stopped" ? Str(": the engine stopped") : Str(""));
            pen(p, finish(&c), STYLE_BOLD, ok ? pal->ok : pal->bad);
        }
    } break;
    default:
        pen(p, m->label.len ? m->label : rolename(m->role), STYLE_BOLD, dim);
    }
}

// An open ask's buttons, below its text.
static void drawaskbuttons(Gui *g, GuiSession *s, ViewMsg *m, b32 latest)
{
    ViewTool *t = m->tool;
    Arena     a = g->frame;
    if (ImGui::Button("Allow")) {
        guianswer(g, s, t->req, 1, {});
    }
    if (latest) itemrect(g, RECT_ALLOW);
    ImGui::SetItemTooltip("Let the %s go ahead", t->tool.len ? cstr0(&a, t->tool) : "tool");
    ImGui::SameLine();
    if (ImGui::Button("Deny")) {
        guianswer(g, s, t->req, 0, {});
    }
    if (latest) itemrect(g, RECT_DENY);
    ImGui::SetItemTooltip("The agent reads why and tries something else");
    ImGui::SameLine();
    if (ImGui::Button("Deny with note...")) {
        g->denyreq  = t->req;
        g->denysess = s;
        g->denynote[0] = 0;
        g->opendeny = 1;
    }
    ImGui::SetItemTooltip("Deny, telling the agent what to do instead");
}

// The transcript of the session shown: virtualized messages with
// selection across them, tool rows and ask cards.
static void drawtranscript(Gui *g)
{
    GuiSession *gs = g->sess ? g->sess : &g->nosession;
    View       *v  = g->view;
    f32         sc = g->scale;
    ImGuiStyle &st = ImGui::GetStyle();

    if (gs->readonly) {
        g->bannershown = 1;
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextWrapped("Read-only: another harness process has this "
                           "session open. Its changes show after it lets go "
                           "and the session is opened again.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Open again")) {
            guiopen(g, gs);
        }
        ImGui::SetItemTooltip("Take the session over, if the other process has let go");
    }
    if (g->engineerr.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        Arena a = g->frame;
        ImGui::TextWrapped("The engine is unavailable: %s", cstr0(&a, g->engineerr));
        ImGui::PopStyleColor();
    }

    // Layout comes first, before the child window begins, so that its
    // content size and scroll take effect this very frame: a scroll set
    // inside it would land a frame late and cost a second present for
    // every streamed update.
    ImVec2      avail = ImGui::GetContentRegionAvail();
    f32         pad   = 12*sc;
    f32         winh  = avail.y;
    f32         width = avail.x - st.ScrollbarSize - 2*pad - g->widthadj;
    width = (f32)(i32)(width<50*sc ? 50*sc : width);
    FontMetrics fm    = fontmetrics(&g->fonts, 0);
    f32         charw = fontadvance(&g->fonts, 0, 'n');
    f32         head  = pad/4 + fm.lineh;  // the header line
    f32         extra = head + pad*3/4;
    f32         foot  = ImGui::GetFrameHeight() + pad/2;
    if (v->extra!=extra || v->foot!=foot) {
        v->topsdirty = 1;
    }
    v->extra = extra;
    v->foot  = foot;
    v->gap   = 6*sc;

    if (width != g->lastwidth) {
        v->topsdirty = 1;
        g->lastwidth = width;
    }

    // Where the view is: last frame's scroll, or where the wheel just
    // sent it.
    ImGuiWindow *last    = gs->win;
    f32          scrolly = 0;
    b32          bottom  = gs->stick;
    if (last) {
        scrolly = last->Scroll.y;
        if (last->ScrollTarget.y < FLT_MAX) {
            scrolly = last->ScrollTarget.y;
            bottom  = 0;
        }
        scrolly = scrolly<0 ? 0 : scrolly>last->ScrollMax.y ? last->ScrollMax.y : scrolly;
    }
    viewtops(v, width, charw, fm.lineh);

    // Anchor on the first visible message, so that height corrections
    // above it do not move what is on screen.
    i32 anchor   = viewfind(v, scrolly);
    f64 anchordy = anchor>=0 ? scrolly - v->msgs[anchor].top : 0;

    // Lay out what is visible, plus a screen of margin each way.
    i32 i0 = viewfind(v, scrolly - winh);
    i32 i1 = viewfind(v, scrolly + 2*winh);
    i64 t0 = os_ticks(g->os);
    for (i32 i = i0; i>=0 && i <= i1; i++) {
        msglayout(v, i, &g->measure, width, sc);
    }
    g->layoutus += os_ticks(g->os) - t0;
    f64 total = viewtops(v, width, charw, fm.lineh);

    f64 want = scrolly;
    if (bottom) {
        want = total + pad - winh;  // the scroll range's end, so it sticks
    } else if (anchor >= 0) {
        want = v->msgs[anchor].top + anchordy;
    }
    if (g->scrollto >= 0) {
        want = g->scrollto;  // a benchmark or a drag past the edge
        g->scrollto = -1;
    }
    want = want>total+pad-winh ? total+pad-winh : want;
    want = want<0 ? 0 : want;

    ImGui::SetNextWindowContentSize(v2(0, (f32)total + pad));
    if ((f32)want != scrolly) {
        ImGui::SetNextWindowScroll(v2(-1, (f32)want));
    }
    pushstr(gs->sid);
    ImGui::BeginChild("##transcript", v2(0, 0), 0,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    gs->win = ImGui::GetCurrentWindow();
    ImVec2 wpos = ImGui::GetWindowPos();
    scrolly = ImGui::GetScrollY();
    f32 top = wpos.y;
    f32 bot = wpos.y + winh;

    // Mouse: an invisible button over the visible area, which the
    // headers' toggles and the asks' buttons, made later, overlap.
    ImGui::SetCursorScreenPos(wpos);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##text", v2(ImGui::GetWindowWidth() - st.ScrollbarSize, winh),
                           ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
    itemrect(g, RECT_TRANSCRIPT);
    ImGuiIO &io = ImGui::GetIO();
    b32 hovered = ImGui::IsItemHovered();
    b32 active  = ImGui::IsItemActive();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    // The position under the mouse, clamped into the visible messages.
    Pos at = {};
    b32 hit = 0;
    if ((hovered || active) && v->msgs.len) {
        f32 my = io.MousePos.y;
        my = my<top ? top : my>bot-1 ? bot-1 : my;
        f64 cy = scrolly + (my - wpos.y);
        i32 i  = viewfind(v, cy);
        if (i >= 0) {
            ViewMsg   *m  = &v->msgs[i];
            MsgLayout *ml = msglayout(v, i, &g->measure, width, sc);
            f32 ly = (f32)(cy - m->top - head);
            at.msg = i;
            at.off = ly<0 || m->collapsed ? 0 :
                     msghit(&g->measure, msgdoc(v, i), ml, io.MousePos.x - wpos.x - pad, ly);
            if (ly > m->h) at.off = (i32)m->text.len;
            hit = 1;
        }
    }

    if (hit && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g->menumsg = at.msg;  // the popup takes the hover from here on
    }
    if (hit && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        i32 clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
        if (io.KeyShift && v->sel.active) {
            selextend(v, at);
        } else {
            selbegin(v, at, clicks>=3 ? SEL_BLOCK : clicks==2 ? SEL_WORD : SEL_CHAR);
        }
        g->dragging = 1;
    }
    if (g->dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g->dragging = 0;
        } else if (hit) {
            selextend(v, at);
            // Past an edge: scroll toward the mouse, faster the further.
            f32 my = io.MousePos.y;
            f32 dy = my<top ? my-top : my>bot ? my-bot : 0;
            if (dy != 0) {
                f32 step = dy * 0.5f;
                step = step<-60*sc ? -60*sc : step>60*sc ? 60*sc : step;
                g->scrollto = scrolly + step;
                if (g->scrollto < 0) g->scrollto = 0;
            }
        }
    }

    if (ImGui::BeginPopupContextItem("##textmenu")) {
        b32 any = !selempty(&v->sel);
        b32 copy = ImGui::MenuItem("Copy", g->mac ? "Cmd+C" : "Ctrl+C", false, any);
        itemrect(g, RECT_COPYITEM);
        if (copy) {
            copyout(g, COPY_TEXT);
        }
        if (ImGui::MenuItem("Copy as Markdown", 0, false, any)) {
            copyout(g, COPY_MARKDOWN);
        }
        if (ImGui::MenuItem("Copy Message", 0, false, g->menumsg<v->msgs.len)) {
            copymessage(g, g->menumsg);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", g->mac ? "Cmd+A" : "Ctrl+A")) {
            selall(v);
        }
        ImGui::EndPopup();
    }

    // Drawing
    ImDrawList         *dl    = ImGui::GetWindowDrawList();
    ImDrawListSplitter *sp    = &g->splitter;
    Range               sel   = selrange(&v->sel);
    i64                 nowms = os_time(g->os);
    i32                 d0    = viewfind(v, scrolly);
    i32                 d1    = viewfind(v, scrolly + winh);
    i32                 lastask = -1;
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        if (v->msgs[i].role==VIEW_ASK && v->msgs[i].tool->answer==VASK_PENDING) {
            lastask = (i32)i;
            break;
        }
    }
    sp->Split(dl, CH_COUNT);
    for (i32 i = d0; i>=0 && i <= d1; i++) {
        ViewMsg   *m     = &v->msgs[i];
        MsgLayout *ml    = msglayout(v, i, &g->measure, width, sc);
        f32        y     = wpos.y + (f32)(m->top - scrolly);
        f32        x     = wpos.x + pad;
        f32        alpha = m->faded ? 0.45f : 1;
        f32        mfoot = msgfoot(v, m);
        ImVec2     box0  = v2(wpos.x+pad/2, y);
        ImVec2     box1  = v2(wpos.x+pad*1.5f+width, y+v->extra+m->h+mfoot);

        // Backgrounds: the user's messages, tool rows, ask cards
        sp->SetCurrentChannel(dl, CH_BG);
        if (m->role == VIEW_USER) {
            dl->AddRectFilled(box0, box1, g->pal->userbg, 6*sc);
        } else if (m->role == VIEW_TOOL) {
            dl->AddRectFilled(box0, box1, fade(g->pal->toolbg, alpha), 4*sc);
            dl->AddRectFilled(box0, v2(box0.x+3*sc, box1.y), fade(toolcolor(g->pal, m->tool->status), alpha*0.8f));
        } else if (m->role == VIEW_ASK) {
            b32 open = m->tool->answer == VASK_PENDING;
            dl->AddRectFilled(box0, box1, g->pal->askbg, 6*sc);
            dl->AddRect(box0, box1, open ? g->pal->amber : fade(g->pal->amber, 0.4f), 6*sc, 1.5f*sc);
        }

        sp->SetCurrentChannel(dl, CH_TEXT);
        Pen p = {dl, &g->fonts, x, y + pad/4, x + width, g->fonts.size*0.85f};
        drawheader(g, &p, v, m, nowms);
        ImVec2 origin = v2(x, y + head);
        if (!m->collapsed) {
            Doc *d = msgdoc(v, i);
            for (iz k = 0; k < ml->blocks.len; k++) {
                Placed *pl = &ml->blocks[k];
                if (origin.y+pl->y > bot || origin.y+pl->y+pl->bl->h < top) {
                    continue;
                }
                drawblock(g, dl, sp, d, i, pl, origin, sel, top, bot, alpha);
            }
        }

        ImGui::PushID(i);
        if (m->role==VIEW_TOOL || m->role==VIEW_THINK) {
            // The header toggles the body
            ImGui::SetCursorScreenPos(v2(x, y));
            if (ImGui::InvisibleButton("##toggle", v2(width, head))) {
                togglemsg(v, i);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }
        if (mfoot > 0) {
            ImGui::SetCursorScreenPos(v2(x, y + head + m->h + pad/4));
            drawaskbuttons(g, gs, m, i==lastask);
        }
        ImGui::PopID();
    }
    sp->Merge(dl);

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) {
            selall(v);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
            copyout(g, COPY_TEXT);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) {
            copyout(g, COPY_MARKDOWN);
        }
    }

    gs->stick = scrolly >= ImGui::GetScrollMaxY()-2 || total<=winh;
    ImGui::EndChild();
    ImGui::PopID();

    if (g->opendeny) {
        ImGui::OpenPopup("##denynote");
        g->opendeny = 0;
    }
    if (ImGui::BeginPopup("##denynote")) {
        ImGui::TextUnformatted("Deny, and tell the agent:");
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(360*sc);
        b32 go = ImGui::InputText("##note", g->denynote, FIELD_MAX,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
        go |= ImGui::Button("Deny");
        if (go) {
            Str note = {(u8 *)g->denynote, (iz)__builtin_strlen(g->denynote)};
            guianswer(g, g->denysess, g->denyreq, 0, trim(note));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}


// Liveness strip and prompt

// Esc cancels the shown session's turn, from inside the windows about
// the session while one has focus: there it goes ahead of the prompt
// box, which would otherwise take it to undo the typing, while a field
// elsewhere (Settings) keeps Esc to abandon its edit.
static void escapecancel(Gui *g)
{
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
            ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteFocused|ImGuiInputFlags_RouteOverActive)) {
        guicancel(g, g->sess);
    }
}

// What the session is doing, how fast, and how long since the model sent
// anything: one line, whose clocks ask for frames only while shown.
static void drawstrip(Gui *g)
{
    View *v  = g->view;
    f32   sc = g->scale;
    if (!g->sess) {
        ImGui::TextDisabled("No session: start one with New session.");
        return;
    }
    Arena a     = g->frame;
    i64   nowms = os_time(g->os);
    i64   now   = g->sched.now;
    b32   req   = v->state==LIVE_DISCOVER || v->state==LIVE_WAITING ||
                  v->state==LIVE_STREAMING || v->state==LIVE_ERROR;
    ImGui::PushStyleColor(ImGuiCol_Text, livecolor(g->pal, v->state));
    ImGui::TextUnformatted(cstr0(&a, v->busy && v->state==LIVE_IDLE ? Str("working")
                                                                     : livenames[v->state]));
    ImGui::PopStyleColor();

    if (v->busy && g->sess->readonly) {
        // Its records go to the other process: nothing here would see
        // the turn end, so no clock counts on forever
        ImGui::SameLine();
        ImGui::TextDisabled("in another process, as of its log when opened");
    } else if (v->busy && v->turnts) {
        // The turn's clock, once a second
        g->roclock |= g->sess->readonly;
        i64 el = nowms - v->turnts;
        el = el<0 ? 0 : el;
        schedat(&g->sched, now + (1000 - el%1000)*1000 + 1000, DUE_CLOCK);
        Buf b(&a, 32);
        print(&b, el/1000);
        print(&b, " s");
        ImGui::SameLine();
        ImGui::TextUnformatted(cstr0(&a, finish(&b)));
    }
    if (req && v->tps > 0) {
        Buf b(&a, 32);
        printfloat(&b, v->tps, 1);
        print(&b, " tok/s");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    }
    if (req && v->ptotal>0 && v->pdone<v->ptotal) {
        ImGui::SameLine();
        f32 frac = (f32)v->pdone / (f32)v->ptotal;
        Buf b(&a, 48);
        print(&b, "reading the prompt ");
        print(&b, (i64)(frac*100));
        putbyte(&b, '%');
        ImGui::ProgressBar(frac, v2(160*sc, 0), cstr0(&a, finish(&b)));
    }
    if (req && v->lastdata) {
        // Amber after AMBER_MS without data, red near the watchdog
        i64 idle = nowms - v->lastdata;
        b32 red  = v->watchat && v->watchms && nowms >= v->watchat - v->watchms/4;
        if (idle >= AMBER_MS || red) {
            g->ambershown = 1;
            schedat(&g->sched, now + (1000 - idle%1000)*1000 + 1000, DUE_CLOCK);
            Buf b(&a, 64);
            print(&b, "no data for ");
            print(&b, idle/1000);
            print(&b, " s");
            if (red && v->watchat>nowms) {
                print(&b, ", giving up in ");
                print(&b, (v->watchat - nowms + 999)/1000);
                print(&b, " s");
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, red ? g->pal->bad : g->pal->amber);
            ImGui::TextUnformatted(cstr0(&a, finish(&b)));
            ImGui::PopStyleColor();
        } else {
            schedat(&g->sched, now + (AMBER_MS - idle)*1000 + 1000, DUE_CLOCK);
        }
        if (v->watchat && v->watchms && !red) {
            i64 redat = v->watchat - v->watchms/4;
            schedat(&g->sched, now + (redat - nowms)*1000 + 1000, DUE_CLOCK);
        }
    }
    if (v->state==LIVE_BACKOFF && v->retryat) {
        g->retryshown = 1;
        i64 secs = countdown(g, v->retryat, nowms);
        Buf b(&a, 48);
        print(&b, "retrying in ");
        print(&b, secs);
        print(&b, " s");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextUnformatted(cstr0(&a, finish(&b)));
        ImGui::PopStyleColor();
    }
    if (v->asks) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextUnformatted("waiting for your answer above");
        ImGui::PopStyleColor();
    }
}

static void drawprompt(Gui *g)
{
    f32  sc   = g->scale;
    View *v   = g->view;
    b32  can  = g->sess && g->sess->view && !g->sess->readonly && !g->quitting &&
                !g->enginegone;
    drawstrip(g);
    escapecancel(g);

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap |
                                ImGuiInputTextFlags_EnterReturnsTrue |
                                ImGuiInputTextFlags_CtrlEnterForNewLine;
    f32 bw = 80*sc;
    ImVec2 size = v2(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x,
                     ImGui::GetContentRegionAvail().y);
    b32 send = ImGui::InputTextMultiline("##prompt", g->prompt, PROMPT_MAX, size, flags);
    itemrect(g, RECT_PROMPT);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::BeginDisabled(!can);
    send |= ImGui::Button(v->busy ? "Steer" : "Send", v2(bw, 0));
    ImGui::SetItemTooltip(v->busy ? "Send to the running turn: the agent reads it at its next step (Enter)"
                                  : "Send the prompt (Enter; Shift+Enter for a new line)");
    if (v->busy) {
        if (ImGui::Button("Stop", v2(bw, 0))) {
            guicancel(g, g->sess);
        }
        ImGui::SetItemTooltip("Cancel the turn, killing any command it runs (Esc)");
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (send && can) {
        Str p = {(u8 *)g->prompt, (iz)__builtin_strlen(g->prompt)};
        p = trim(p);
        if (p.len) {
            guiprompt(g, p);
            g->prompt[0] = 0;
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}


// Sessions

static Str sesstitle(GuiSession *s)
{
    Str t = s->view && s->view->title.len ? s->view->title : s->title;
    return t.len ? t : Str("(untitled)");
}

static b32 sidafter(GuiSession **a, GuiSession **b)
{
    return compare((*a)->sid, (*b)->sid) > 0;
}

static Str sessparent(GuiSession *s)
{
    return s->view && s->view->parent.len ? s->view->parent : s->parent;
}

struct SessTree {
    Map<Slice<GuiSession *>> *kids;  // by parent sid, oldest first
    Slice<GuiSession *>       out;
    Arena                    *a;
};

static void sessplace(SessTree *t, GuiSession *s, i32 depth)
{
    if (s->depth >= 0) {
        return;
    }
    s->depth = depth;
    t->out   = push(t->a, t->out, s);
    Slice<GuiSession *> *k = upsert(&t->kids, s->sid, (Arena *)0);
    for (iz i = 0; k && i<k->len && depth<8; i++) {
        sessplace(t, (*k)[i], depth+1);
    }
}

// The sessions made or opened here and listed, newest first, but each
// subagent's under its parent's, oldest first, with its depth there.
static Slice<GuiSession *> sesslist(Gui *g, Arena *a)
{
    Slice<GuiSession *> all = {};
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->view || s->listed) {
            all = push(a, all, s);
            s->depth = -1;  // not placed yet
        }
    }
    if (!all.len) {
        return all;
    }
    sort(all.data, all.len, sidafter, *a);

    SessTree t = {};
    t.a = a;
    Map<GuiSession *> *bysid = 0;
    for (iz i = 0; i < all.len; i++) {
        *upsert(&bysid, all[i]->sid, a) = all[i];
    }
    for (iz i = all.len-1; i >= 0; i--) {
        Str p = sessparent(all[i]);
        if (p.len && upsert(&bysid, p, (Arena *)0)) {
            Slice<GuiSession *> *k = upsert(&t.kids, p, a);
            *k = push(a, *k, all[i]);
        }
    }
    for (iz i = 0; i < all.len; i++) {
        Str p = sessparent(all[i]);
        if (!p.len || !upsert(&bysid, p, (Arena *)0)) {
            sessplace(&t, all[i], 0);
        }
    }
    for (iz i = 0; i < all.len; i++) {
        sessplace(&t, all[i], 0);  // in a cycle of parents
    }
    return t.out;
}

static void drawsessions(Gui *g)
{
    f32   sc = g->scale;
    Arena a  = g->frame;
    if (g->enginekind != ENGINE_FAKE) {
        if (ImGui::SmallButton("Refresh")) {
            guilist(g);
        }
        ImGui::SetItemTooltip("List the sessions again");
    }
    Slice<GuiSession *> list = sesslist(g, &a);
    b32         other = 0;  // the checklist's first session not shown
    ImDrawList *dl    = ImGui::GetWindowDrawList();
    f32         lineh = ImGui::GetTextLineHeight();
    f32         small = g->fonts.size * 0.85f;
    for (iz i = 0; i < list.len; i++) {
        GuiSession *s     = list[i];
        View       *v     = s->view;
        i32         state = v ? v->state : s->state;
        pushstr(s->sid);
        ImVec2 p = ImGui::GetCursorScreenPos();
        f32    w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##s", s==g->sess, 0, v2(0, 2*lineh + 4*sc))) {
            guiselect(g, s);
        }
        ImVec2 r0 = ImGui::GetItemRectMin();
        ImVec2 r1 = ImGui::GetItemRectMax();
        s->row = {r0.x, r0.y, r1.x, r1.y};
        if (!other && s!=g->sess) {
            itemrect(g, RECT_OTHERSESS);
            other = 1;
        }
        Str cwd = v && v->cwd.len ? v->cwd : s->cwd;
        ImGui::SetItemTooltip("%s\n%s", cstr0(&a, cwd), cstr0(&a, s->sid));
        f32 x = p.x + 4*sc + (f32)s->depth*14*sc;  // a subagent under its parent
        Pen t = {dl, &g->fonts, x, p.y + 2*sc, p.x + w, g->fonts.size};
        pen(&t, sesstitle(s), 0, g->pal->text);
        Pen b = {dl, &g->fonts, x, p.y + 2*sc + lineh, p.x + w, small};
        if (s->depth && v && v->role.len) {
            pen(&b, v->role, 0, g->pal->dim);
            pen(&b, "  ", 0, g->pal->dim);
        }
        if (s->readonly || (!v && s->locked)) {
            pen(&b, "read-only", STYLE_BOLD, g->pal->amber);
        } else if (v && v->asks) {
            pen(&b, "needs an answer", STYLE_BOLD, g->pal->amber);
        } else {
            Str name = v && v->busy && state==LIVE_IDLE ? Str("working") : livenames[state];
            pen(&b, name, STYLE_BOLD, livecolor(g->pal, state));
        }
        pen(&b, "  ", 0, g->pal->dim);
        pen(&b, cwd, 0, g->pal->dim);
        ImGui::PopID();
    }
    if (!list.len) {
        ImGui::TextDisabled("No sessions yet.");
    }
}

// The new session dialog: a working directory, typed or from those of
// earlier sessions, and a model, by default the configured one.
static void drawnew(Gui *g)
{
    f32 sc = g->scale;
    if (g->opennew) {
        ImGui::OpenPopup("New session");
        g->opennew = 0;
    }
    ImGui::SetNextWindowSize(v2(560*sc, 0));
    if (!ImGui::BeginPopupModal("New session", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena a = g->frame;
    ImGui::TextUnformatted("Working directory");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##cwd", g->newcwd, FIELD_MAX, ImGuiInputTextFlags_AutoSelectAll);
    itemrect(g, RECT_NEWCWD);

    // Recent directories, newest first
    Slice<GuiSession *> list   = sesslist(g, &a);
    Slice<Str>          recent = {};
    for (iz i = 0; i<list.len && recent.len<8; i++) {
        Str cwd = list[i]->view && list[i]->view->cwd.len ? list[i]->view->cwd : list[i]->cwd;
        b32 dup = !cwd.len;
        for (iz k = 0; !dup && k < recent.len; k++) {
            dup = recent[k] == cwd;
        }
        if (!dup) {
            recent = push(&a, recent, cwd);
        }
    }
    if (recent.len) {
        ImGui::TextDisabled("Recent");
        for (iz i = 0; i < recent.len; i++) {
            ImGui::PushID((i32)i);
            if (ImGui::Selectable(cstr0(&a, recent[i]))) {
                iz n = recent[i].len<FIELD_MAX ? recent[i].len : FIELD_MAX-1;
                copybytes(g->newcwd, recent[i].data, n);
                g->newcwd[n] = 0;
            }
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Model");
    ImGui::SetNextItemWidth(-1);
    Str dflt = {};
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (!r->kind.len && r->key=="model") {
            dflt = r->value;
        }
    }
    char const *hint = dflt.len ? cstr0(&a, concat(&a, "configured: ", dflt)) : "the model's name";
    ImGui::InputTextWithHint("##model", hint, g->newmodel, FIELD_MAX);
    if (g->newerr.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, g->newerr));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    b32 create = ImGui::Button("Create", v2(120*sc, 0));
    itemrect(g, RECT_NEWCREATE);
    if (create) {
        Str cwd   = trim({(u8 *)g->newcwd, (iz)__builtin_strlen(g->newcwd)});
        Str model = trim({(u8 *)g->newmodel, (iz)__builtin_strlen(g->newmodel)});
        guinew(g, cwd, model);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", v2(120*sc, 0))) {
        g->newerr = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Closing the window with turns running asks first.
static void drawclose(Gui *g)
{
    f32 sc = g->scale;
    if (g->closereq) {
        g->closereq = 0;
        if (!g->quitting && !running(g)) {
            startquit(g);
        } else if (!g->quitting) {
            ImGui::OpenPopup("Quit harness?");
        }
    }
    if (!ImGui::BeginPopupModal("Quit harness?", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena a = g->frame;
    if (g->quitting) {
        ImGui::TextUnformatted("Stopping the running sessions...");
    } else {
        i32 n = running(g);
        Buf b(&a, 128);
        print(&b, "Stop ");
        print(&b, (i64)n);
        print(&b, n==1 ? Str(" running session and quit") : Str(" running sessions and quit"));
        Str label = finish(&b);
        ImGui::TextUnformatted(n ? "Turns are still running. Quitting stops them, and kills their commands."
                                 : "Nothing is running any more.");
        b32 stop = ImGui::Button(cstr0(&a, n ? label : Str("Quit")));
        itemrect(g, RECT_CLOSESTOP);
        if (stop) {
            startquit(g);
        }
        ImGui::SameLine();
        b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0));
        itemrect(g, RECT_CLOSECANCEL);
        if (cancel) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}


// Settings

// "from harness.ini:12" from a path and line, or "default", which is
// "default, empty at harness.ini:12" when set empty.
static Str provenance(Arena *a, CfgRow *r)
{
    if (!r->file.len) {
        return "default";
    }
    Str name = r->file;
    for (iz i = name.len-1; i >= 0; i--) {
        if (name[i]=='/' || name[i]=='\\') {
            name = cuthead(name, i+1);
            break;
        }
    }
    Buf b(a, 64);
    print(&b, r->isdflt ? Str("default, empty at ") : Str("from "));
    print(&b, name);
    putbyte(&b, ':');
    print(&b, r->line);
    return finish(&b);
}

// A setting's row: its key, its value to edit, where it is set. Enums
// are picked from their choices; the rest are typed. The checklist's
// target row records its field's rectangle (RECT_*, or -1 for none).
static void drawcfgrow(Gui *g, CfgRow *r, i32 target)
{
    Arena a = g->frame;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(cstr0(&a, r->key));
    ImGui::SetItemTooltip("%s\n(%s, default %s)", cstr0(&a, r->desc), cstr0(&a, r->type),
                          r->dflt.len ? cstr0(&a, r->dflt) : "none");
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(r);
    if (r->choices.len) {
        // A pick goes out at once; the theme even before the file has it
        b32    theme = r->kind=="ui" && r->key=="theme";
        ImVec2 at    = ImGui::GetCursorScreenPos();
        Str    pick  = {};
        if (ImGui::BeginCombo("##v", cstr0(&a, r->value))) {
            for (iz i = 0; i < r->choices.len; i++) {
                Str c = r->choices[i];
                if (ImGui::Selectable(cstr0(&a, c), c==r->value)) {
                    pick = c;
                }
                if (theme && i<THEME_COUNT) {
                    itemrect(g, RECT_THEME0+(i32)i);
                }
            }
            ImGui::EndCombo();
        }
        if (target >= 0) {
            f32 w = ImGui::GetContentRegionAvail().x;
            g->rects[target] = {at.x, at.y, at.x+w, at.y+ImGui::GetFrameHeight()};
        }
        if (pick.len && pick!=r->value && !(r->sent && pick==r->sentval)) {
            if (theme) {
                settheme(g, themeparse(pick));
            }
            r->sent    = 1;
            r->sentval = clone(&g->cfgmem, pick);
            guiconfigset(g, r->kind, r->name, r->key, pick);
        }
    } else {
        Str hint  = r->isdflt ? r->value : Str{};
        b32 enter = ImGui::InputTextWithHint("##v", hint.len ? cstr0(&a, hint) : "", r->buf, (uz)r->cap,
                                             ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_AutoSelectAll);
        if (target >= 0) {
            itemrect(g, target);
        }
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            // Enter both returns and deactivates: send each edit once
            Str v   = trim({(u8 *)r->buf, (iz)__builtin_strlen(r->buf)});
            Str was = r->isdflt ? Str{} : r->value;
            if (v!=was && !(r->sent && v==r->sentval)) {
                r->sent    = 1;
                r->sentval = clone(&g->cfgmem, v);
                guiconfigset(g, r->kind, r->name, r->key, v.len ? v : Str{});
            }
        }
    }
    if (target>=0 && target==g->reveal) {
        ImGui::SetScrollHereY(0.5f);  // for the checklist, which aims here
        g->reveal = -1;
    }
    ImGui::PopID();
    ImGui::TableNextColumn();
    if (!r->valid) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextUnformatted("invalid: default used");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("%s", cstr0(&a, provenance(&a, r)));
    }
}

static void drawsettings(Gui *g)
{
    Arena a = g->frame;
    if (g->enginekind == ENGINE_FAKE) {
        ImGui::TextDisabled("No engine: the fake agent has no settings.");
        return;
    }
    if (ImGui::IsWindowAppearing() || (!g->cfg && !g->cfgget)) {
        guiconfigget(g);  // the file may have changed by hand
    }
    if (!g->cfg) {
        ImGui::TextDisabled("Loading...");
        return;
    }
    Json *j    = g->cfg;
    Str   path = jsonstr(jsonget(j, "path"));
    ImGui::TextDisabled("Edits go to %s", path.len ? cstr0(&a, path) : "(no file)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) {
        guiconfigget(g);
    }
    if (g->cfgstatus.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->cfgok ? g->pal->ok : g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, g->cfgstatus));
        ImGui::PopStyleColor();
    }

    // Diagnostics, with where each is
    Json *diags = jsonget(j, "diags");
    if (jsonlen(diags) && ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (Json *d = diags->child; d; d = d->next) {
            Str sev  = jsonstr(jsonget(d, "severity"));
            Buf b(&a, 256);
            print(&b, jsonstr(jsonget(d, "file")));
            putbyte(&b, ':');
            print(&b, jsonint(jsonget(d, "line"), 0));
            putbyte(&b, ':');
            print(&b, jsonint(jsonget(d, "col"), 0));
            print(&b, ": ");
            print(&b, sev);
            print(&b, ": ");
            print(&b, jsonstr(jsonget(d, "message")));
            ImGui::PushStyleColor(ImGuiCol_Text, sev=="error" ? g->pal->bad : g->pal->amber);
            ImGui::TextWrapped("%s", cstr0(&a, finish(&b)));
            ImGui::PopStyleColor();
        }
    }

    // A section per kind, a table per section
    b32 first = 1;  // the first endpoint's idle_timeout is the checklist's
    for (Json *k = jsonget(j, "kinds")->child; k; k = k->next) {
        Str kind  = jsonstr(jsonget(k, "kind"));
        b32 named = jsonint(jsonget(k, "named"), 0) != 0;
        Str desc  = jsonstr(jsonget(k, "desc"));
        Buf hb(&a, 128);
        print(&hb, kind.len ? kind : Str("general"));
        print(&hb, ": ");
        print(&hb, desc);
        ImGui::PushID(cstr0(&a, kind));
        if (!ImGui::CollapsingHeader(cstr0(&a, finish(&hb)), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PopID();
            continue;
        }
        // Each section's rows, in order of first appearance
        Slice<Str> names = {};
        for (iz i = 0; i < g->cfgrows.len; i++) {
            CfgRow *r = &g->cfgrows[i];
            b32 seen = r->kind != kind;
            for (iz n = 0; !seen && n < names.len; n++) {
                seen = names[n] == r->name;
            }
            if (!seen) {
                names = push(&a, names, r->name);
            }
        }
        for (iz n = 0; n < names.len; n++) {
            if (named) {
                Buf sb(&a, 64);
                putbyte(&sb, '[');
                print(&sb, kind);
                putbyte(&sb, ' ');
                print(&sb, names[n]);
                putbyte(&sb, ']');
                ImGui::TextUnformatted(cstr0(&a, finish(&sb)));
            }
            ImGui::PushID(cstr0(&a, names[n]));
            ImGuiTableFlags tf = ImGuiTableFlags_RowBg;
            if (ImGui::BeginTable("##t", 3, tf)) {
                // Keys and provenance as wide as they need, the rest for
                // values
                f32 keyw  = ImGui::CalcTextSize("first_byte_timeout").x;
                f32 fromw = ImGui::CalcTextSize("from harness.ini:999").x;
                ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, keyw);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("from", ImGuiTableColumnFlags_WidthFixed, fromw);
                for (iz i = 0; i < g->cfgrows.len; i++) {
                    CfgRow *r = &g->cfgrows[i];
                    if (r->kind==kind && r->name==names[n]) {
                        i32 target = -1;
                        if (first && kind=="endpoint" && r->key=="idle_timeout") {
                            target = RECT_SETTING;
                        } else if (kind=="ui" && r->key=="theme") {
                            target = RECT_THEMECOMBO;
                        } else if (r->value.len >= FIELD_MAX) {
                            target = RECT_LONGFIELD;  // more than the slack
                        }
                        drawcfgrow(g, r, target);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            first &= kind != "endpoint";
        }
        if (named) {
            // A new section: named, with its first key set
            ImGui::SetNextItemWidth(160*g->scale);
            ImGui::InputTextWithHint("##add", "name", g->addname, FIELD_MAX);
            if (kind == "endpoint") {
                itemrect(g, RECT_ADDNAME);
            }
            ImGui::SameLine();
            Buf ab(&a, 64);
            print(&ab, "Add ");
            print(&ab, kind);
            b32 add = ImGui::Button(cstr0(&a, finish(&ab)));
            if (kind == "endpoint") {
                itemrect(g, RECT_ADDBTN);
            }
            if (add) {
                Str name = trim({(u8 *)g->addname, (iz)__builtin_strlen(g->addname)});
                Str key  = kind=="endpoint" ? Str("url") :
                           kind=="model"    ? Str("reasoning") : Str("tools");
                Str val  = kind=="endpoint" ? Str("http://localhost/") :
                           kind=="model"    ? Str("reasoning_content") :
                           Str("read write edit shell");
                b32 exists = 0;
                for (iz i = 0; i < names.len; i++) {
                    exists |= names[i] == name;
                }
                if (exists) {
                    // Its first key would be overwritten: leave it be
                    Buf eb(&a, 128);
                    print(&eb, "[");
                    print(&eb, kind);
                    putbyte(&eb, ' ');
                    print(&eb, name);
                    print(&eb, "] already exists");
                    g->cfgstatus = clone(&g->perm, finish(&eb));
                    g->cfgok     = 0;
                } else if (name.len) {
                    guiconfigset(g, kind, name, key, val);
                    g->addname[0] = 0;
                }
            }
        }
        ImGui::PopID();
    }
}


// Metrics overlay and log

static void drawmetrics(Gui *g)
{
    MetricsSnap *m = &g->snap;
    Arena        a = g->frame;
    Buf          b(&a, 256);

    static Str names[] = {"input", "mail", "deadline", "probe", "force", "pace", "other"};
    print(&b, "wakes/s ");
    for (i32 i = 0; i < WAKE_COUNT; i++) {
        print(&b, " ");
        print(&b, names[i]);
        putbyte(&b, ' ');
        print(&b, m->rate.wakes[i]);
    }
    text(finish(&b));

    b.len = 0;
    print(&b, "frames/s ");
    print(&b, m->rate.frames);
    print(&b, "  presents/s ");
    print(&b, m->rate.presents);
    text(finish(&b));

    b.len = 0;
    print(&b, "frame ");
    print(&b, m->frameus);
    print(&b, " us  max ");
    print(&b, m->maxframeus);
    print(&b, " us  wake max ");
    print(&b, m->maxwakeus);
    print(&b, " us  layout ");
    print(&b, m->layoutus);
    print(&b, " us");
    text(finish(&b));

    b.len = 0;
    print(&b, "totals: frames ");
    print(&b, m->frames);
    print(&b, "  presents ");
    print(&b, m->presents);
    print(&b, "  wakes ");
    print(&b, m->wakes);
    text(finish(&b));

    b.len = 0;
    print(&b, "next deadline: ");
    if (m->deadline == I64MAX) {
        print(&b, "none");
    } else {
        printfloat(&b, (f64)m->deadline/1000, 0);
        print(&b, " ms (");
        print(&b, duenames[m->duewhy]);
        putbyte(&b, ')');
    }
    text(finish(&b));

    b.len = 0;
    print(&b, "owed ");
    print(&b, (i64)m->owed);
    print(&b, "  changing ");
    print(&b, (i64)m->changing);
    print(&b, "  busy");
    for (i32 i = 0; i < BUSY_COUNT; i++) {
        if (m->busy & (1u<<i)) {
            putbyte(&b, ' ');
            print(&b, busynames[i]);
        }
    }
    text(finish(&b));

    b.len = 0;
    print(&b, "stale ");
    print(&b, m->stale);
    print(&b, "  runaway ");
    print(&b, m->runaway);
    if (m->stale) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->stale);
    }
    text(finish(&b));
    if (m->stale) {
        ImGui::PopStyleColor();
    }

    b.len = 0;
    print(&b, "messages ");
    print(&b, (i64)g->view->msgs.len);
    print(&b, "  deltas ");
    print(&b, g->view->deltas);
    print(&b, "  overflows ");
    print(&b, g->overflows);
    print(&b, "  resyncs ");
    print(&b, g->resyncs);
    if (g->view->overflow) {
        print(&b, "  OVERFLOW");
    }
    text(finish(&b));

    if (checkbox("Staleness probe", &g->sched.probing)) {
        g->sched.shadowat = 0;
    }
    ImGui::SetItemTooltip("Render a hidden frame 250 ms after going idle, and report a missed wake if it differs");
    ImGui::SameLine();
    checkbox("Aggressive", &g->sched.aggressive);
    ImGui::SetItemTooltip("Probe every second while idle");
}

static void drawlog(Gui *g)
{
    ImGui::BeginChild("##log");
    for (iz i = 0; i < g->logs.len; i++) {
        text(g->logs[i]);
    }
    ImGui::EndChild();
}

static void drawhelp(Gui *)
{
    ImGui::TextWrapped(
        "Each session is a conversation with the agent in one working "
        "directory. Enter sends, Shift+Enter starts a new line, and "
        "sending while the agent works steers it. Esc or Stop cancels "
        "the turn. Tool rows and reasoning open and close with a click on "
        "their header. When a tool asks to leave the workspace, answer "
        "on its card before the countdown ends, or it is denied.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Drag to select across messages; double-click selects a word, "
        "triple-click a block, Shift-click extends. Right-click for copy "
        "options.");
    ImGui::Spacing();
    ImGui::BulletText("Roboto: Copyright 2011 Google (Apache 2.0)");
    ImGui::BulletText("JetBrains Mono: Copyright 2020 JetBrains (OFL 1.1)");
}


// Toolbar, dock space, and windows

static void drawfaketools(Gui *g)
{
    f32 sc = g->scale;
    b32 prompt = ImGui::Button(g->view->busy ? "Stop" : "Prompt");
    itemrect(g, RECT_PROMPTBTN);
    if (prompt) {
        if (g->view->busy) {
            fakecmd(g, FAKE_STOP, "");
        } else {
            fakecmd(g, FAKE_PROMPT, "Show me something with code and a table.");
        }
    }
    ImGui::SetItemTooltip(g->view->busy ? "Cancel the running turn"
                                        : "Send a canned prompt");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140*sc);
    if (ImGui::SliderInt("tok/s", &g->tps, 1, 2000, "%d tok/s", ImGuiSliderFlags_Logarithmic)) {
        fakeint(g, FAKE_TPS, g->tps);
    }
    ImGui::SetItemTooltip("Streaming rate of the fake agent");
    ImGui::SameLine();
    if (checkbox("Bursts", &g->bursts)) {
        fakeint(g, FAKE_BURSTS, g->bursts);
    }
    ImGui::SetItemTooltip("Deliver tokens in bursts of 25");
    ImGui::SameLine();
    if (checkbox("Pauses", &g->pauses)) {
        fakeint(g, FAKE_PAUSES, g->pauses);
    }
    ImGui::SetItemTooltip("Stall for 5 s partway through each response");
    ImGui::SameLine();
    if (ImGui::Button("Huge code")) {
        fakecmd(g, FAKE_HUGE, "");
    }
    ImGui::SetItemTooltip("Stream one response that is a single huge code block");
    ImGui::SameLine();
    b32 history = ImGui::Button("10k history");
    itemrect(g, RECT_HISTORY);
    if (history) {
        fakeint(g, FAKE_HISTORY, 10000);
    }
    ImGui::SetItemTooltip("Load 10,000 messages (about 5 MB) of history");
}

static void drawtoolbar(Gui *g)
{
    f32 sc = g->scale;
    if (g->enginekind == ENGINE_FAKE) {
        drawfaketools(g);
    } else {
        ImGui::BeginDisabled(!g->engineok || g->quitting);
        b32 add = ImGui::Button("New session");
        itemrect(g, RECT_NEWBTN);
        if (add) {
            g->opennew = 1;
            g->newerr  = {};
        }
        ImGui::SetItemTooltip("Start a session in a working directory");
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    b32 menu = ImGui::Button("Menu");
    itemrect(g, RECT_MENU);
    if (menu) {
        ImGui::OpenPopup("##menu");
    }
    ImGui::SetItemTooltip("Layout and other options");
    if (ImGui::BeginPopup("##menu")) {
        bool demo = g->showdemo;
        if (ImGui::MenuItem("Show demo window", 0, &demo)) {
            g->showdemo = demo;
        }
        if (ImGui::MenuItem("Reset layout")) {
            g->resetpending = 1;
        }
        if (g->enginekind == ENGINE_FAKE) {
            b32 modal = ImGui::MenuItem("Modal dialog...");
            itemrect(g, RECT_MODALITEM);
            if (modal) {
                g->openmodal = 1;
            }
        }
        if (ImGui::MenuItem("Quit")) {
            gui_close(g);
        }
        ImGui::EndPopup();
    }
    if (g->openmodal) {
        ImGui::OpenPopup("Modal");
        g->openmodal = 0;
    }
    if (ImGui::BeginPopupModal("Modal", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("A modal dialog, which dims the background.");
        b32 ok = ImGui::Button("OK", v2(120*sc, 0));
        itemrect(g, RECT_MODALOK);
        if (ok) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    drawnew(g);
    drawclose(g);

    if (g->engineerr.len) {
        Arena a = g->frame;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextUnformatted(cstr0(&a, g->engineerr));
        ImGui::PopStyleColor();
    } else if (g->lasterr.len) {
        Arena a = g->frame;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cstr0(&a, g->lasterr));
    }
}

static void dockwindows(Gui *g)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(v2(vp->WorkSize.x, 0));
    ImGui::Begin("##Toolbar", 0,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoSavedSettings);
    drawtoolbar(g);
    f32 toolh = ImGui::GetWindowHeight();
    ImVec2 tp = ImGui::GetWindowPos();
    g->rects[RECT_TOOLBAR] = {tp.x, tp.y, tp.x+ImGui::GetWindowWidth(), tp.y+toolh};
    ImGui::End();

    ImVec2 pos  = v2(vp->WorkPos.x, vp->WorkPos.y + toolh);
    ImVec2 size = v2(vp->WorkSize.x, vp->WorkSize.y - toolh);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##DockHost", 0,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
    ImGuiID id = ImGui::GetID("DockSpace");
    g->dockspace = id;

    // A reset waits a frame, so windows just shown again are submitted
    // before the builder places them.
    if (g->resetpending) {
        g->resetpending = 0;
        g->resetlayout  = 1;
    } else if (g->resetlayout || (g->firstlayout && !ImGui::DockBuilderGetNode(id))) {
        g->firstlayout = 0;
        g->resetlayout = 0;
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, size);
        // The first split is between the right column and the rest, of
        // which the sessions take the left
        ImGuiID left = 0, rest = 0, right = 0, center = 0;
        ImGuiID top = 0, bottom = 0, rtop = 0, rbot = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Right, 0.28f, &right, &rest);
        ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.25f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, &bottom, &top);
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.40f, &rbot, &rtop);
        ImGui::DockBuilderDockWindow("Sessions", left);
        ImGui::DockBuilderDockWindow("Help", top);
        ImGui::DockBuilderDockWindow("Transcript", top);
        ImGui::DockBuilderDockWindow("Prompt", bottom);
        ImGui::DockBuilderDockWindow("Metrics", rtop);
        ImGui::DockBuilderDockWindow("Settings", rtop);
        ImGui::DockBuilderDockWindow("Log", rbot);
        ImGui::DockBuilderFinish(id);
    }
    ImGui::DockSpace(id, v2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    if (ImGui::Begin("Sessions")) {
        escapecancel(g);
        drawsessions(g);
    }
    ImGui::End();
    // Submitted first, so that it is the tab selected in its dock node.
    if (ImGui::Begin("Transcript")) {
        escapecancel(g);
        drawtranscript(g);
    }
    ImGui::End();
    if (ImGui::Begin("Help")) {
        drawhelp(g);
    }
    ImGui::End();
    if (ImGui::Begin("Prompt")) {
        drawprompt(g);
    }
    ImGui::End();
    g->metricsshown = ImGui::Begin("Metrics");
    if (g->metricsshown) {
        drawmetrics(g);
    }
    ImGui::End();
    if (ImGui::Begin("Settings")) {
        drawsettings(g);
    }
    ImGui::End();
    if (ImGui::Begin("Log")) {
        drawlog(g);
    }
    ImGui::End();
    if (g->showdemo) {
        bool open = 1;
        ImGui::ShowDemoWindow(&open);
        g->showdemo = open;
    }
}
